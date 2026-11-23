/* ==============================================================
 * 自检的演示载荷（从 init/main.c 拆出来，见 include/init/demo.h）
 *
 * 这个文件里全是“跑起来给别人看”的任务：烧 CPU 的、睡着的、让出的、
 * 借页就死的、故意把栈写穿的。它们有一个共同点——**每个实验都自己产出证据**，
 * 而证据就是这里的 static 变量；main() 只能通过 demo.h 里的取数函数读，
 * 这就把“谁拥有这些数字”定清楚了（曾经的坑：槽位复用把阶段一的数读串了，
 * 根源就是证据散落在编排代码旁边、随手就能读早/读晚）。
 * ============================================================== */
#include "linux/demo.h"
#include "asm/io.h"
#include "linux/report.h"
#include "linux/lease.h"
#include "linux/task.h"
#include "linux/tune.h"
#include "linux/timer.h"

/* ---- 参数所有权自检：一份“结构体载荷” + 一个析构 ---- */
#define OWN_MAGIC 0x600DF00DUL

struct own_job {
    unsigned long magic;        /* 析构会把它清零：清掉就是“所有权交接完成”的痕迹 */
    unsigned long id;
};

static struct own_job own_job;
static unsigned long own_seen;          /* 任务自己确认过载荷内容 */
static unsigned long own_why = 0xFF;    /* dtor 收到的死因：正常 return 应为 0 */

/* 忙等的终点：CPU 型任务空转到这个 tick 就返回 */
static unsigned long sched_deadline;

/* 阶段六里经 int 0x31 拿到的三个值，攒到收尾行一起打：
 *   tune_old  = 第一次改阈值时返回的旧值（应为 0x1E = 30）
 *   tune_bad  = 非法值（0）那次调用的返回值（应为 0，被拒绝）
 *   tune_kept = 拒绝之后 restore_ticks 的实际值（应仍是 0x3C = 60） */
static unsigned long tune_old;
static unsigned long tune_bad;
static unsigned long tune_kept;

/* 阶段五：醒来顺序打包（4 个 sleeper 各占一个半字节，高半字节在前） */
static unsigned long wake_order;

/* 阶段三的 C/B 合并行用到的证据（C 先醒 → seq 应为 0x12） */
static unsigned long c_slot, c_gap, b_slot, b_gap, wake_seq;

/* 阶段八的载荷与死因 */
static struct own_job guard_job;
static unsigned long guard_why = 0xFF;
static unsigned long guard_seen = 1;

/* 借用表统计：由 main 在租约演示之后填入（数据源是 mm/lease.c 的计数） */
static unsigned long lease_denied_in;
static unsigned long lease_peak_in;

void demo_run_begin(unsigned long deadline_ticks)
{
    sched_deadline = deadline_ticks;
}

void *demo_own_payload(void)
{
    own_job.magic = OWN_MAGIC;
    own_job.id = 0xC0FFEEUL;
    own_seen = 0;
    own_why = 0xFF;
    return &own_job;
}

void demo_own_dtor(void *p, int reason)
{
    struct own_job *j = (struct own_job *)p;

    /* 任务死后由调度器调用：此刻任务栈页已经还回去了，这里只处理载荷本身。
     * reason 是死因——同一个载荷在不同死法下该做的事不一样，所以它必须传进来。 */
    own_why = (unsigned long)reason;
    if (j->magic == OWN_MAGIC)
        j->magic = 0;
}

unsigned long demo_own_magic(void) { return own_job.magic; }
unsigned long demo_own_seen(void)  { return own_seen; }
unsigned long demo_own_why(void)   { return own_why; }

void demo_set_lease_stats(unsigned long denied, unsigned long peak)
{
    lease_denied_in = denied;
    lease_peak_in = peak;
}

unsigned long demo_tune_old(void) { return tune_old; }
unsigned long demo_tune_bad(void) { return tune_bad; }
unsigned long demo_tune_kept(void) { return tune_kept; }

unsigned long demo_wake_order(void) { return wake_order; }
void demo_wake_order_reset(void)    { wake_order = 0; }
void demo_wake_seq_reset(void)      { wake_seq = 0; }

void *demo_guard_payload(void)
{
    guard_job.magic = OWN_MAGIC;
    guard_job.id = 0xBADCAFEUL;
    guard_why = 0xFF;
    guard_seen = 1;
    return &guard_job;
}

void demo_guard_dtor(void *p, int reason)
{
    struct own_job *j = (struct own_job *)p;

    guard_why = (unsigned long)reason;
    if (j->magic == OWN_MAGIC)
        j->magic = 0;
}

unsigned long demo_guard_why(void) { return guard_why; }

/* ---------------- 载荷本体 ---------------- */

/* 只做“确认载荷还在”，然后 return：走 task_return_stub → task_exit → dtor */
void demo_owned(void *arg)
{
    struct own_job *j = (struct own_job *)arg;

    if (j && j->magic == OWN_MAGIC)
        own_seen = 1;
}

/* 跑满时间片直到降到最低级（阶段六用两次：第一次验衰减，第二次为了演示
 * “把阈值在线抬上去之后同一段睡眠不再回血”）。deadline 是兜底期限：
 * 衰减若被改坏，这里不会挂死，只会让屏幕上打出难看的数字。 */
static void burn_until_floor(void)
{
    unsigned long deadline = timer_ticks() + 12;

    while (task_prio_of(task_self()) > 0 && timer_ticks() < deadline)
        ;
}

/* 纯 CPU 型任务：不 sleep、不 yield，只靠 tick 中断被抢占。
 * 用来观察“时间片”和“优先级”——只有这两种机制能把它切开。 */
void demo_burn(void *arg)
{
    (void)arg;

    while (timer_ticks() < sched_deadline)
        ;
}

/* 租约回收演示：一个“借了页就死”的任务。借一页、写一下、直接返回，
 * 从不 close —— 收回全靠 task_reclaim 里的 page_lease_release_owner(slot)。
 * 这正是“租约跟着任务走”和“靠调用方自觉”的区别所在。
 *
 * 再加一条“可撤销契约”的证据：它绑定一个通知回调，死前由 dtor 在页里留下
 * 哨兵。回调只有在 ① 真的收到了撤销通知 ② 通知到达时 dtor 已经跑过（哨兵在）
 * 两条同时成立时才把 lease_notice 置 1。所以 ls 末位那个数字回答的是
 * “通知来了，而且是在 dtor 之后来的”。 */
#define LEASE_SENTINEL 0x00C0FFEEu

static unsigned int lease_notice;    /* 1 = 收到撤销通知且时序正确 */
static unsigned int lease_dtor_ran;  /* dtor 是否已经跑过 */

static void lease_notice_cb(unsigned int owner, unsigned int phys, unsigned int why)
{
    volatile unsigned int *p = (volatile unsigned int *)phys;

    if (lease_dtor_ran && *p == LEASE_SENTINEL && why == PAGE_LEASE_WHY_OWNER)
        lease_notice = 1;
}

/* 任务死后由调度器调用：死前最后一次合法写“自己借的那页”，哨兵落在页里。
 * 这一步必须在“撤销通知”之前发生，否则回调判不出时序（见上面那段）。 */
void demo_lease_dtor(void *arg, int reason)
{
    unsigned int *pages = (unsigned int *)arg;

    (void)reason;
    *(volatile unsigned int *)pages[0] = LEASE_SENTINEL;
    lease_dtor_ran = 1;
}

unsigned long demo_lease_notice(void)
{
    return (unsigned long)lease_notice;
}

void demo_lease(void *arg)
{
    unsigned int *pages = (unsigned int *)arg;   /* 5 页；arg 也归它了（dtor 收尾） */
    unsigned int owner = (unsigned int)task_self();
    int k;

    page_lease_bind(owner, lease_notice_cb);
    /* 借 5 页，但每任务借用表只有 4 格：第 5 次必须先失败。
     * 这不是“资源不够就凑合”，而是“表是回收的唯一索引，记不下就不能借”。 */
    for (k = 0; k < 5; k++)
        page_lease_open(owner, pages[k], 200);
    /* 第 2 页当场还了：借用表里划掉一条，回收时就只收剩下的 3 条 */
    page_lease_close(owner, pages[1]);
    *(volatile unsigned int *)pages[0] = 0x1EADBEEFu;   /* 真用一下，说明这页是它借的 */
}

/* 阶段四：主动让出型任务。每跑一趟就 task_yield() 一次，不 sleep、不退出。
 * 它证明两件事：① yield 真的会换人（切换次数远大于 tick 数）；
 * ② yield 不白拿 CPU——时间片被直接清零，三个人照样均分 tick。 */
void demo_yield(void *arg)
{
    (void)arg;

    while (timer_ticks() < sched_deadline)
        task_yield();
}

/* 阶段五：每个 sleeper 醒来时把自己的槽位号塞进打包变量（高半字节在前）。
 * 4 个 sleeper 睡眠时长互不相同，醒来顺序应当按 ms 升序，而不是按创建顺序。 */
void demo_sleep_wake(void *arg)
{
    unsigned long ms = (unsigned long)arg;

    task_sleep(ms);
    cpu_cli();
    wake_order = (wake_order << 4) | (unsigned long)(task_self() & 0xF);
    cpu_sti();
}

/* 阶段六：验证“历史惩罚（衰减）”。一个 prio2 的纯 CPU 任务独跑，没人跟它抢，
 * 它一片片地把时间片跑满；连续 TASK_DECAY_QUANTA 片之后落回级别降一级。
 * 输出自己开始/结束时的优先级与烧掉的 tick 数——8 个 tick（slice=1、4 片一级）
 * 应当把它从 prio2 压到 prio0。独跑也能被降级，正说明降的不是“被抢占次数”。 */
void demo_decay(void *arg)
{
    int start_prio;
    int floor_prio;
    unsigned long burned;
    int rest_default;
    int rest_high;
    int rest_back;
    int deep_long;
    int deep_short;

    (void)arg;

    start_prio = task_prio_of(task_self());
    burn_until_floor();
    floor_prio = task_prio_of(task_self());
    burned = task_ticks_of(task_self());

    /* ① 默认阈值 sched_tune.restore_ticks=30：睡 400ms（40 tick）够额度，还一级 */
    task_sleep(400);
    rest_default = task_prio_of(task_self());

    /* ② 再跑回最低级，然后**通过 int 0x31** 把阈值抬到 60 tick：同一段
     *    400ms 睡眠就不够额度了，应当停在 0。这一步编译期常量做不到，
     *    而且改值与读旧值走的是对外接口，不是内核自己写全局。 */
    burn_until_floor();
    tune_old = tune_field_set(TUNE_FIELD_RESTORE, 60UL);
    task_sleep(400);
    rest_high = task_prio_of(task_self());

    /* ②' 非法值必须被拒：0 tick 的阈值没有任何意义，系统调用返回 0 且不改值 */
    tune_bad = tune_field_set(TUNE_FIELD_RESTORE, 0UL);
    tune_kept = sched_tune.restore_ticks;

    /* ③ 再走一次系统调用改回 30：同一段睡眠又回血——证明改的是行为，
     *    不是只改了打印 */
    tune_field_set(TUNE_FIELD_RESTORE, 30UL);
    task_sleep(400);
    rest_back = task_prio_of(task_self());

    /* ④ 线性回血：从最低级睡 600ms（60 tick = 2 个单价）→ 应当一次还两级。
     *    旧实现（够阈值就还一级）在这一段只会还 1，所以这两个数就是新旧的分界。 */
    burn_until_floor();
    task_sleep(600);
    deep_long = task_prio_of(task_self());

    /* ⑤ 同一单价系列里只睡一个单价（300ms = 30 tick）→ 只还一级。
     *    与 ④ 一起看：睡得久不是“多睡一次”，是按倍数折算，两级差摆在一行里。 */
    burn_until_floor();
    task_sleep(300);
    deep_short = task_prio_of(task_self());

    cpu_cli();
    rep_decay((unsigned long)start_prio, (unsigned long)floor_prio, burned,
              (unsigned long)rest_default, (unsigned long)rest_high,
              (unsigned long)rest_back, (unsigned long)deep_long,
              (unsigned long)deep_short);
    cpu_sti();
}

/* 阶段七：故意把栈写穿。栈页下面那一页的恒等映射已被撤掉，于是这次写会
 * 触发 #PF；缺页处理认出“落在 guard 页里”之后，把现场 eip 换成
 * task_guard_exit —— 任务死掉、内核继续跑。若 guard 没生效，这次写会静默
 * 成功，打出 not-trapped（那就是失败的样子）。
 * 这个任务也带一个所有权载荷：它死掉之后 dtor 会拿到 reason=2，于是
 * “死因”不是靠嘴说的，而是从被守卫杀掉的那条路径里带出来的。 */
void demo_guard(void *arg)
{
    unsigned long bottom = task_stack_bottom();

    /* arg 是所有权载荷：真正收尾由 demo_guard_dtor 在任务死后做，这里只确认拿到了它 */
    if (!arg || ((struct own_job *)arg)->magic != OWN_MAGIC)
        guard_seen = 0;
    *(volatile unsigned long *)(bottom - 4) = 0xBADCAFEUL;

    /* 走到这里说明越界写没被拦住 */
    cpu_cli();
    rep_guard_not_trapped();
    cpu_sti();
}

/* 阶段三：A 一进来就返回（返回地址是 task_return_stub，会自动 task_exit） */
void demo_a(void *arg)
{
    (void)arg;
}

/* 阶段三的 C/B 两行合并成一行（屏幕 24 行已满，ring3 那行要挤进来）。
 * 合并之后“C 先醒”这条证据不能靠行序了，改成显式的 wake_seq：
 * 每醒一个就左移一位十进制推进一格，C 记 1、B 记 2 → seq 应为 0x12；
 * 如果顺序反了，seq 会变成 0x21，断言一眼就能看出来。 */
static void print_phase3_wake(unsigned long arg)
{
    cpu_cli();                      /* 一行输出期间不被抢占，避免两行交错 */
    rep_wake(c_slot, c_gap, b_slot, b_gap, wake_seq, arg);
    cpu_sti();
}

/* D：由 B 动态创建，打出自己的 slot 与收到的参数 */
static void worker_d(void *arg);

/* B：睡 100ms，醒来记下自己的数据并打出合并行，再带着 arg 动态创建 D，然后返回 */
void demo_b(void *arg)
{
    unsigned long start = timer_ticks();

    task_sleep(100);
    b_slot = (unsigned long)task_self();
    b_gap = timer_ticks() - start;
    wake_seq = wake_seq * 16 + 2;   /* B 是第二个醒的 */
    print_phase3_wake((unsigned long)arg);
    task_spawn(worker_d, arg, 0, 2);
}

/* C：睡 50ms——比 B 晚创建却更早醒，证明睡眠队列按到期时间排序 */
void demo_c(void *arg)
{
    unsigned long start = timer_ticks();

    (void)arg;
    task_sleep(50);
    c_slot = (unsigned long)task_self();
    c_gap = timer_ticks() - start;
    wake_seq = wake_seq * 16 + 1;   /* C 是第一个醒的 */
}

/* D：由 B 动态创建，打出自己的 slot 与收到的参数 */
static void worker_d(void *arg)
{
    cpu_cli();
    /* lt=被借用表拒绝的次数 / 表的历史最高水位：证明“记不下就不借”和表容量 */
    rep_d_slot((unsigned long)task_self(), (unsigned long)arg,
               lease_denied_in, lease_peak_in);
    cpu_sti();
}

/* 跑一段 worker、等全部退出：三个阶段共用 */
void demo_run_workers(void)
{
    task_sched_on();
    cpu_sti();
    while (task_active())
        cpu_hlt();                  /* worker 全在睡时落到这里空转（slot 0） */
    cpu_cli();
    task_reclaim();                 /* 最后一个退出的栈页，换回 main 之后才还 */
}
