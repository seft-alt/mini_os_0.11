/* ==============================================================
 * mini-os 最小内核（C 入口）
 *
 * 目标：验证引导链路 boot -> setup -> head -> C main 打通，装真实中断/异常
 * 向量，把 8254 重编程到 100Hz 并用 RTC 秒信号交叉校验频率，再用按需分页
 * 演示完整生命周期：缺页补页 -> 读写成功 -> 解除映射 -> 再访问重新补页。
 *
 * 屏幕只有 25 行（写满 25 行就会立即上滚），所以自检输出刻意压缩在 23 行内。
 * 符号命名：head.asm 以 `EXTERN main / printk / stack_start` 引用，故相关
 * 定义处用 asm label 钉死名字（见 include/printk.h、include/stack.h）。
 * ============================================================== */
#include "linux/bootparam.h"
#include "linux/exception.h"
#include "linux/idt.h"
#include "linux/intr.h"
#include "asm/io.h"
#include "linux/kernel.h"
#include "linux/page.h"
#include "linux/pgdir.h"
#include "linux/heap.h"
#include "linux/lease.h"
#include "linux/printk.h"
#include "linux/stat.h"
#include "linux/serial.h"
#include "linux/ata.h"
#include "linux/mosfs.h"
#include "linux/rtc.h"
#include "linux/clock.h"
#include "linux/tss.h"
#include "linux/stack.h"
#include "linux/task.h"
#include "linux/tune.h"
#include "linux/timer.h"
#include "linux/keyboard.h"
#include "linux/vga.h"
#include "linux/shell.h"
#include "linux/report.h"
#include "linux/demo.h"

/* 窗口之外、页目录也没建项的地址：应当落到“unhandled”分支 */
#define OUTSIDE_ADDR 0x04000000UL

/* 阶段一/二/四/五的槽位号（task_spawn 的返回值），打印 tick 账单时用 */
static int slot_u, slot_v, slot_h, slot_l;
static int slot_y1, slot_y2, slot_y3;
static int slot_s1, slot_s2, slot_s3, slot_s4;

/* 阶段三：入口为空时 task_spawn 的返回值（应当是 0） */
static unsigned long spawn_null;

/* 调用门自检的结果：门描述符里的 DPL、白名单外的写（应为 0）、白名单内的写（旧值） */
static unsigned long gate_dpl, gate_deny, gate_old;

/* ring3 自检用：两张用户页、用户栈顶、用户在 ring3 里写下的三个返回值 */
static unsigned long user_code;
static unsigned long user_stack;
static unsigned long user_esp0;
static unsigned long probe_entry;   /* 用户页里 user_probe 的位置（相对拷贝起点） */
static unsigned long ring3_ret;     /* 第一次门调用返回的旧值（应为 0x1E） */
static unsigned long ring3_over;    /* 超上限那次：应为 0 */
static unsigned long ring3_wild;    /* 野指针那次：应为 0 */
static unsigned long ring3_cpl;     /* 门里看到的调用者 CS 低两位（应为 3） */
/* 四次越权探测的错误码，每位一个数字，打印成 knl=5505：
 *   [0] 0x8000   内核 text（1MB 以内）        → 5（P|U、读）
 *   [1] 0xA00000 1MB 以上的空白恒等映射页     → 5（证明锁的范围不止 1MB）
 *   [2] 用户栈页（刚被 page_mark_user 放行）  → 0（放行的页真能读，正对照）
 *   [3] 同一张用户栈页，被 page_mark_kernel 关上之后 → 5（反向接口真的生效） */
static unsigned long ring3_knl[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
static unsigned long lease_forced;  /* 超时强制回收的累计页数（应为 1：代码页被故意忘了还） */
static unsigned long lease_revoked; /* 随任务死亡按借用表收回的累计页数（应为 3） */
static unsigned int lease_pages[5]; /* 那个“借页就死”的任务要借的 5 页 */
static int slot_lease;              /* 它的槽位号（运行期间打印用不到，留作核对） */

/* 键盘自检：真中断次数、队列深度、Shift/大写解码是否对 */
static unsigned long kbd_irqs;
static unsigned long kbd_q;
static unsigned long kbd_shift_ok;

/* 队列归属的证据（屏幕已排满，只能从内存看：`tools/kbd-owner.sh` 用 QMP xp 读）
 *   own_slot   —— attach 拿到的槽号，期望 1
 *   own_got    —— kbd_getchar_t(1) 取到的字符，期望 'a' = 0x61
 *   own_iso    —— 之后调 kbd_getchar()（槽 0）的结果，期望 -1 = 0xFFFFFFFF
 *   own_after  —— detach 之后 kbd_owner_active()，期望 0（退回共享）
 * 这几项**不放在 static** 里，就是为了 nm 能拿到地址。 */
unsigned long own_slot;
unsigned long own_got;
unsigned long own_iso;
unsigned long own_after;

/* 每条队列单独的环（`tools/kbd-lane.sh` 读这四个）：
 *   lane_peak1 —— 槽 1 那条环的历史最高水位，连喂 6 个字符后应为 4（满了）
 *   lane_drop1 —— 槽 1 那条环因满丢掉的字符数，应为 2（6 进 4）
 *   lane_q1    —— 这时槽 1 环里还有几个，应为 4
 *   lane_q0    —— 同一时刻共享环里还有几个，应为 4（**没被槽 1 挤掉**）
 * 旧方案（一条 16 格 + 标签）在这一步会得到 lane_q1=4 / lane_q0=0：
 * 先来的槽 1 把 16 格占掉，共享的字符根本进不去——这就是这轮要证明的差别。 */
unsigned long lane_peak1;
unsigned long lane_drop1;
unsigned long lane_q1;
unsigned long lane_q0;

/* 提交队列的证据（`tools/kbd-subq.sh` 读这四个）：
 *   subq_peak  —— 连压 4 条后的 pending，应为 4（容量 4 真的用得满）
 *   subq_order —— 依次取出的 4 条首字符打成一个 32 位数，应为 0x64336261（a,b,c,d）
 *   subq_full  —— 第 5 条 push 的返回值，应为 0（队满被丢）
 *   subq_drop  —— 队满丢掉的累计次数，应为 1（且**旧行没被覆盖**：顺序仍然 a,b,c,d）
 * 旧的一格槽子在这一步会得到 subq_peak=1、subq_order 只剩最后一条，
 * 也就是“谁慢了谁丢行”——这就是这轮要证明的差别。 */
unsigned long subq_peak;
unsigned long subq_order;
unsigned long subq_full;
unsigned long subq_drop;

/* 深度可配的证据（`tools/kbd-depth.sh` 读这七个）：
 *   dep_lane_depth   —— `kbd_lane_set_depth(1, 6)` 的返回值，应为 6（生效了）
 *   dep_lane_peak    —— 同一条环连喂 8 个后到过 6，应为 6（真的按新深度装）
 *   dep_lane_drop    —— 那次多出来的 2 个被丢（8 进 6），用**差值**算，不累加前面的账
 *   dep_lane_restore —— 改回 4，应为 4（不是只能放大）
 *   dep_sub_cap      —— `kbd_line_set_capacity(6)` 的返回值，应为 6
 *   dep_sub_full     —— 深度 6 时压第 7 条，应为 0（按新容量判满）
 *   dep_sub_restore  —— 改回 4，应为 4
 * 编译期把深度写死（KBD_LANE_DEF / KBD_SUBMIT_DEF）时，这套数字一个都拿不到。 */
unsigned long dep_lane_depth;
unsigned long dep_lane_peak;
unsigned long dep_lane_drop;
unsigned long dep_lane_restore;
unsigned long dep_sub_cap;
unsigned long dep_sub_full;
unsigned long dep_sub_restore;

/* shell 的证据（`tools/kbd-shell.sh` 读这一组）：
 *   sh_rc_echo / sh_out_echo_len / sh_out_echo —— `echo hello` 的返回 0、输出长度 5、内容 "hello"
 *   sh_rc_help / sh_help_lines —— `help` 返回 0、输出缓冲里有几行（>=1）
 *   sh_rc_unknown / sh_unknown_n —— `bogus` 返回 -1、未知命令计数 1
 *   sh_rc_quote / sh_quote_argc / sh_out_quote —— `echo "a b"` 返回 0、参数 2 个（引号内不切分）、输出 "a b"
 *   sh_rc_argv / sh_error_n —— `echo a b c d e` 六个参数 → 返回 -2、解析失败计数 1
 * 这五项合起来才是“行协议真的存在”：切词、引号、未知命令、超参数各咬一处。 */
unsigned long sh_rc_echo;
unsigned long sh_out_echo_len;
char sh_out_echo[16];
unsigned long sh_rc_help;
unsigned long sh_help_lines;
unsigned long sh_rc_unknown;
unsigned long sh_unknown_n;
unsigned long sh_rc_quote;
unsigned long sh_quote_argc;
unsigned long sh_out_quote_len;
char sh_out_quote[16];
unsigned long sh_rc_argv;
unsigned long sh_error_n;

/* 双页控制台的证据：`console_start` 是 CRTC 起始地址（第 1 页 = 0x800 字符格），
 * `console_chars` 是开页那一刻已经写进去的字符数（横幅长度）。 */
unsigned long console_start;
unsigned long console_chars;

/* shell 持久态的证据（`tools/shell-vars.sh` 读这一组）：
 *   sh_rc_set       —— `set who seft` 的返回（期望 0）
 *   sh_var_val_len  —— 接着 `echo $who` 的输出长度（期望 4）
 *   sh_var_val      —— 那段输出本身（期望 "seft"）
 *   sh_rc_iso / sh_var_iso_len —— 换绑到 owner 0 之后 `echo $who`：返回 0、长度 0（看不到 owner 1 的变量）
 *   sh_rc_unset / sh_var_after_len —— 回到 owner 1 执行 `unset who` 再 `echo $who`：长度 0
 *   sh_rc_full      —— 变量表塞满后 `set` 的返回（期望 -5）
 */
unsigned long sh_rc_set;
unsigned long sh_var_val_len;
char sh_var_val[16];
unsigned long sh_rc_iso;
unsigned long sh_var_iso_len;
unsigned long sh_rc_unset;
unsigned long sh_var_after_len;
unsigned long sh_rc_full;

/* 阶段五的醒来顺序，打包成一个 32 位数（每 4 位一个槽位号） */

/* RTC 校验的等待上限：400 tick = 4 秒，RTC 坏掉时不会死等 */
#define RTC_GUARD_TICKS 400UL

/* GCC 为 main 生成的 __main 桩，裸机下没有 libgcc 提供 */
void __main(void) { }

/* 停机：独立符号，便于调试器直接在该地址下断点（tools/qemu-debug.sh 用）。
 * 语义是“出事了，别再进来”——所以这里 `cli` 之后就不再开中断。 */
__attribute__((noinline)) void kernel_halt(void) __asm__("kernel_halt");
void kernel_halt(void)
{
    cpu_cli();
    for (;;)
        cpu_hlt();
}

/* 空闲循环消费掉的那一行（供调试器/测试直接读，不是给 printk 用的）：
 * 屏幕 24 行已经排满，所以“提交被谁取走、取到了什么”只能从这里看——
 * `nm` 拿到地址，QMP `xp` 直接读。 */
char line_taken_buf[79];
unsigned int line_taken_len;      /* 最近一次取走的长度（0 = 还没取过） */
unsigned int line_taken_count;    /* 一共取走了几条 */
int shell_rc;                     /* 那一行交给 shell 之后的返回值（0 = 成功） */

/* 空闲：自检跑完、系统还活着时的等事件循环。
 * 和 kernel_halt 的唯一区别就是**中断保持打开**：`hlt` 会被 IRQ 唤醒，
 * 醒来继续等下一个。键盘（IRQ1）就是在这个循环里被收进来的——
 * 若用 kernel_halt 收尾，`cli` 会让 8042 的 IRQ1 永远进不来。
 *
 * 醒来之后顺手把“回车提交的那一行”取走，**交给 shell 解释**——这就是输入行
 * 真正的消费者：取走即出队（`kbd_line_take` 的约定），解释结果落在 shell 的
 * 内存输出缓冲里（屏幕 24 行都占着，见 `shell/shell.c` 的说明）。 */
__attribute__((noinline)) void kernel_idle(void) __asm__("kernel_idle");
void kernel_idle(void)
{
    cpu_sti();
    for (;;) {
        int n;

        clock_poll();                    /* 每 300s 用 RTC 把走时拉回来；读 CMOS 不宜进中断 */
        cpu_hlt();
        n = kbd_line_take(line_taken_buf, 79);
        if (n > 0) {
            line_taken_len = (unsigned int)n;
            line_taken_count++;
            /* 这一行归谁：谁在“当前归属”就解释到谁那份 shell 状态里
             * （历史/变量互不可见）。空闲循环通常是共享槽 0。 */
            shell_bind((unsigned int)kbd_owner_active());
            shell_rc = shell_run(line_taken_buf, (unsigned int)n);
        }
    }
}

/* boot/intr.asm 里 export 的三样东西：用户态代码段、它的末尾、进 ring3 的跳板 */
extern void user_stub(void);
extern void user_stub_end(void);
extern void user_probe(void);
extern void user_probe_end(void);
extern void user_io(void);
extern void user_io_end(void);
extern void user_enter(unsigned long entry, unsigned long user_esp);

/* 裸机下没有 libc：自己拷字节。逐字节是因为源（内核 text）与目的（用户页）
 * 都是普通的平坦地址，不需要考虑对齐/重叠。 */
static void copy_bytes(unsigned long dst, unsigned long src, unsigned long n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    unsigned long i;

    for (i = 0; i < n; i++)
        d[i] = s[i];
}

/* ==============================================================
 * 调度演示用的任务体
 * ============================================================== */

int main(int argc, char **argv, char **envp) __asm__("main");
int main(int argc, char **argv, char **envp)
{
    volatile unsigned int *page0;
    volatile unsigned int *page1;
    unsigned int value0;
    unsigned int value1;
    unsigned int demand_base;
    unsigned int demand_size;
    unsigned int released;
    unsigned int cursor_hw;
    unsigned int cursor_shape;
    unsigned long rtc_a;
    unsigned long rtc_b;
    unsigned long sec_delta;
    unsigned long tick_a;
    unsigned long tick_b;
    unsigned long tick_delta;
    unsigned long deadline;
    unsigned long sw1, u_ticks, v_ticks, layout_ovf;
    int i;

    (void)argc;
    (void)argv;
    (void)envp;

    sched_tune_init();              /* 调度参数装默认值 + 挂 int 0x31 门 */
    sched_tune_gate_init();         /* GDT 第 3 项摆 386 调用门（DPL=3） */
    serial_init();                  /* COM1：早于任何 printk，启动日志才会落到串口上 */
    heap_init();                    /* 只清账，不占页——24 行自检的数字一个都不动 */
    ata_probe();                    /* 主 IDE 通道两个盘：有盘记扇区数，没盘也不拦启动 */

    vga_clear();
    vga_cursor_shape(14, 15);        /* 下划线式光标：扫描线 14..15 */

    /* 光标寄存器往返测试：搬到一个非零位置再读回，不等就报警（成功时不留痕） */
    vga_cursor_set(1234);
    if (vga_cursor_hardware() != 1234)
        printk("cursor: set/hardware mismatch\n");
    vga_cursor_set(0);

    page_init();

    rep_banner();
    rep_cpu(stack_start.esp, stack_start.ss, boot_params.ext_mem_k);

    /* 只进串口和环（printk_dev），不写显存：24 行自检屏是按行算死的。
     * 放在横幅之后，是为了让串口的第一行永远是横幅——"我起来了"这件事
     * 不该被任何设备结论抢到前面。 */
    printk_dev("[ata] m=%d s=%d sec=0x%06X\n",
               (ata_present_mask() & 1) ? 1 : 0,
               (ata_present_mask() & 2) ? 1 : 0,
               ata_sectors((ata_present_mask() & 1) ? 0 : 1));

    /* 文件系统：挂不上不算致命（这台机器可能根本没挂数据盘），
     * 只把结论记进串口/环，shell 的 fs 命令随时能问第二遍。
     * fs_mount() 必须自己先落成变量：cdecl 下参数从右往左求值，
     * 直接写 `printk_dev("...", fs_mount(), fs_blocks())` 会先打 b/i（还是 0）。 */
    {
        int fs_rc = fs_mount();

        printk_dev("[fs] rc=%d b=%d i=%d\n", fs_rc, fs_blocks(), fs_inodes());
    }

    exception_init();
    timer_init();                    /* 内含 8254 重编程到 100Hz */
    pic_unmask_irq0();
    keyboard_init();                 /* 向量 0x21 + 只把 PIC bit1 放开 */
    rep_idt();

    rep_pit(timer_hz(), timer_pit_divisor(), timer_pit_counter());

    cpu_sti();

    /* RTC 交叉校验：用"更新结束"标志对齐整秒边界，量两个边界之间的 tick 数。
     * 秒值本身在更新瞬间会撕裂，所以不能靠自己轮询秒值来定边界。 */
    rtc_clear_flags();
    deadline = timer_ticks() + RTC_GUARD_TICKS;
    while (!rtc_update_flag() && timer_ticks() < deadline)
        ;
    tick_a = timer_ticks();
    rtc_a = rtc_seconds();

    rtc_clear_flags();
    deadline = tick_a + RTC_GUARD_TICKS;
    while (!rtc_update_flag() && timer_ticks() < deadline)
        ;
    tick_b = timer_ticks();
    rtc_b = rtc_seconds();
    cpu_cli();

    sec_delta = (rtc_b + 60 - rtc_a) % 60;
    tick_delta = tick_b - tick_a;

    /* 100Hz 下一个整秒理论上是 100 tick。但 QEMU/TCG 的定时器服务有速率上限，
     * 实测 100Hz 只到 70~85（对照组：不重编程时 18.2Hz 恰好给 18，50Hz 给 49，
     * 200Hz 给 112），所以这里只断言"确实比 BIOS 默认快得多"——
     * 这恰好是本机唯一能真判的事：18Hz 与 70+Hz 分得开，分频是否生效一目了然。
     * 告警先打（成功路径不出现）：原来是“半行 + 告警插在中间”，失败时会把
     * 两段拼成一行残句；现在整行只剩一次输出。 */
    if (sec_delta != 1 || tick_delta < 40 || tick_delta > 120)
        rep_rtc_warn();

    /* RTC 与 PIT 都是“频率对不对”的证据，合一行给阶段五腾出屏幕行 */
    rep_rtc(sec_delta, tick_delta, timer_ticks(), timer_uptime_ms());

    /* 墙钟定锚：刚量完 RTC 秒边界，正好把"绝对时间 + tick"绑在一起。
     * 只在串口/环里留痕（屏幕 24 行已排满）；shell 的 `clock` / `date` 随时能问。
     * 失败不致命：没有基准时 `date` 退回直读 RTC，只是不再有"可校准的走时"。 */
    {
        int clk_rc = clock_init();

        printk_dev("[clk] rc=%d ep=%d t=0x%06X\n", clk_rc,
                   (int)clock_base_epoch(), (unsigned int)clock_base_tick());
    }

    /* 行首先读硬件光标再打印：值应当等于本行行首的字符格索引。
     * 这里本行是第 8 行（行号 7），期望 7*80 = 560 = 0x230。 */
    /* ---- 键盘（IRQ1）：门 + 桩 + 处理体 + 解码，两条路径各验一次 ----
     * ① 用 `int 0x21` 软触发**真的门**：走的是和硬件中断同一个 isr 桩，
     *    桩里读 0x60、解码、回 EOI —— 于是 irq 计数从 0 变 1。这证明的是
     *    “门 → 桩 → 处理体 → EOI”这条链是通的，而不是“我装了门所以应该通”。
     * ② 再喂 7 个扫描码给**同一个解码函数**：Shift+字母要出大写、释放码
     *    不许入队、扩展码前缀整码丢弃。队列里应当正好 3 个字符 "Hei"。 */
    __asm__ volatile("int $0x21");
    kbd_feed(0x2A);     /* Shift 按下（不出字符） */
    kbd_feed(0x23);     /* h -> H（Shift 生效） */
    kbd_feed(0xA3);     /* h 释放：不入队 */
    kbd_feed(0xAA);     /* Shift 释放：清 Shift 状态 */
    kbd_feed(0x12);     /* e */
    kbd_feed(0x17);     /* i */
    kbd_feed(0xE0);     /* 扩展码前缀：只置状态，不出字符 */
    kbd_feed(0x4B);     /* 前缀后的 Left：被当成编辑动作吃掉，也不出字符 */
    kbd_irqs = (unsigned long)kbd_irq_count();
    kbd_q = (unsigned long)kbd_queue_count();
    if (kbd_getchar() == 'H' && kbd_getchar() == 'e' && kbd_getchar() == 'i')
        kbd_shift_ok = 1;

    /* ③ 队列归属：attach 一个槽 → 切到它 → 喂一个 'a' → 只有它能取到，
     *    而槽 0 的老接口必须拿到 -1（“不顺手拿别人的”）。
     *    最后 select 回 0 再 detach，验证“还槽顺手把归属退回共享”。 */
    own_slot = (unsigned long)kbd_owner_attach();
    (void)kbd_owner_select((int)own_slot);
    kbd_feed(0x1E);                      /* a：此时归属是 own_slot */
    own_got = (unsigned long)kbd_getchar_t((int)own_slot);
    own_iso = (unsigned long)kbd_getchar();   /* 期望 -1：槽 0 取不到槽 1 的字符 */
    (void)kbd_owner_select(0);
    kbd_owner_detach((int)own_slot);
    own_after = (unsigned long)kbd_owner_active();

    /* ④ 每 owner 一条环：槽 1 连喂 6 个字符（环只有 4 格）→ 它自己装满、
     *    丢 2 个；此时切回共享再喂 4 个，共享那条环照样收得下 4 个。
     *    这正是“一个人打满不会把别人挤掉”的可测形式：
     *    如果还是老的一条大环，共享会拿到 0（16 格已被 4 个 + ... 占住的语义不同，
     *    量化差别就是 lane_q0=4 vs lane_q0=0）。 */
    own_slot = (unsigned long)kbd_owner_attach();          /* 还是槽 1 */
    (void)kbd_owner_select((int)own_slot);
    kbd_feed(0x1E);                                       /* a */
    kbd_feed(0x30);                                       /* b */
    kbd_feed(0x2E);                                       /* c */
    kbd_feed(0x20);                                       /* d */
    kbd_feed(0x12);                                       /* e：第 5 个，满 */
    kbd_feed(0x17);                                       /* i：第 6 个，满 */
    lane_peak1 = (unsigned long)kbd_lane_peak(1);
    lane_drop1 = (unsigned long)kbd_lane_drop(1);
    lane_q1 = (unsigned long)kbd_queue_count_t(1);
    (void)kbd_owner_select(0);
    kbd_feed(0x1E);                                       /* 共享环：a */
    kbd_feed(0x30);                                       /* b */
    kbd_feed(0x2E);                                       /* c */
    kbd_feed(0x20);                                       /* d */
    lane_q0 = (unsigned long)kbd_queue_count_t(0);
    /* 把两条环取干净：自检不能给别人留下字符（后面的负向/归属脚本都假定空队列） */
    while (kbd_getchar() >= 0) { }
    while (kbd_getchar_t(1) >= 0) { }
    kbd_owner_detach(1);

    /* ⑤ 提交队列（4 格 FIFO）：外部生产者压 4 条 → 必须都排着（peak=4）；
     *    取 4 条 → 顺序必须是 a,b,c,d（**旧行没被覆盖**）；再压第 5 条 → 队满，
     *    返回 0 且只丢新的这一条；最后把队列清干净，不给后面的脚本留东西。 */
    {
        char line_tmp[79];
        unsigned int i;
        static const char *const probe_lines[5] = { "a", "b", "c", "d", "e" };

        for (i = 0; i < 4; i++) {
            if (!kbd_line_push(probe_lines[i], 1))
                break;                             /* 压不进去就是容量小于 4 */
        }
        subq_peak = (unsigned long)kbd_line_pending();
        subq_full = (unsigned long)kbd_line_push(probe_lines[4], 1);   /* 队满：期望 0 */
        subq_order = 0;
        for (i = 0; i < 4; i++) {
            int n = kbd_line_take(line_tmp, 79);

            if (n <= 0)
                break;                             /* 取不出来：把缺口留成 0 */
            subq_order |= (unsigned long)(unsigned char)line_tmp[0] << (i * 8);
        }
        subq_drop = (unsigned long)kbd_line_drop();
        while (kbd_line_take(0, 0) > 0) { }         /* 清干净：自检不占用提交队列 */
    }

    /* ⑥ 深度可配（不是编译期定死）：把槽 1 的环临时调到 6 → 连喂 8 个字符，
     *    应当装 6 丢 2（peak=6、drop 差值 2）；改回 4 也生效。提交队列同样：
     *    调到 6 → 压第 7 条才被拒。最后全部恢复默认，不给后面的脚本留状态。 */
    {
        char line_tmp[79];
        unsigned int i, drop_before;
        static const char *const deep_lines[7] = {
            "u0", "u1", "u2", "u3", "u4", "u5", "u6"
        };

        drop_before = kbd_lane_drop(1);
        dep_lane_depth = (unsigned long)kbd_lane_set_depth(1, 6);   /* 期望 6 */
        (void)kbd_owner_attach();                               /* 重新领回槽 1 */
        (void)kbd_owner_select(1);
        kbd_feed(0x1E); kbd_feed(0x30); kbd_feed(0x2E); kbd_feed(0x20);
        kbd_feed(0x12); kbd_feed(0x17); kbd_feed(0x1A); kbd_feed(0x1F);  /* 8 个 */
        dep_lane_peak = (unsigned long)kbd_lane_peak(1);            /* 期望 6 */
        dep_lane_drop = (unsigned long)(kbd_lane_drop(1) - drop_before); /* 期望 2 */
        dep_lane_restore = (unsigned long)kbd_lane_set_depth(1, 4); /* 期望 4 */
        (void)kbd_owner_select(0);
        while (kbd_getchar_t(1) >= 0) { }
        kbd_owner_detach(1);                                    /* 还回槽 1 */

        dep_sub_cap = (unsigned long)kbd_line_set_capacity(6);      /* 期望 6 */
        for (i = 0; i < 6; i++) {
            if (!kbd_line_push(deep_lines[i], 2))
                break;
        }
        dep_sub_full = (unsigned long)kbd_line_push(deep_lines[6], 2); /* 第 7 条：期望 0 */
        dep_sub_restore = (unsigned long)kbd_line_set_capacity(4);   /* 期望 4 */
        while (kbd_line_take(line_tmp, 79) > 0) { }                  /* 清空（6 条） */
    }

    /* ⑦ shell：把几行固定输入喂给**真解释器**（不是打印假字符串），
     *    覆盖切词、引号、未知命令、超参数四件事；输出进内存缓冲，用 xp 读。 */
    {
        unsigned int i;
        const char *s;

        sh_rc_echo = (unsigned long)shell_run("echo hello", 10);
        sh_out_echo_len = (unsigned long)shell_out_len(0);
        s = shell_out_line(0);
        for (i = 0; s && s[i] && i < 15; i++)
            sh_out_echo[i] = s[i];
        sh_out_echo[i] = '\0';

        sh_rc_help = (unsigned long)shell_run("help", 4);
        sh_help_lines = (unsigned long)shell_out_count();

        sh_rc_unknown = (unsigned long)shell_run("bogus", 5);
        sh_unknown_n = (unsigned long)shell_unknown();

        sh_rc_quote = (unsigned long)shell_run("echo \"a b\"", 10);
        sh_quote_argc = (unsigned long)shell_last_argc();
        sh_out_quote_len = (unsigned long)shell_out_len(0);
        s = shell_out_line(0);
        for (i = 0; s && s[i] && i < 15; i++)
            sh_out_quote[i] = s[i];
        sh_out_quote[i] = '\0';

        sh_rc_argv = (unsigned long)shell_run("echo a b c d e", 14);
        sh_error_n = (unsigned long)shell_errors();
    }

    /* ⑧ shell 的持久态按 owner 分：在 owner 1 里 set 一个变量、回 owner 0 看不到、
     *    回 owner 1 又看得到；unset 之后就没值了；表塞满时 `set` 必须报 -5。 */
    {
        unsigned int i;
        const char *s;
        static const char *const fill[7] = {
            "set v0 0", "set v1 1", "set v2 2", "set v3 3",
            "set v4 4", "set v5 5", "set v6 6"
        };

        (void)shell_bind(1);
        sh_rc_set = (unsigned long)shell_run("set who seft", 12);
        (void)shell_run("echo $who", 9);                 /* 展开成 "seft" 写进输出缓冲 */
        sh_var_val_len = (unsigned long)shell_out_len(0);
        s = shell_out_line(0);
        for (i = 0; s && s[i] && i < 15; i++)
            sh_var_val[i] = s[i];
        sh_var_val[i] = '\0';

        (void)shell_bind(0);
        sh_rc_iso = (unsigned long)shell_run("echo $who", 9);
        sh_var_iso_len = (unsigned long)shell_out_len(0);

        (void)shell_bind(1);
        sh_rc_unset = (unsigned long)shell_run("unset who", 9);
        (void)shell_run("echo $who", 9);
        sh_var_after_len = (unsigned long)shell_out_len(0);
        for (i = 0; i < 7; i++)
            sh_rc_full = (unsigned long)shell_run(fill[i], 8);   /* 第 7 个：表满 */

        (void)shell_bind(0);
    }

    cursor_hw = vga_cursor_hardware();
    cursor_shape = vga_cursor_shape_read();
    /* kbd 接在光标行尾部：两者都是 drivers 模块的事，合一行正好腾出行宽。 */
    rep_vga_kbd(cursor_hw, cursor_shape, kbd_pic_mask(),
                kbd_irqs, kbd_q, kbd_shift_ok);

    /* 按需分页窗口：第一次访问必然缺页，由 #PF 补页后 iret 重试成功 */
    page_demand_window(&demand_base, &demand_size);
    rep_demand(demand_base, demand_size);

    page0 = (volatile unsigned int *)demand_base;
    *page0 = 0xA5A5C0DEu;
    value0 = *page0;

    page1 = (volatile unsigned int *)(demand_base + 4096);
    *page1 = 0x12345678u;
    value1 = *page1;

    /* 解除第一页的映射：PTE 清零 + invlpg + 物理页归还分配器。
     * 这一步和上面的读回之间没有缺页输出，所以两件事可以并成一行——
     * 屏幕只有 24 行可用（第 25 行会立即上滚），省出来的那一行给阶段七。 */
    released = page_unmap(demand_base);
    rep_read(value0, value1, demand_base, released);

    /* 再读同一地址：必然重新缺页，补页后 iret 重试；新页已清零，读到 0。
     * 注意先取值再打印，否则缺页输出会插在 printk 中间把这一行打断。 */
    value0 = *page0;
    rep_touch(value0, page_frames_used(), page_frames_total());

    /* ---- 阶段一：同优先级两个 CPU 型任务，时间片 2 tick ----
     * 两者永不 sleep，只能靠 tick 抢占；slice=2 时应当每 2 个 tick 换一次人，
     * 12 个 tick 里各占 6 个 —— 这就是“时间片按 tick 递减”的直接证据。
     * 这一行不换行，阶段二的数字接在它后面：24 行屏幕已经满了，两段账目合成
     * 一行仍然读得出来（U/V 是一组，H/L 是另一组）。 */
    task_reset();
    demo_run_begin(timer_ticks() + 12);
    slot_u = task_spawn(demo_burn, 0, 2, 0);
    slot_v = task_spawn(demo_burn, 0, 2, 0);
    demo_run_workers();
    sw1 = task_switches();                 /* 阶段一自己的切换次数，阶段二会覆盖它 */
    u_ticks = task_ticks_of(slot_u);       /* 槽位会被阶段二复用，数必须先抄下来 */
    v_ticks = task_ticks_of(slot_v);

    /* ---- 阶段二：优先级差 + 老化保底 ----
     * H(prio2, slice 2) 一直就绪，L(prio0, slice 8) 照理会被饿死；但 L 每等够
     * TASK_AGE_TICKS 就升一级，升到最高级后能在 H 的时间片边界上抢到一小段，
     * 跑完降回 prio0。结果 L 不再是 0，而是拿到“保底”的 1~2 个 tick。 */
    task_reset();
    demo_run_begin(timer_ticks() + 12);
    slot_h = task_spawn(demo_burn, 0, 2, 0);
    slot_l = task_spawn(demo_burn, 0, 0, 0);
    demo_run_workers();
    /* 两段账单合一行：24 行屏幕已经满了，U/V 是一组、H/L 是另一组 */
    rep_phase12(u_ticks, v_ticks, sw1,
                task_ticks_of(slot_h), task_ticks_of(slot_l),
                task_switches());

    /* ---- 两个“对外契约”的自检，合占一行 ----
     * gate：调用门描述符里的 DPL（从 GDT 读回，不是打印常量）→ 必须是 3；
     *       deny：拿老化字段去走门 → 白名单外，返回 0 且 sched_tune.age_ticks 不变；
     *       old：拿回血阈值去走门 → 白名单内，返回旧值 0x1E（30），值原样写回。
     * own：一个带析构的载荷交给任务，任务死后调度器调 own_dtor 一次（dtor=0x01），
     *      载荷里的 magic 被析构清零（mag=0x00）——这就是“参数归谁管”的可数证据。 */
    task_reset();
    gate_dpl = tune_gate_dpl();
    gate_deny = tune_gate_set(TUNE_FIELD_AGE, 7);           /* 白名单外：应该被拒 */
    gate_old = tune_gate_set(TUNE_FIELD_RESTORE, 30);       /* 白名单内：返回旧值 30 */

    task_spawn_owned(demo_owned, demo_own_payload(), demo_own_dtor, 1, 0);
    demo_run_workers();                                     /* 任务 return → dtor 交接 */

    rep_gate(gate_dpl, gate_deny, gate_old, task_dtor_calls(),
             demo_own_magic(), demo_own_seen(), demo_own_why());

    /* ---- 租约跟着任务走：一个借了页就死的任务（row 14 的 ls 后两位） ----
     * 它借 5 页、当场还 1 页、写一下就返回，从不 close 剩下的；收回靠的是
     * task_reclaim 里的 page_lease_release_owner(slot)，而后者现在是
     * **按这个任务自己的借用表**逐条收回的（不再线性扫全局 8 槽）。
     * 这一条把“谁放行谁必须记得关”换成了“放行的那一刻就登记了主人与页号，
     * 主人死了按登记逐页收回”。 */
    task_reset();
    for (i = 0; i < 5; i++)
        lease_pages[i] = page_alloc();
    slot_lease = task_spawn_owned(demo_lease, (void *)lease_pages,
                                  demo_lease_dtor, 1, 0);
    demo_run_workers();                          /* 内部最后一步就是 task_reclaim() */
    lease_revoked = (unsigned long)page_lease_revoked();
    demo_set_lease_stats((unsigned long)page_lease_denied(), (unsigned long)task_lease_peak());
    for (i = 0; i < 5; i++) {               /* 页本身是 main 分配的，还回来 */
        page_free(lease_pages[i]);
        lease_pages[i] = 0;
    }

    /* ---- ring3：TSS 装好之后，从用户态真的走一次调用门 ----
     * 顺序是死的：先 page_lock_kernel()（把整张恒等映射的 U/S 全清掉）、再
     * 租这两张用户页。反过来做，刚租出去的页会被一起清掉。
     * 拷而不是“直接让用户去跑内核 text”：代码页必须是 U/S=1 的页，
     * 而内核自己的 text 页在这一步之后就真的关上了。 */
    tss_init();
    /* 每任务页目录：放在 page_init 之后（要靠页分配器拿页）、user_enter 之前
     * （进 ring3 时 asm 会按槽位装 CR3）。slot 0 不建私有表——main 就在内核态，
     * 而且 0x02000000 那个按需分页窗口只在内核页目录里建。
     * 这一步开 8 页（4 个槽位 × 页目录 + PT0），所以它必须在页分配器的账上
     * 可回收的地方：失败时返回负值，这里只打一行，不 panic（少一张页目录
     * 只等于"那个槽位不开隔离"，不该拖垮整机）。 */
    {
        int pgd_rc = pgdir_init();

        printk_dev("[pgd] rc=%d s1=0x%06X s2=0x%06X\n",
                   pgd_rc, pgdir_of(1), pgdir_of(2));
    }
    page_lock_kernel(0, KERNEL_LOCK_END);
    user_code = page_alloc();
    user_stack = page_alloc();
    /* 租借而不是裸开：ttl=200 tick（约 2 秒）远大于这趟自检的时长，
     * 所以这里的“超时”只会在下面那句手动 tick 里真发生——tick 已经挂进
     * sched_tick 了，随便一个真中断就可能把租约提前收走，把证据弄脏。 */
    page_lease_open(PAGE_LEASE_OWNER_KERNEL, user_code, 200);
    page_lease_open(PAGE_LEASE_OWNER_KERNEL, user_stack, 200);
    /* 用户代码在 intr.asm 里是连着的一段，一次拷完：
     * user_stub（走门）在前，user_probe（越权探测）居中，user_io（端口探测）收尾。 */
    copy_bytes(user_code, (unsigned long)user_stub,
               (unsigned long)user_io_end - (unsigned long)user_stub);
    /* 栈顶往下留 64 字节：用户态那段代码会把三个返回值写在自己栈顶下方 */
    user_esp0 = user_stack + PAGE_SIZE_BYTES - 64;
    /* 只登记这两页：ring3 给的指针必须落在它们之间才允许 deref */
    tune_gate_user_window(user_code < user_stack ? user_code : user_stack,
                          (user_code > user_stack ? user_code : user_stack)
                          + PAGE_SIZE_BYTES);
    user_enter(user_code, user_esp0);
    /* user_enter 进来时 eflags 是 0x002（IF=0）：ring3 期间不许中断插进来，
     * 而 far call 回来不会恢复 eflags，所以出 ring3 之后这里必须自己开中断。 */
    cpu_sti();
    /* 栈页顶上往下：前 32 字节是用户态的“请求结构体 + 结果区”，
     * 结果区在 esp0-16/-12/-8（见 boot/intr.asm 的 user_stub）。
     * 第三次的返回值在 user_esp0-8：野指针落在未映射的 0x04000000，
     * 窗口校验是唯一拦住“内核替用户去解引用”的东西。 */
    ring3_ret = *(volatile unsigned long *)(user_esp0 - 16);
    ring3_over = *(volatile unsigned long *)(user_esp0 - 12);
    ring3_wild = *(volatile unsigned long *)(user_esp0 - 8);
    ring3_cpl = tune_gate_last_cpl;

    /* ---- 第二趟以后：同一段探测代码，换一个目标地址 ----
     * 目标地址写进用户栈顶下面一格（probe 自己 `mov ecx,[esp-4]`），
     * 这样“探哪儿”由内核决定，代码不必重复拷四份。 */
    probe_entry = user_code + ((unsigned long)user_probe - (unsigned long)user_stub);
    *(volatile unsigned long *)(user_esp0 - 4) = 0x00008000UL;  /* 1MB 内的内核 text */
    user_enter(probe_entry, user_esp0);
    cpu_sti();
    ring3_knl[0] = ring3_fault_err;
    ring3_fault_err = 0;

    *(volatile unsigned long *)(user_esp0 - 4) = 0x00A00000UL;  /* 1MB 以上的空白页 */
    user_enter(probe_entry, user_esp0);
    cpu_sti();
    ring3_knl[1] = ring3_fault_err;
    ring3_fault_err = 0;

    /* 第 3、4 趟专门验“引用计数”：先给栈页再加一个借用者（refs 2），
     * 还一次之后 refs 变 1 —— 这一页**还开着**，ring3 依然读得到（应为 0）；
     * 再还一次 refs 归零才真的收回 U/S（应为 5）。
     * 这两个数就是“关一次不等于关掉”和“最后一个借用者说了算”的证据。 */
    page_lease_open(PAGE_LEASE_OWNER_KERNEL, user_stack, 200);
    page_lease_close(PAGE_LEASE_OWNER_KERNEL, user_stack);    /* refs 2 -> 1：仍开着 */
    *(volatile unsigned long *)(user_esp0 - 4) = user_stack;
    user_enter(probe_entry, user_esp0);
    cpu_sti();
    ring3_knl[2] = ring3_fault_err;
    ring3_fault_err = 0;

    /* refs 归零：这一页真关上（Probe 4 的 err=5 就是它关上的证据） */
    page_lease_close(PAGE_LEASE_OWNER_KERNEL, user_stack);
    *(volatile unsigned long *)(user_esp0 - 4) = user_stack;
    user_enter(probe_entry, user_esp0);
    cpu_sti();
    ring3_knl[3] = ring3_fault_err;
    ring3_fault_err = 0;

    /* ---- 第五趟：ring3 直接读端口 ----
     * TSS 里没有 I/O 位图（iomap_base = 0x68，等于“表里没有这张表”），所以
     * CPL3 的 in/out 在“查位图”那一步就越界，得到 #GP(0)。被拦住时
     * exception_handler_gp 把现场收回内核、ring3_io_deny=1；没拦住就是 0 ——
     * 那意味着 ring3 能直接动 8259/8254/CRTC，等于把设备全给了用户态。
     * 这一趟放在租约超时回收**之前**：代码页还得开着，否则取指就先缺页了。 */
    ring3_io_deny = 0;
    probe_entry = user_code + ((unsigned long)user_io - (unsigned long)user_stub);
    user_enter(probe_entry, user_esp0);
    cpu_sti();

    /* 代码页故意“忘了还”：它还在租约表里（ttl=200）。手动 tick 到超时，
     * 让超时回收把这一页强制关上——这是“忘了关”的兜底路径，也是
     * page_lease_forced() 唯一会 +1 的地方。上限 400 只是防止死循环。 */
    for (i = 0; i < 400 && page_lease_active(); i++)
        page_lease_tick();
    lease_forced = (unsigned long)page_lease_forced();

    rep_ring3(tss_tr(), ring3_cpl, ring3_ret, ring3_over, ring3_wild,
              sched_tune.restore_ticks, tune_gate_cap(TUNE_FIELD_RESTORE),
              ring3_knl, lease_forced, lease_revoked, demo_lease_notice());
    tune_field_set(TUNE_FIELD_RESTORE, 30);

    /* ---- 阶段三：睡眠队列排序 + 动态创建 + 传参 + 槽位复用 ----
     * A 一进来就返回（槽位和栈页立刻空出来）；C 睡 50ms、B 睡 100ms，
     * 于是“后创建的 C 先醒”，证明睡眠队列按 wake_tick 排序而非先来先醒；
     * B 醒来后带着参数 0xDEAD0001 动态创建 D，D 打印自己的 slot，应正好是 1。 */
    task_reset();
    /* 先验一次“入口为空”：必须原样返回 0，且不占槽位/栈页——否则 A 会拿到 slot 2 */
    spawn_null = (unsigned long)task_spawn(0, 0, 1, 0);
    demo_wake_seq_reset();
    task_spawn(demo_a, 0, 1, 0);
    task_spawn(demo_b, (void *)0xDEAD0001UL, 2, 0);
    task_spawn(demo_c, 0, 0, 0);
    rep_phase3_head(spawn_null);
    demo_run_workers();

    /* ---- 阶段四：主动让出（yield）——3 个同优先级任务，每趟都 yield ----
     * 若 yield 只是“切一下”而不扣时间片，三个人照样均分 tick，看不出差别；
     * 真正的证据是切换次数：阶段一同样 12 个 tick 只切 7 次，这里每个时间片
     * 被 yield 提前用完，切换次数会大一个量级。 */
    task_reset();
    demo_run_begin(timer_ticks() + 12);
    slot_y1 = task_spawn(demo_yield, 0, 2, 0);
    slot_y2 = task_spawn(demo_yield, 0, 2, 0);
    slot_y3 = task_spawn(demo_yield, 0, 2, 0);
    demo_run_workers();
    rep_phase4(task_ticks_of(slot_y1), task_ticks_of(slot_y2),
               task_ticks_of(slot_y3), task_switches());

    /* ---- 阶段五：睡眠队列（小顶堆）的排序证据 ----
     * 4 个 sleeper 睡眠时长分别 100/200/50/150ms，创建顺序是 slot 1..4。
     * 醒来顺序若按 ms 升序，应当打出 3,1,4,2 —— 既不是创建顺序（1234），
     * 也不是“先睡先醒”（1234），只有按键排序的堆才会给出这个排列。 */
    task_reset();
    demo_wake_order_reset();
    slot_s1 = task_spawn(demo_sleep_wake, (void *)100UL, 1, 0);
    slot_s2 = task_spawn(demo_sleep_wake, (void *)200UL, 1, 0);
    slot_s3 = task_spawn(demo_sleep_wake, (void *)50UL, 1, 0);
    slot_s4 = task_spawn(demo_sleep_wake, (void *)150UL, 1, 0);
    demo_run_workers();
    rep_phase5(demo_wake_order());

    /* ---- 阶段六：历史惩罚（衰减）----
     * 独跑一个 prio2 任务、slice 显式给 1：4 片降一级，8 个 tick 落到 prio0。
     * 降到 prio0 之后它自己退出，run_workers 等到队列排空。 */
    task_reset();
    task_spawn(demo_decay, 0, 2, 1);
    demo_run_workers();

    /* ---- 阶段七：guard 页把“栈溢出”变成一次可恢复的事件 ----
     * 标签先打出来，越界写触发 #PF，缺页处理把后半句接在同一行上。 */
    task_reset();
    rep_guard_head();
    task_spawn_owned(demo_guard, demo_guard_payload(), demo_guard_dtor, 2, 0);
    demo_run_workers();
    rep_guard_tail(demo_guard_why());

    /* int 0x31 的三个值：旧值 / 非法值被拒 / 拒后仍保持。
     * 整行必须 ≤ 79 列（80 列时行尾的 '\n' 会多顶一行），所以标签写成 "|tune"。
     * 列宽由 tools/linewidth.py 在 make 前算，别再用肉眼数格子。 */
    rep_done(task_switches(), page_frames_used(), page_frames_total(),
             (unsigned long)task_rq_depth(), (unsigned long)task_sq_depth(),
             demo_tune_old(), demo_tune_bad(), demo_tune_kept());

    /* 排版法律的终值检查：整屏打完、切页之前读一次只增不减的 ovf。
     * 非 0 就打一行告警——它落在 row 24，行尾的 '\n' 会让屏幕整体上滚，
     * verify 的 24 行基准随即错位报红。所以这里不需要在正常路径上显示
     * “ovf 应该是 0”这种字段：真违规时页面自己会喊，而且喊得很难忽略。 */
    layout_ovf = stat_read(STAT_OVF);
    if (layout_ovf)
        rep_layout_alarm(layout_ovf);

    /* 屏幕证据全部打完，切到用户页：之后的键与 shell 输出都落在第 1 页，
     * 第 0 页（0xB8000）保持原样给 verify / pmemsave 用。
     * 注意顺序：必须放在下面那次“窗口外写”之前——#PF unhandled 会直接进
     * kernel_idle()，`main` 剩下的代码根本跑不到。 */
    shell_console_open();
    console_start = (unsigned long)vga_crtc_start();   /* 期望 0x800（第 1 页起点 / 2 字节） */
    console_chars = (unsigned long)shell_console_chars();

    /* 窗口之外：没有页目录项也不归补页逻辑管，应打印 unhandled 后停机 */
    *(volatile unsigned int *)OUTSIDE_ADDR = 1;

    printk("unreachable\n");
    kernel_idle();                  /* 自检跑完：开中断等事件，键盘在这里被收进来 */
    return 0;                       /* 不可达 */
}
