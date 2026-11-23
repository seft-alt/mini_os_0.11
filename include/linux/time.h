/* ==============================================================
 * 日历时间：`struct tm` 与"纪元秒 <-> 年月日时分秒"的换算
 *
 * 分两层，互不依赖：
 *   1. 纯算术（本文件）：给一个日期就能算出纪元秒，给一个纪元秒就能反算出
 *      年月日、星期、年内第几天。全部是常量时间，不查表、不循环。
 *   2. 硬件读数（`drivers/char/rtc.h` 的 `rtc_now`）：从 CMOS 读一份日期，
 *      再用这里的换算补齐 wday/yday。
 *
 * 边界写在函数返回值里，不写在注释里：**能算就返回 0，算不了就返回 -1**。
 * "算不了"包括：月份越界、2 月 30 日、`long` 装不下的纪元秒（i386 上
 * `long` 是 32 位，2038-01-19 03:14:08 之后就装不下了）。
 *
 * 时区：内核一律按 **UTC** 算，RTC 读到什么就按什么算（QEMU 默认
 * `-rtc base=utc`，所以读到的是 UTC）。没有本地时区、没有夏令时——
 * 那是"需要一张时区表 + 一个偏移"的事，现在不做。
 * ============================================================== */
#ifndef MINI_OS_LIB_TIME_H
#define MINI_OS_LIB_TIME_H

/* 与 C 库的 `struct tm` 同名同义，只有两点不同：
 *   - `year` 是**完整年份**（2000），不是"距 1900 的年数"——那个约定在
 *     内核里从来只会让人写错 `t->year + 1900`；
 *   - 没有 `isdst`：没有时区，就没有这个字段。 */
struct tm {
    int sec;      /* 0..59（不接受 60 的闰秒：装不下就报错，不假装装下） */
    int min;      /* 0..59 */
    int hour;     /* 0..23，永远 24 小时制（RTC 的 12 小时制在驱动里就换算掉） */
    int mday;     /* 1..31 */
    int mon;      /* 0..11，0 = 一月：和 C 库一致，和"人话"不一致，打印时才转 */
    int year;     /* 完整年份，如 2026 */
    int wday;     /* 0..6，0 = 周日 */
    int yday;     /* 0..365，0 = 一月一日 */
};

/* 闰年：能被 4 整除，但整百年要能被 400 整除。 */
int time_is_leap(int year);

/* 某年某月的天数（mon 0..11）；mon 越界返回 0。 */
int time_days_in_month(int year, int mon);

/* 时 -> 纪元秒（1970-01-01 00:00:00 UTC 起算）。
 * 只读 `sec/min/hour/mday/mon/year`，`wday/yday` 被忽略（并会重新算）。
 * 返回 0 = 成功；-1 = 字段非法或超出 `long` 的表示范围（不写 *out）。 */
int time_mktime(const struct tm *t, long *out);

/* 纪元秒 -> 时。`wday`/`yday` 一并填好。返回 0 / -1（*out 为空指针时）。 */
int time_gmtime(long epoch, struct tm *out);

/* 格式化成 "YYYY-MM-DD HH:MM:SS"（19 字节 + NUL，所以 cap 至少 20）。
 * 返回写出的字节数（19）；cap 不够或指针为空返回 0。 */
unsigned int time_iso(const struct tm *t, char *buf, unsigned int cap);

/* 名字表：都是固定的 3 字母，直接给 `printk`/shell 拼行用。 */
const char *time_wday_name(int wday);     /* "Sun".."Sat"，越界给 "---" */
const char *time_mon_name(int mon);       /* "Jan".."Dec"，越界给 "---" */

/* 不依赖硬件的自检（`date chk` 的数据源）：闰年、月末、已知纪元秒、
 * 2038 边界、非法字段、以及"算过去再算回来必须一模一样"的往返。
 * 返回失败条数；*total 写总条数，*first_fail 写第一条失败的编号（0 = 没有失败）。 */
unsigned int time_selftest(unsigned int *total, unsigned int *first_fail);

#endif /* MINI_OS_LIB_TIME_H */
