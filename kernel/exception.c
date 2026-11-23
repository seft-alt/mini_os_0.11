/* ==============================================================
 * CPU 异常处理
 *
 * 三种向量：0（除零）、13（通用保护）、14（缺页）。
 *
 * 缺页有恢复路径：交给 page_fault_map 补页表/补页，成功就直接返回，
 * 桩的 iret 会把出错的那条指令重新执行一遍；补不了（越界、权限问题）
 * 才打印现场并停机。其余异常没有恢复语义，一律 panic。
 * ============================================================== */
#include "linux/exception.h"
#include "linux/idt.h"
#include "linux/intr.h"
#include "linux/kernel.h"
#include "linux/panic.h"
#include "linux/page.h"
#include "linux/printk.h"
#include "linux/stat.h"
#include "linux/vector.h"
#include "linux/task.h"
#include "linux/vga.h"

/* CR2 保存最近一次缺页的线性地址 */
static unsigned int read_cr2(void)
{
    unsigned int value;

    __asm__ volatile ("mov %%cr2, %0" : "=r"(value));
    return value;
}

/* boot/intr.asm 里的“回内核”桩：ring3 越权访问被拦下时，把返回帧改成它 */
extern void user_return_stub(void);

/* 一位十六进制：槽位号最大 4，用 8 位打印白占 7 列，腾出来的位置给死因 */
static void put_hex1(unsigned long v)
{
    char b[2];

    b[0] = "0123456789ABCDEF"[v & 0xFUL];
    b[1] = 0;
    printk(b);
}

static const char *exception_name(unsigned int vector){
    switch (vector) {
    case IDT_VEC_DIVIDE:     return "divide error";
    case IDT_VEC_GP:         return "general protection";
    case IDT_VEC_PAGE_FAULT: return "page fault";
    default:                 return "unknown exception";
    }
}

/* 输出压缩成一行一行，因为文本屏只有 25 行：屏上写满第 25 行就会立即上滚，
 * 自检输出必须掐着行数，异常现场也尽量一行说完。
 * 停机统一走 panic()：现场（原因 + err）在这里打，退出点（cli + hlt + 调用点 eip）
 * 和别处的"到达不了"共用一条路径，串口日志里也会留下同一行。 */
void exception_handler(unsigned int vector, unsigned int error)
{
    printk("exception %s err=0x%08X\n", exception_name(vector), error);
    panic(exception_name(vector));
}

/* 缺页专用：桩把现场地址传进来，处理函数可以改现场（eip）并返回 esp。
 *
 * 现场布局（pusha 之后的 esp = ctx，往上是 CPU 的 iret 帧）：
 *   ctx[0..7]   pusha 保存的 8 个寄存器（edi..eax）
 *   ctx[8]      错误码
 *   ctx[9]      eip
 *   ctx[10]     cs
 *   ctx[11]     eflags
 * 注意错误码在**第 9 个字**（8 个寄存器之后），不是第 1 个——这一点踩错过一次：
 * 当成 ctx[0] 读会拿到 edi，于是"缺页"被判成"保护错误"，补页逻辑直接放弃。
 *
 * 四条出路：
 *   1) guard 页命中 —— 栈溢出。不改栈，只把 eip 改成 task_guard_exit：iret 之后该
 *      任务走进退出路径（槽位立刻空出、栈页延后回收），内核继续跑；走这条
 *      专门的入口而不是 task_exit，是为了让 dtor 拿到死因 2（被守卫杀掉），
 *      而不是被当成正常收工；
 *   1.5) ring3 发起的一次缺页 —— **一律当越权**，把返回帧改成“回内核”
 *      （eip=user_return_stub、cs=内核代码段）。判据只有一条：`err & 0x04`
 *      （U/S=1，说明发起者是用户态）。这条不杀任务也不停机，只把控制权交回内核：
 *      既不是内核 bug，也不是“任务跑炸了”，而是权限位把不该发生的事拦住了。
 *      ——为什么不再按地址范围判：每任务页目录已经把 **16MB 以上做成“整个不存在”**
 *      （dir[4..1023] = 0），0..16MB 里真正对 ring3 放行的只有 lease / 私有 PT0
 *      开过的那几页；其余任何一页对用户来说都是“不该碰的”，碰了就是越权。
 *      早先那条 `cr2 < KERNEL_LOCK_END` 是给“单张页目录”写的补丁——那时用户任务
 *      和内核共用一张表，只有 16MB 恒等映射被锁过，16MB 以上只能靠“地址不在窗口内”
 *      兜底。现在表都不是同一张了，再按地址判只会漏：把高地址的越权放给
 *      page_fault_map 去试补页。
 *      ——后果是：**用户态缺页再也不会触发内核补页**，补页只服务内核态。
 *   2) page_fault_map 补上页 —— iret 回到原 eip 重试那条指令；
 *   3) 其余 —— 打现场、停机。
 *
 * 为什么栈溢出要区别对待：栈溢出不是"程序写错了地址"，而是"这个任务跑炸了"。
 * 一个任务把内核拖下水是最糟的结果；把它变成一次干净的 #PF + 任务死亡，
 * 才叫"隔离"。 */
#define PF_FRAME_ERROR 8
#define PF_FRAME_EIP   9
#define PF_FRAME_CS    10

/* ring3 越权访问被拦下的现场（cr2 / err），由 main 打在屏幕行上。
 * 是“全局”而不是“每任务”的，因为现在只有 main 会进 ring3。 */
unsigned long ring3_fault_va;
unsigned long ring3_fault_err;

/* ring3 执行 in/out 被 #GP 拦下的证据：1 = 拦住，0 = 没拦住。
 * TSS 里没有 I/O 位图（iomap_base = 结构大小）时这一步必然拦；
 * 摆错成“有位图且允许”时这个 0 就是“端口对用户态开着”的指纹。 */
unsigned long ring3_io_deny;

unsigned long exception_handler_pf(unsigned long ctx)
{
    unsigned int *frame = (unsigned int *)ctx;
    unsigned int error = frame[PF_FRAME_ERROR];
    unsigned int cr2 = read_cr2();
    unsigned int phys;
    int slot;

    vec_hit(IDT_VEC_PAGE_FAULT);        /* 先记账：这条向量真的被走过了 */
    slot = task_guard_victim(cr2);
    if (slot >= 0) {
        stat_inc(STAT_KILL);                /* 任务因为栈溢出被守卫杀掉：计数给 stat 看 */
        printk(" slot=0x");
        put_hex1((unsigned long)slot);      /* 槽位号只有一位：8 位打印白占 7 列 */
        printk(" va=0x");
        vga_print_hex(cr2);
        printk(" stack overflow -> task killed");

        frame[PF_FRAME_EIP] = (unsigned int)task_guard_exit;  /* iret 之后按“被守卫杀掉”退出 */
        return ctx;
    }

    /* ring3 发起的缺页：不是内核 bug，而是隔离生效的证据。
     * 判据只有一条：错误码 bit2（U/S）=1 → 是用户态发起的。
     * **不再看地址落在哪**——每任务页目录已经把 16MB 以上整个做成“不存在”，
     * 0..16MB 里对 ring3 开着的只有 lease / 私有 PT0 放过的那几页，其余一律越权。
     * 处理方式是把返回帧改成“回内核”：eip=user_return_stub、cs=KERNEL_CS。
     * iret 从 CPL0 到 CPL0 只弹 EIP/CS/EFLAGS 三项，所以 esp 会停在这张 esp0 栈的
     * ctx[8] 上——user_return_stub 正好用 user_kret[cur] 把 esp 换回这个任务自己的
     * 栈，同一段桩两处复用。eflags 原样带过来（可能是 0x202，IF=1）：回 C 侧再 sti。
     * 现场的两个数（cr2/err）留给 main 打在 row 14 上：缺页处理体在这里不打屏，
     * 免得这一行和后面的行序耦合。
     *
     * 这条同时是一道**止损**：用户态碰高地址不再进 page_fault_map，也就不会再
     * 出现“用户把内核拖进补页路径、补不上就 kernel_idle 把系统停住”的场面。 */
    if (error & 0x04) {
        ring3_fault_va = cr2;
        ring3_fault_err = error;
        frame[PF_FRAME_EIP] = (unsigned int)user_return_stub;
        frame[PF_FRAME_CS] = KERNEL_CS;
        return ctx;
    }

    phys = page_fault_map(cr2, error);

    if (phys) {
        stat_inc(STAT_PF);                  /* 补页成功：这是“按需分页真的起作用了”的次数 */
        /* 一行一次输出，列宽才可算（tools/linewidth.py 按整行算） */
        printk("[mem] PF 0x%08X -> 0x%08X err=0x%08X resume\n", cr2, phys, error);
        return ctx;                 /* iret 重试原指令，此时页已存在 */
    }

    stat_inc(STAT_PF_FAIL);                 /* 补不上：这一笔是“分页守住边界”的证据 */
    printk("[mem] PF 0x%08X err=0x%08X %s%s -> unhandled\n",
           cr2, error,
           (error & 1) ? "(protection)" : "(not-present)",
           (error & 2) ? " write" : " read");
    /* 这是自检的最后一步（main 故意往窗口外写），所以这里进的是**空闲**而不是
     * 停机：`clip + hlt` 会让 IRQ1 永远进不来，键盘就只能在自检期间工作。
     * 真正的异常仍然走 exception_handler() 的 kernel_halt()。 */
    kernel_idle();
    return ctx;
}

/* 向量 13（#GP）的入口：和缺页一样“改现场、返回到内核”，但只对 ring3 生效。
 *
 * 为什么要区别对待：ring3 执行 `in`/`out` 时，CPU 会先查 TSS 的 I/O 位图；
 * 我们的 iomap_base 等于结构大小（表里根本没有位图），这一步就因“超出 TSS
 * 界限”失败 → #GP(0)。这是**权限位在干活**，是隔离生效的证据；把整个内核
 * 停掉就再也看不到键盘，自检也走不完。判据两条：
 *   cs 的低两位 = 3  —— 确实是从用户态发起的（内核自己踩到 #GP 仍然要停机）；
 *   error = 0        —— 不是“选择子不合法”那一类，而是权限/位图被拒。
 * 满足就把返回帧改成“回内核”（eip = user_return_stub、cs = KERNEL_CS），
 * 与缺页那条路复用同一个桩。 */
unsigned long exception_handler_gp(unsigned long ctx)
{
    unsigned int *frame = (unsigned int *)ctx;
    unsigned int error = frame[PF_FRAME_ERROR];
    unsigned int cs = frame[PF_FRAME_CS];

    vec_hit(IDT_VEC_GP);                /* ring3 探端口也会走到这里 */
    if ((cs & 3u) == 3u && error == 0u) {
        ring3_io_deny = 1;
        stat_inc(STAT_IO);
        frame[PF_FRAME_EIP] = (unsigned int)user_return_stub;
        frame[PF_FRAME_CS] = KERNEL_CS;
        return ctx;
    }

    exception_handler(IDT_VEC_GP, error);   /* 其余 #GP 仍是致命的：打印现场后停机 */
    return ctx;                             /* kernel_halt 不返回，这里只为对齐签名 */
}

void exception_init(void)
{    idt_set_gate(IDT_VEC_DIVIDE, isr_divide_error);
    idt_set_gate(IDT_VEC_GP, isr_general_protection);
    idt_set_gate(IDT_VEC_PAGE_FAULT, isr_page_fault);
}
