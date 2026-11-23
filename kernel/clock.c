/* core/clock.c —— 墙钟（契约见 include/core/clock.h）
 *
 * 关键细节只有三个：
 *   1. 走时用 `timer_ticks() - base_tick`（unsigned 减法）：ticks 是 32 位、
 *      约 497 天回绕一次，直接减会让"回绕之后"变成负数，这里靠无符号模运算
 *      自动跨过回绕点；
 *   2. tick 不是秒表：TCG 下 100Hz 实测只有 70，所以 tick 走的秒数只能当
 *      "预测"，定期用 RTC 修正。修正只挪基准，不动 ticks；
 *   3. 修正幅度封顶：RTC 读数一旦是垃圾（年份错位、世纪寄存器缺失），
 *      差值会是几亿秒——照抄就是把系统时间扔到 1970 或 2100。
 */
#include "linux/clock.h"
#include "linux/printk.h"
#include "linux/rtc.h"
#include "linux/timer.h"
#include "linux/time.h"

/* 每 300 秒（按 tick 算）自动校准一次。选 300 而不是 60：
 * TCG 下 tick 偏慢约 30%，300 秒会攒出 ~90 秒误差，60 秒攒 ~18 秒——
 * 校准得越勤，单次跳得越小，但每次都要读一次 CMOS（毫秒级忙等）。
 * 300 秒是"误差还能被秒级显示容忍"与"不打搅空闲循环"之间的折中。 */
#define CLOCK_SYNC_TICKS  30000UL

/* 单次校准的封顶：超过 60 秒的差值不采纳，只记账 */
#define CLOCK_MAX_STEP    60

static int ready;
static long base_epoch;
static unsigned long base_tick;
static unsigned long last_sync_tick;

static unsigned int syncs, rejects, read_fails, max_step;
static int last_delta;

int clock_init(void)
{
    struct tm t;
    long ep;

    ready = 0;
    if (rtc_now(&t) != 0 || time_mktime(&t, &ep) != 0) {
        read_fails++;
        return -1;
    }
    /* 先取 RTC 再取 tick：倒过来会把"读 RTC 花掉的时间"算进基准里，
     * 于是每次开机都凭空快出几毫秒（虽然这里只到秒，但顺序要先对）。 */
    base_epoch = ep;
    base_tick = timer_ticks();
    last_sync_tick = base_tick;
    last_delta = 0;
    ready = 1;
    return 0;
}

int clock_ready(void)
{
    return ready;
}

long clock_epoch(void)
{
    unsigned long dt;

    if (!ready)
        return -1;
    dt = timer_ticks() - base_tick;          /* 无符号：跨 32 位回绕也对 */
    return base_epoch + (long)(dt / TIMER_HZ);
}

unsigned int clock_ms_of_second(void)
{
    unsigned long dt;

    if (!ready)
        return 0;
    dt = timer_ticks() - base_tick;
    return (unsigned int)((dt % TIMER_HZ) * (1000UL / TIMER_HZ));
}

int clock_resync(void)
{
    struct tm t;
    long rtc_ep, pred, delta, mag;

    if (!ready)
        return -1;
    if (rtc_now(&t) != 0 || time_mktime(&t, &rtc_ep) != 0) {
        read_fails++;
        return -1;
    }

    pred = clock_epoch();
    delta = rtc_ep - pred;
    mag = (delta < 0) ? -delta : delta;

    last_delta = (int)delta;
    if ((unsigned int)mag > max_step)
        max_step = (unsigned int)mag;

    if ((unsigned int)mag > CLOCK_MAX_STEP) {
        rejects++;
        return -2;
    }

    /* 只有真的偏了才动基准：差 0 也重设 base 的话，会把"这一秒已经走了
     * 多少毫秒"抹掉（`clock_ms_of_second` 会跳回 0）。 */
    if (delta != 0) {
        base_epoch = rtc_ep;
        base_tick = timer_ticks();
    }
    last_sync_tick = timer_ticks();
    syncs++;
    return 0;
}

int clock_skew(int secs)
{
    struct tm t;
    long rtc_ep;

    if (!ready)
        return -1;
    if (secs < 0 || secs > 600)
        return -1;
    if (rtc_now(&t) != 0 || time_mktime(&t, &rtc_ep) != 0) {
        read_fails++;
        return -1;
    }
    /* 先重新定锚到"现在"，再减去 secs：误差正好是 secs，而不是"上一次误差
     * 加上 secs"——测试要的是可复现，不是可累积。 */
    base_epoch = rtc_ep - (long)secs;
    base_tick = timer_ticks();
    return 0;
}

void clock_poll(void)
{
    if (!ready)
        return;
    if ((timer_ticks() - last_sync_tick) >= CLOCK_SYNC_TICKS)
        (void)clock_resync();
}

long clock_base_epoch(void)
{
    return ready ? base_epoch : -1;
}

unsigned long clock_base_tick(void)
{
    return base_tick;
}

unsigned int clock_syncs(void)
{
    return syncs;
}

unsigned int clock_rejects(void)
{
    return rejects;
}

unsigned int clock_read_fails(void)
{
    return read_fails;
}

int clock_last_delta(void)
{
    return last_delta;
}

unsigned int clock_max_step(void)
{
    return max_step;
}
