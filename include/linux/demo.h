/* ==============================================================
 * init/demo.h：自检的“演示载荷”接口（实现在 init/demo.c）
 *
 * 为什么把这些搬出 main.c：main() 的职责是“按顺序把东西装起来、跑一遍、
 * 把结果打出去”。载荷函数（跑到死的、睡着的、故意越界的）属于**实验材料**，
 * 它们的状态（期限 tick、证据全局）和编排逻辑混在一个文件里，加一个实验就要
 * 在多处改来改去。分开之后：
 *   - main.c 只调用 demo_* 入口、再从 demo_* 取数打印；
 *   - demo.c 拥有载荷本身的所有状态，外面拿不到裸全局，只能通过下面这些
 *     “取数”函数读，所以不会有人在别处顺手改坏它。
 * ============================================================== */
#ifndef MINI_OS_DEMO_H
#define MINI_OS_DEMO_H

/* 阶段计时：所有“跑满 12 tick 就返回”的载荷都看这一个期限。
 * 每开一段 worker 之前调一次（timer_ticks() + 12 之类）。 */
void demo_run_begin(unsigned long deadline_ticks);

/* ---- 所有权载荷（阶段七：dtor 收到死因；阶段三：正常 return 死因为 0）---- */
/* 取载荷：会重填 magic/id 并清掉上一轮的证据，返回可直接给 task_spawn_owned 的指针 */
void *demo_own_payload(void);
void demo_own_dtor(void *p, int reason);
unsigned long demo_own_magic(void);     /* 0 = 析构把它清了 = 所有权交接完成 */
unsigned long demo_own_seen(void);      /* 任务自己确认过载荷内容 */
unsigned long demo_own_why(void);       /* dtor 收到的死因 */

/* ---- guard 页载荷（阶段八：故意把栈写穿）---- */
void *demo_guard_payload(void);
void demo_guard_dtor(void *p, int reason);
unsigned long demo_guard_why(void);     /* 应为 2 = 被守卫杀掉 */

/* ---- 阶段六：经 int 0x31 拿到的三个返回值 ---- */
unsigned long demo_tune_old(void);
unsigned long demo_tune_bad(void);
unsigned long demo_tune_kept(void);

/* ---- 阶段五：醒来顺序的打包值（高半字节在前）---- */
unsigned long demo_wake_order(void);
void demo_wake_order_reset(void);
/* 阶段三的醒来序号（C 先醒应打成 0x12）：跑之前清零 */
void demo_wake_seq_reset(void);

/* 借用表统计：租约演示跑完之后由 main 填，worker_d 打行时读 */
void demo_set_lease_stats(unsigned long denied, unsigned long peak);

/* ---- 载荷入口（签名与 task_spawn 的 void (*)(void *arg) 一致）---- */
void demo_owned(void *arg);
void demo_burn(void *arg);
void demo_lease(void *arg);
void demo_lease_dtor(void *arg, int reason);
unsigned long demo_lease_notice(void);   /* 1 = 撤销通知来了且时序正确 */
void demo_yield(void *arg);
void demo_decay(void *arg);
void demo_guard(void *arg);
void demo_a(void *arg);
void demo_b(void *arg);
void demo_c(void *arg);
void demo_sleep_wake(void *arg);

/* 跑一段 worker 并等全部退出（阶段三/四/六/七共用） */
void demo_run_workers(void);

#endif /* MINI_OS_DEMO_H */
