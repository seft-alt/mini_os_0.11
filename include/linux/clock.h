/* ==============================================================
 * 墙钟：把 RTC 的"绝对时间"和 PIT 的"经过时间"绑成一个
 *
 * 两路时间各有一个致命缺点，正好互补：
 *   - RTC 有绝对值（年/月/日），但读数慢、会撕裂、频率由主板晶振决定；
 *   - PIT tick 单调、便宜到随手就能读，但没有"我是什么时候"这回事，
 *     而且在 QEMU/TCG 下 100Hz 实测只有 70 上下——**它不是秒表**。
 *
 * 做法和所有操作系统的做法一样：开机用 RTC 定一次锚点，之后靠 tick 走时，
 * 再定期用 RTC 把走时拉回来（`clock_resync`）。锚点只定一次，校准只改
 * "基准"，不改 tick 计数本身——所以 `timer_ticks()` 永远是单调的，
 * 而 `clock_epoch()` 是被校准过的绝对时间，允许被往回拨。
 *
 * 两条性质分开承诺，不要混：
 *   `timer_uptime_ms()`  单调不减，与绝对时间无关（日志排序用它）；
 *   `clock_epoch()`      绝对、可校准，可能被 RTC 纠正（"现在几点"用它）。
 *
 * 校准的幅度有上限（`CLOCK_MAX_STEP` 秒）：差值超过它说明 RTC 读到了垃圾
 * （世纪寄存器缺失、年份错位都会造成这种大跳），这时候**拒绝**比"照抄"安全。
 * ============================================================== */
#ifndef MINI_OS_CORE_CLOCK_H
#define MINI_OS_CORE_CLOCK_H

/* 开机定锚：读一次 RTC 当基准，同时记下那一刻的 tick。
 * 返回 0 = 有基准；-1 = RTC 读不稳/日期非法（此时 `clock_ready()` 为 0，
 * 上层必须退回直读 RTC 的路径，而不是假装有时间）。 */
int clock_init(void);

int clock_ready(void);

/* 现在的绝对时间（纪元秒，UTC）。没有基准时返回 -1。
 * 注意它可能因为校准而**变小**（最多回退 CLOCK_MAX_STEP 秒）。 */
long clock_epoch(void);

/* 当前这一秒已经走了多少毫秒（0..990，10ms 一档）。
 * 单独给这个数是因为"纪元毫秒"在 32 位 long 里只装得下 24 天。 */
unsigned int clock_ms_of_second(void);

/* 立刻用 RTC 校准一次。返回：0 = 已校准（可能差值为 0，即本来就不偏）；
 * -1 = RTC 读不稳；-2 = 差值超过 CLOCK_MAX_STEP，拒绝采纳。 */
int clock_resync(void);

/* **只给测试用**：把钟的实际误差设成 secs 秒（先读 RTC 重新定锚，再让
 * `clock_epoch()` 比 RTC 慢 secs 秒）。它不动 ticks，所以"已经过了多久"
 * 不受影响，被歪掉的只有绝对时间。于是可以确定地验证两件事：
 *   1. 校准能把错的基准拉回来（secs=20 → sync 报 d≈+20）；
 *   2. 超出封顶会被拒（secs=90 → sync 返回 -2，钟仍歪着）。
 * 收尾用 `clock skew 0`（误差设 0），不需要输入负数。
 * 生产路径没人调它；名字不叫 set_ 就是为了让它看起来不像个正经接口。
 * 返回 0 / -1（还没定锚、secs 越界（0..600）、或 RTC 读不稳）。 */
int clock_skew(int secs);

/* 空闲循环里调：距离上次校准超过 CLOCK_SYNC_TICKS 就自动校准一次。
 * 放在 idle 而不是时钟中断里，是因为读 CMOS 要等 UIP（最长 1 个更新周期），
 * 那是"毫秒级忙等"，进中断会把 10ms 的片长吃掉一截。 */
void clock_poll(void);

/* 基准与统计（证据用） */
long clock_base_epoch(void);
unsigned long clock_base_tick(void);
unsigned int clock_syncs(void);       /* 成功的校准次数 */
unsigned int clock_rejects(void);     /* 差值越界被拒的次数 */
unsigned int clock_read_fails(void);  /* RTC 读不稳的次数 */
int clock_last_delta(void);           /* 最近一次 RTC - 预测（秒，可负） */
unsigned int clock_max_step(void);    /* 历史上最大的 |delta|（秒） */

#endif /* MINI_OS_CORE_CLOCK_H */
