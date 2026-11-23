/* 观测类命令：stat / irq / tasks / log / heap / kstress / kwait / panic。
 *
 * 由 shell/shell.c 拆分而来（纯搬运，逻辑未改）；clock / date 后来
 * 又拆进 shell/cmd_time.c，这一层只剩“看一眼计数器”。
 * ============================================================== */
#include "linux/shell.h"
#include "linux/state.h"
#include "linux/console.h"
#include "linux/fmt.h"
#include "linux/cmds.h"
#include "linux/stat.h"
#include "linux/vector.h"
#include "linux/idt.h"
#include "linux/klog.h"
#include "linux/task.h"
#include "linux/kwait.h"
#include "linux/heap.h"
#include "linux/page.h"
#include "linux/panic.h"
#include "linux/kv.h"

/* `stat`：把 core/stat.h 里那几个计数器摆成一张小表。
 *
 * 为什么不在自检屏上多打一行：自检屏的 24 行是“一次性证据”，用完就固定了；
 * 计数器是**跑着涨**的（敲几个命令、缺几次页、ring3 探几次端口），
 * 属于“随时问一下”的东西，所以放在这里看。
 * 顺序 = STAT_* 的枚举顺序，加新计数器只要动 stat.h 和 stat.c 的短名表。
 * 一行放不下就换行（续行用 5 个空格与第一行的键对齐），不截半个字段。 */
int cmd_stat(void)
{
    char line[SHELL_OUT_COLS];
    unsigned int k = 0, id;

    line[k++] = 's';
    line[k++] = 't';
    line[k++] = 'a';
    line[k++] = 't';
    for (id = 0; id < (unsigned int)STAT_MAX; id++) {
        const char *nm = stat_name(id);
        unsigned int nlen = 0, need;

        while (nm[nlen] != '\0')
            nlen++;
        need = 1 + nlen + 1 + 5;                    /* 空格 + 键 + '=' + 0xNNN */
        if (k + need > SHELL_OUT_COLS) {
            shell_out_puts(line, k);
            k = 0;
            while (k < 5)                           /* 续行缩进到第一行的键 */
                line[k++] = ' ';
        }
        line[k++] = ' ';
        while (*nm != '\0')
            line[k++] = *nm++;
        line[k++] = '=';
        k += hex3(line + k, stat_read(id));
    }
    if (k > 0)
        shell_out_puts(line, k);

    /* 同一条证据走串口。`pf_fail` 就是 wx.sh / pghigh.sh 判"没进 unhandled
     * 分支"的那个数——屏幕上是 0 或非 0，KV 里是十进制，判据不用再数行。 */
    kv_begin("stat");
    for (id = 0; id < (unsigned int)STAT_MAX; id++)
        kv_dec(stat_name(id), stat_read(id));
    kv_end();
    return SHELL_RC_OK;
}

/* `irq`：按向量号打印“这条路真的被叫到过几次”。
 * 只挑六个有故事的：0D #GP、0E #PF、20 时钟、21 键盘、30 让出、31 调参。
 * 和 `stat` 的区别是它数的是**中断/异常本身**，不是业务事件；两者一起看，
 * 才能分清“按需分页在干活”和“时钟只是在空转”。
 * 末行多打一个 `sum=`：六条之和不可能超过全向量合计，越了就是打点漏了/错位了。 */
int cmd_irq(void)
{
    static const char *const names[6] = { "gp", "pf", "tick", "kbd", "yield", "tune" };
    static const unsigned int vecs[6] = {
        IDT_VEC_GP, IDT_VEC_PAGE_FAULT, IDT_VEC_TIMER,
        IDT_VEC_KEYBOARD, IDT_VEC_YIELD, 0x31
    };
    char line[SHELL_OUT_COLS];
    unsigned int k = 0, i;

    line[k++] = 'i';
    line[k++] = 'r';
    line[k++] = 'q';
    for (i = 0; i < 6; i++) {
        const char *nm = names[i];
        unsigned long n = vec_count(vecs[i]);

        if (k + 14 > SHELL_OUT_COLS) {              /* 一行放不下就换行，续行缩进 */
            shell_out_puts(line, k);
            k = 0;
            while (k < 5)
                line[k++] = ' ';
        }
        line[k++] = ' ';
        while (*nm != '\0')
            line[k++] = *nm++;
        line[k++] = '=';
        if (n == VEC_BAD) {                         /* 号不存在 ≠ 一次没来过 */
            line[k++] = '?';
            line[k++] = '?';
        } else {
            k += hex6(line + k, n);
        }
    }
    if (k + 16 > SHELL_OUT_COLS) {
        shell_out_puts(line, k);
        k = 0;
        while (k < 5)
            line[k++] = ' ';
    }
    line[k++] = ' ';
    line[k++] = 's';
    line[k++] = 'u';
    line[k++] = 'm';
    line[k++] = '=';
    k += hex6(line + k, vec_total());
    shell_out_puts(line, k);

    /* 串口那一路：判据是"这条命令真的被执行了"（有记录）+"对应向量真的被走过"
     * （计数非 0），不必再从屏幕数 `irq` 回显的字符。`?`（号不存在）写成 `-`，
     * 让 KV 的值仍是"数或不数"，不夹带屏幕的排版约定。 */
    kv_begin("irq");
    for (i = 0; i < 6; i++) {
        unsigned long n = vec_count(vecs[i]);

        if (n == VEC_BAD)
            kv_str(names[i], "-");
        else
            kv_dec(names[i], n);
    }
    kv_dec("sum", vec_total());
    kv_end();
    return SHELL_RC_OK;
}

/* `tasks`：每槽位一行，看得到“谁在跑、还剩几 tick、一共吃了多少 CPU”。
 * cpu= 是累计占用的 tick（100Hz 下一个 tick = 10ms），换人不清零；
 * s= 是配置的时间片、斜杠后是本轮剩余——两者差得越多，说明这一轮快用完了。
 * 为什么这条命令值得有：`sw=` 只能说“换了多少回”，说不出“换了多少 CPU”。
 * 空槽也照列（状态 unused、c= 保留它最后一次跑完时的账）：任务跑完就回收了，
 * 只剩这一行数字能证明“这个槽位真的烧过 CPU”。 */
int cmd_tasks(void)
{
    char line[SHELL_OUT_COLS];
    unsigned int i, k;
    int slots = task_slots();

    for (i = 0; (int)i < slots; i++) {
        const char *st = task_state_name((int)i);

        if (!st)
            continue;
        k = 0;
        line[k++] = 't';
        k += udec(line + k, i);
        line[k++] = ' ';
        while (*st != '\0')
            line[k++] = *st++;
        line[k++] = ' ';
        line[k++] = 'p';
        line[k++] = '=';
        k += udec(line + k, (unsigned long)task_prio_of((int)i));
        line[k++] = ' ';
        line[k++] = 's';
        line[k++] = '=';
        k += udec(line + k, task_slice_of((int)i));
        line[k++] = '/';
        k += udec(line + k, task_left_of((int)i));
        line[k++] = ' ';
        line[k++] = 'c';
        line[k++] = '=';
        k += hex6(line + k, task_ticks_of((int)i));
        line[k++] = ' ';
        line[k++] = 'r';
        line[k++] = '=';
        k += udec(line + k, task_runs_of((int)i));
        /* 从 ring3 进门时会落到这个栈顶：worker 各是各的任务页（>= 0x20000），
         * main 是内核默认栈（< 0x20000）。一列就看出"TSS.esp0 跟不跟着任务走"。
         * 末尾再挂一个它名下的用户页数（exec 装上来、任务死时由调度器收）。 */
        line[k++] = ' ';
        line[k++] = 'e';
        line[k++] = '=';
        k += hex6(line + k, task_esp0_of((int)i));
        line[k++] = ' ';
        line[k++] = 'u';
        line[k++] = '=';
        k += udec(line + k, (unsigned long)task_user_pages_of((int)i));
        shell_out_puts(line, k);
    }
    return SHELL_RC_OK;
}

/* `log`：把内核日志环按"旧 -> 新"打出来，每行前面带行首 tick。
 * 数据源就是 printk 的字符流（core/klog.c 按 '\n' 切行），所以"屏幕上出现过的
 * 每一行"这里都有——用户页只有 24 行、会滚，环在内存里，滚不掉。 */
int cmd_log(void)
{
    char line[SHELL_OUT_COLS];
    unsigned int n = klog_count(), i, k;
    unsigned long tick;

    for (i = 0; i < n; i++) {
        const char *s = klog_get(i, &tick);

        if (!s)
            continue;
        k = 0;
        line[k++] = 't';
        line[k++] = '=';
        k += hex6(line + k, tick);               /* 11 列：t=0xNNNNNN + 空格 */
        line[k++] = ' ';
        while (*s != '\0' && k < SHELL_OUT_COLS)
            line[k++] = *s++;
        shell_out_puts(line, k);
    }
    if (n == 0)
        shell_out_puts("log empty", 9);
    return SHELL_RC_OK;
}

/* `heap`：堆的账——要了几页、发出去多少字节、live 几块。 */
int cmd_heap(void)
{
    char line[SHELL_OUT_COLS];
    unsigned int k = 0;

    line[k++] = 'h'; line[k++] = 'e'; line[k++] = 'a'; line[k++] = 'p';
    line[k++] = ' ';
    line[k++] = 'p'; line[k++] = '=';  k += udec(line + k, heap_pages());
    line[k++] = ' ';
    line[k++] = 'u'; line[k++] = '=';  k += hex6(line + k, heap_used_bytes());
    line[k++] = ' ';
    line[k++] = 'l'; line[k++] = '=';  k += udec(line + k, heap_live_blocks());
    /* 物理页账也放这一行：堆与页分配器是同一件事的两层（堆的页就是从这里要的），
     * 而"跑完一趟 exec 页有没有还回来"只有这个数看得出来。 */
    line[k++] = ' ';
    line[k++] = 'p'; line[k++] = 'f'; line[k++] = '=';
    k += udec(line + k, page_frames_used());
    line[k++] = '/';
    k += udec(line + k, page_frames_total());
    shell_out_puts(line, k);
    return SHELL_RC_OK;
}

/* `kstress`：堆自检——分 8 块 × 100 字节、写图案、读回、全还。
 * bad=0 说明"要得到、写得上、读得回、还得了"四步都成立；
 * used 回到 0、live 回到 0，才说明归还这条路也真的走通了。 */
int cmd_kstress(void)
{
    char line[SHELL_OUT_COLS];
    unsigned long bad = heap_selftest(8, 100);
    unsigned int k = 0;

    line[k++] = 'k'; line[k++] = 's'; line[k++] = 't'; line[k++] = 'r';
    line[k++] = 'e'; line[k++] = 's'; line[k++] = 's';
    line[k++] = ' ';
    line[k++] = '8'; line[k++] = 'x';
    line[k++] = '1'; line[k++] = '0'; line[k++] = '0';
    line[k++] = ' ';
    line[k++] = 'b'; line[k++] = 'a'; line[k++] = 'd'; line[k++] = '=';
    k += hex6(line + k, bad);
    line[k++] = ' ';
    line[k++] = 'u'; line[k++] = '=';
    k += hex6(line + k, heap_used_bytes());
    line[k++] = ' ';
    line[k++] = 'l'; line[k++] = '=';
    k += udec(line + k, heap_live_blocks());
    shell_out_puts(line, k);
    return SHELL_RC_OK;
}

/* `kwait`：等事件那一层的自检（kernel/kwait.c）——两个 worker 用一个信箱
 * ping-pong 16 轮，全程靠 task_wait_chan / task_wakeup，不靠 task_sleep。
 *
 * 两行证据：
 *   kwait n=16 ok=1 to=0
 *        —— 完成 16 轮、16 轮数据顺序全对（ok=1）、没被超时兜底打断（to=0）；
 *   kwait w=32 k=32 left=0
 *        —— 等待/唤醒次数配对（差值记账）、收工后没有槽位还挂在通道上。
 * left=0 是关键：它同时证明"被叫醒的确实回了就绪队列并被跑完"，
 * 而不是"唤醒丢了、槽位永远睡在那里"。 */
int cmd_kwait(void)
{
    char line[SHELL_OUT_COLS];
    unsigned int k, ok;

    ok = (unsigned int)kwait_selftest();

    k = put_lit(line, 0, "kwait n=");
    k += udec(line + k, kwait_rounds());
    k = put_lit(line, k, " ok=");
    k += udec(line + k, ok);
    k = put_lit(line, k, " to=");
    k += udec(line + k, kwait_timeout());
    shell_out_puts(line, k);

    k = put_lit(line, 0, "kwait w=");
    k += udec(line + k, kwait_wakeups());
    k = put_lit(line, k, " k=");
    k += udec(line + k, kwait_waits());
    k = put_lit(line, k, " left=");
    k += udec(line + k, (unsigned long)kwait_left());
    shell_out_puts(line, k);

    return SHELL_RC_OK;
}

/* `panic <原因>`：**故意**走一次统一停机出口。
 *
 * 为什么留一条"自杀"命令：panic 这条路径正常永远跑不到，"它在不在、串口上能不能
 * 看到、停机是不是真的停住"就没有任何证据。负向脚本 `tools/panic.sh` 按下它，
 * 断言屏幕与串口都出现 `PANIC: <原因>` 且机器不再响应。 */
int cmd_panic(void)
{
    const char *why = (shell_argc > 1) ? shell_argv_buf[1] : "probe";

    panic(why);                          /* 不返回 */
    return SHELL_RC_OK;                  /* 到不了这里：给编译器的交代 */
}
