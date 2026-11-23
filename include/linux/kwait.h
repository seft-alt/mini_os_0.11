/* ==============================================================
 * 等事件那一层的自检（kernel/kwait.c）
 *
 * 两个 worker 用一个静态“信箱”做 16 轮 ping-pong：writer 摆数据后
 * wakeup(c_data) 去等确认，reader 被叫醒后收数据、wakeup(c_ack) 再等下一轮。
 * 这条链路要能同时证明四件事：
 *   1. 睡下去的任务真的把 CPU 让了出去（否则对端一轮也跑不动）；
 *   2. 唤醒后回到就绪队列、还能被调度到（16 轮数据全部顺序正确）；
 *   3. 等待/唤醒次数对得上（不多叫、不漏叫）；
 *   4. 收工后没有槽位还挂在通道上（没有“睡死”的残留）。
 *
 * 超时兜底：main 侧等这两端时用一个 tick 上限（约 2 秒），超了就置 abort
 * 并把两端全叫醒——它们从 cond 里看到 abort 就退出。没有这个兜底，
 * 一次丢唤醒会让 shell 永久挂住，回归脚本只能靠 timeout 猜。
 * ============================================================== */
#ifndef MINI_OS_KWAIT_H
#define MINI_OS_KWAIT_H

/* 跑一趟自检：成功 1、失败 0。会 spawn 两个任务并等它们全部退出。 */
int kwait_selftest(void);

/* 证据（跑完一趟之后读；都是上一趟的快照） */
unsigned int kwait_rounds(void);     /* 完成的轮数 */
unsigned int kwait_ok(void);         /* 16 轮数据全部对得上 = 1 */
unsigned int kwait_timeout(void);    /* 被 tick 上限兜底打断 = 1 */
unsigned int kwait_waits(void);      /* 本趟等待次数（两个任务合计） */
unsigned int kwait_wakeups(void);    /* 本趟唤醒次数 */
int          kwait_left(void);       /* 收工后还在等的槽位数（应为 0） */

#endif /* MINI_OS_KWAIT_H */
