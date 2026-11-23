/* ==============================================================
 * CMOS/RTC：秒计数 + 完整日期
 *
 * 秒计数的用途是**独立交叉验证**：PIT 频率改了没有，光看 tick 数字看不出来；
 * RTC 由另一路晶振驱动，读它的秒跳变当"墙钟"，数两个跳变之间的 tick 数，
 * 就能确定 PIT 到底是 18.2Hz 还是 100Hz。
 *
 * 对齐整秒必须用"更新结束"标志（reg C 的 bit4 UF），而不是自己轮询秒值：
 * 秒值在更新瞬间可能读到撕裂值，轮询会误判出一个偏短的"秒"。UF 由硬件在
 * 每次更新后置位、读 reg C 时清零，因此"清标志 -> 等标志 -> 读数"得到的
 * 两个时刻刚好相差 1.000 秒。
 *
 * 完整日期（`rtc_now`）要读七个寄存器，撕裂的后果比秒计数严重得多：
 * 23:59:59 读到 00:00:59 这种"跨天不跨小时"的组合是可能的，换算出来是个
 * 合法但错误的时刻，**没有任何事后检查能发现**。所以这里用最笨也最可靠的
 * 办法：等 UIP 清零 -> 整块读 -> 等 UIP 清零 -> 再整块读，两次逐字节相同
 * 才认；连续 4 次都不一致就返回 -1（宁可不给时间，也不给错时间）。
 *
 * 寄存器通过 0x70（索引）/0x71（数据）访问；索引字节的 bit7 顺手屏蔽 NMI
 * （0x70 的 bit7 就是 NMI disable，写 0 会让 NMI 使能状态受后续字节影响）。
 * 0x0A 的 bit7 是 UIP（update in progress），置位期间读数据不可靠，必须先等它清零。
 * ============================================================== */
#include "linux/rtc.h"
#include "asm/io.h"

#define CMOS_INDEX     0x70
#define CMOS_DATA      0x71
#define RTC_SECONDS    0x00
#define RTC_MINUTES    0x02
#define RTC_HOURS      0x04
#define RTC_DAY        0x07
#define RTC_MONTH      0x08
#define RTC_YEAR       0x09
#define RTC_CENTURY    0x32   /* 世纪寄存器：不是所有实现都有，没有时返回 0xFF */
#define RTC_STATUS_A   0x0A
#define RTC_STATUS_B   0x0B
#define RTC_STATUS_C   0x0C

#define STATUS_A_UIP   0x80
#define STATUS_B_24H   0x02   /* bit1=1 表示 24 小时制 */
#define STATUS_B_BIN   0x04   /* bit2=1 表示走二进制而不是 BCD */
#define STATUS_C_UF    0x10   /* bit4=更新结束标志，读 reg C 时自动清零 */
#define HOUR_PM        0x80   /* 12 小时制下 hour 的 bit7 = 下午 */

static unsigned char cmos_read(unsigned char reg)
{
    outb(CMOS_INDEX, (unsigned char)(reg | 0x80));   /* bit7=1：屏蔽 NMI */
    return inb(CMOS_DATA);
}

/* 等 UIP 清零，最多等 20000 次，避免 RTC 异常时死循环 */
static void wait_uip(void)
{
    unsigned int guard = 0;

    while ((cmos_read(RTC_STATUS_A) & STATUS_A_UIP) && guard++ < 20000)
        ;
}

/* BCD -> 二进制；已经是二进制的寄存器原样返回 */
static unsigned int cv(unsigned char raw, int bin)
{
    if (bin)
        return raw;
    return (unsigned int)(raw & 0x0F) + (unsigned int)(raw >> 4) * 10;
}

/* 一次读齐七个寄存器（顺序 = 下面 struct 的顺序：秒 分 时 日 月 年 世纪） */
static void read_raw(unsigned char *raw)
{
    raw[0] = cmos_read(RTC_SECONDS);
    raw[1] = cmos_read(RTC_MINUTES);
    raw[2] = cmos_read(RTC_HOURS);
    raw[3] = cmos_read(RTC_DAY);
    raw[4] = cmos_read(RTC_MONTH);
    raw[5] = cmos_read(RTC_YEAR);
    raw[6] = cmos_read(RTC_CENTURY);
}

unsigned int rtc_seconds(void)
{
    unsigned char raw;

    wait_uip();
    raw = cmos_read(RTC_SECONDS);
    return cv(raw, (cmos_read(RTC_STATUS_B) & STATUS_B_BIN) ? 1 : 0);   /* 0..59 */
}

int rtc_now(struct tm *out)
{
    unsigned char a[7], b[7], sb;
    unsigned int sec, min, hour, mday, mon, year, yy, cen;
    int bin, h24, tries, i;
    long ep;
    struct tm t;

    if (!out)
        return -1;

    /* 两遍整块读，逐字节比对 */
    for (tries = 0; tries < 4; tries++) {
        wait_uip();
        read_raw(a);
        wait_uip();
        read_raw(b);
        for (i = 0; i < 7 && a[i] == b[i]; i++)
            ;
        if (i == 7)
            break;
    }
    if (tries == 4)
        return -1;

    sb = cmos_read(RTC_STATUS_B);
    bin = (sb & STATUS_B_BIN) ? 1 : 0;
    h24 = (sb & STATUS_B_24H) ? 1 : 0;

    sec = cv(a[0], bin);
    min = cv(a[1], bin);

    /* 12 小时制：bit7 是"下午"，12 点是分界点（上午 12 点 = 0 点，下午 12 点 = 12 点） */
    if (h24) {
        hour = cv(a[2], bin);
    } else {
        unsigned int pm = (a[2] & HOUR_PM) ? 1 : 0;

        hour = cv((unsigned char)(a[2] & (unsigned char)~HOUR_PM), bin);
        if (pm && hour != 12)
            hour += 12;
        else if (!pm && hour == 12)
            hour = 0;
    }

    mday = cv(a[3], bin);
    mon = cv(a[4], bin);                    /* 1..12 */
    yy = cv(a[5], bin);
    cen = cv(a[6], bin);

    /* 世纪：19/20/21 才认；0xFF（寄存器不存在）或 0 一律按 20xx 解释。
     * 这条是明确的取舍：这台机器不会跑到 2100 年以后，而"猜错世纪"比
     * "拒绝给时间"更糟——所以在猜之前先说清楚这里是猜的。 */
    if (cen == 19 || cen == 20 || cen == 21)
        year = cen * 100 + yy;
    else
        year = 2000 + yy;

    t.sec = (int)sec;
    t.min = (int)min;
    t.hour = (int)hour;
    t.mday = (int)mday;
    t.mon = (int)mon - 1;                   /* 寄存器 1..12 -> struct tm 的 0..11 */
    t.year = (int)year;
    t.wday = 0;
    t.yday = 0;

    /* 合法性交给日历层判：2 月 30 日、小时 25 这类垃圾在这里被挡掉。
     * 反过来再由 gmtime 填 wday/yday，保证"同一个时刻只有一种表示"。 */
    if (time_mktime(&t, &ep) != 0)
        return -1;
    if (time_gmtime(ep, out) != 0)
        return -1;
    return 0;
}

void rtc_clear_flags(void)
{
    (void)cmos_read(RTC_STATUS_C);       /* 读一次把 UF/PF/AF 等标志清干净 */
}

int rtc_update_flag(void)
{
    return (cmos_read(RTC_STATUS_C) & STATUS_C_UF) ? 1 : 0;
}
