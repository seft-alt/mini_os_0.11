/* lib/time.c —— 日历算术（契约见 include/lib/time.h）
 *
 * 换算用的是 Howard Hinnant 的 `days_from_civil` / `civil_from_days`：
 * 先把"年月日"折成"距 1970-01-01 的天数"，再折成秒。相比"按年累加 + 按
 * 月累加 + 按闰年打补丁"的老写法，它的好处是**没有循环、没有分支判断
 * 闰月**，闰年规则被吸收进一个 400 年周期（`era`）的整数除法里，所以
 * 2100 年这种整百不闰的边界也不会算错。
 *
 * 边界意识：i386 上 `long` 是 32 位，纪元秒最大到 2038-01-19 03:14:07。
 * 这里**不**用 unsigned 去"多装一倍"——超过范围就是算不了，返回 -1 比返回
 * 一个回绕的数字诚实。
 */
#include "linux/time.h"

/* 平年各月天数；2 月由 time_days_in_month() 单独加一天 */
static const unsigned char mon_len[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
static const char *const wday_name[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *const mon_name[12] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

int time_is_leap(int year)
{
    if (year % 4)
        return 0;
    if (year % 100)
        return 1;
    return (year % 400) == 0 ? 1 : 0;
}

int time_days_in_month(int year, int mon)
{
    if (mon < 0 || mon > 11)
        return 0;
    if (mon == 1 && time_is_leap(year))
        return 29;
    return (int)mon_len[mon];
}

/* 年月日 -> 距 1970-01-01 的天数（可以为负；这里只会用到 1900 年以后） */
static long days_from_civil(int y, int mon, int d)
{
    int m = mon + 1;                       /* 折成 1..12 的"人话月" */
    long era;
    unsigned int yoe, doy, doe;

    y -= (m <= 2) ? 1 : 0;                 /* 把 1、2 月算进上一年，闰日就总在年末 */
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = (unsigned int)(y - era * 400);                       /* 0..399 */
    doy = (unsigned int)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);  /* 0..365 */
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;               /* 0..146096 */
    return era * 146097 + (long)doe - 719468;                  /* 719468 = 1970-01-01 */
}

/* 天数 -> 年月日（上面那条的逆运算） */
static void civil_from_days(long z, int *yy, int *mm, int *dd)
{
    long era;
    unsigned int doe, yoe, doy, mp;
    int y, m, d;

    z += 719468;
    era = (z >= 0 ? z : z - 146096) / 146097;
    doe = (unsigned int)(z - era * 146097);                    /* 0..146096 */
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;   /* 0..399 */
    y = (int)(yoe + era * 400);
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);              /* 0..365 */
    mp = (5 * doy + 2) / 153;                                   /* 0..11，3 月为 0 */
    d = (int)(doy - (153 * mp + 2) / 5 + 1);
    m = (int)(mp + (mp < 10 ? 3 : -9));                          /* 1..12 */
    *yy = y + (m <= 2);
    *mm = m - 1;
    *dd = d;
}

int time_mktime(const struct tm *t, long *out)
{
    long days;

    if (!t || !out)
        return -1;
    if (t->mon < 0 || t->mon > 11)
        return -1;
    /* 1900..2200：下界是"人类会读到的日期"，上界是 2 的 31 次方秒之外，
     * 真正的范围由下面 days 的检查兜住 —— 这里只是先挡掉明显的胡话 */
    if (t->year < 1900 || t->year > 2200)
        return -1;
    if (t->mday < 1 || t->mday > time_days_in_month(t->year, t->mon))
        return -1;
    if (t->hour < 0 || t->hour > 23 || t->min < 0 || t->min > 59)
        return -1;
    if (t->sec < 0 || t->sec > 59)
        return -1;

    days = days_from_civil(t->year, t->mon, t->mday);
    /* ±24855 天 = ±2147472000 秒：再大就装不进 32 位 long（2038 边界）。
     * 但"天数刚好卡在 24855"时，只要时分秒不是 0 就会越过 2^31-1（2038-01-19 03:14:08
     * 那一下），所以秒数用 unsigned 先算再判——直接用有符号加法在那一步已经是 UB 了。 */
    if (days > 24855 || days < -24855)
        return -1;
    if (days >= 0) {
        unsigned long secs = (unsigned long)days * 86400UL
                           + (unsigned long)t->hour * 3600UL
                           + (unsigned long)t->min * 60UL
                           + (unsigned long)t->sec;

        if (secs > 2147483647UL)
            return -1;
        *out = (long)secs;
    } else {
        *out = days * 86400L + (long)t->hour * 3600L + (long)t->min * 60L + t->sec;
    }
    return 0;
}

int time_gmtime(long epoch, struct tm *out)
{
    long days = epoch / 86400L;
    long rem = epoch % 86400L;
    int y, m, d;

    if (!out)
        return -1;
    if (rem < 0) {                          /* 1970 年以前：除法向零取整，得自己往下借一天 */
        rem += 86400L;
        days--;
    }

    civil_from_days(days, &y, &m, &d);
    out->year = y;
    out->mon = m;
    out->mday = d;
    out->hour = (int)(rem / 3600L);
    out->min = (int)((rem % 3600L) / 60L);
    out->sec = (int)(rem % 60L);
    out->wday = (int)(((days + 4) % 7 + 7) % 7);          /* 1970-01-01 是周四 */
    out->yday = (int)(days - days_from_civil(y, 0, 1));   /* 年内第几天，0 起 */
    return 0;
}

unsigned int time_iso(const struct tm *t, char *buf, unsigned int cap)
{
    unsigned int k = 0;

    if (!t || !buf || cap < 20)
        return 0;
    if (t->year < 0 || t->year > 9999)
        return 0;

    /* YYYY-MM-DD HH:MM:SS，19 字节，全部定宽补零 */
    buf[k++] = (char)('0' + (t->year / 1000) % 10);
    buf[k++] = (char)('0' + (t->year / 100) % 10);
    buf[k++] = (char)('0' + (t->year / 10) % 10);
    buf[k++] = (char)('0' + t->year % 10);
    buf[k++] = '-';
    buf[k++] = (char)('0' + (t->mon + 1) / 10);
    buf[k++] = (char)('0' + (t->mon + 1) % 10);
    buf[k++] = '-';
    buf[k++] = (char)('0' + t->mday / 10);
    buf[k++] = (char)('0' + t->mday % 10);
    buf[k++] = ' ';
    buf[k++] = (char)('0' + t->hour / 10);
    buf[k++] = (char)('0' + t->hour % 10);
    buf[k++] = ':';
    buf[k++] = (char)('0' + t->min / 10);
    buf[k++] = (char)('0' + t->min % 10);
    buf[k++] = ':';
    buf[k++] = (char)('0' + t->sec / 10);
    buf[k++] = (char)('0' + t->sec % 10);
    buf[k] = '\0';
    return k;
}

const char *time_wday_name(int wday)
{
    if (wday < 0 || wday > 6)
        return "---";
    return wday_name[wday];
}

const char *time_mon_name(int mon)
{
    if (mon < 0 || mon > 11)
        return "---";
    return mon_name[mon];
}

/* ---- 自检：不依赖 RTC，任何机器上结果都一样 ---- */

struct chk {
    unsigned int total;
    unsigned int fails;
    unsigned int first;                     /* 第一条失败的编号（1 起；0 = 没失败） */
};

static void chk_add(struct chk *c, int ok)
{
    c->total++;
    if (!ok) {
        c->fails++;
        if (c->first == 0)
            c->first = c->total;
    }
}

static int mk(const struct tm *t, long *ep)
{
    return time_mktime(t, ep) == 0;
}

unsigned int time_selftest(unsigned int *total, unsigned int *first_fail)
{
    struct chk c = { 0, 0, 0 };
    struct tm t, back;
    long ep;
    int i;

    /* 1. 闰年规则：整百不闰、被 400 整除才闰 */
    chk_add(&c, time_is_leap(2000) && time_is_leap(2024) && time_is_leap(1996));
    chk_add(&c, (!time_is_leap(2100)) && (!time_is_leap(1900)) && (!time_is_leap(2023)));

    /* 2. 月末天数：2 月要跟着闰年走 */
    chk_add(&c, time_days_in_month(2024, 1) == 29 && time_days_in_month(2023, 1) == 28);
    chk_add(&c, time_days_in_month(2024, 0) == 31 && time_days_in_month(2024, 3) == 30);
    chk_add(&c, time_days_in_month(2024, 12) == 0);      /* 月份越界要明确说"没有" */

    /* 3. 已知纪元秒：两个锚点，错了说明算法整体偏了 */
    t.year = 1970; t.mon = 0; t.mday = 1; t.hour = 0; t.min = 0; t.sec = 0;
    chk_add(&c, mk(&t, &ep) && ep == 0);
    t.year = 2000; t.mday = 1;
    chk_add(&c, mk(&t, &ep) && ep == 946684800L);

    /* 4. 反算：2000-01-01 是周六，年内第 0 天 */
    chk_add(&c, time_gmtime(946684800L, &back) == 0
                && back.year == 2000 && back.mon == 0 && back.mday == 1
                && back.hour == 0 && back.min == 0 && back.sec == 0
                && back.wday == 6 && back.yday == 0);

    /* 5. 闰日：2024-02-29 12:34:56 = 1709210096，反算是周四、年内第 59 天 */
    t.year = 2024; t.mon = 1; t.mday = 29; t.hour = 12; t.min = 34; t.sec = 56;
    chk_add(&c, mk(&t, &ep) && ep == 1709210096L);
    chk_add(&c, time_gmtime(1709210096L, &back) == 0
                && back.year == 2024 && back.mon == 1 && back.mday == 29
                && back.hour == 12 && back.min == 34 && back.sec == 56
                && back.wday == 4 && back.yday == 59);

    /* 6. 往返：随便挑 200 个时刻，算过去再算回来必须一模一样（含 wday/yday）。
     * 这一步比任何"手算的期望值"都有用：它能在不看代码的前提下抓出取整错误。 */
    {
        int rt_ok = 1;

        for (i = 0; i < 200; i++) {
            long probe = 946684800L + (long)i * 13337L;      /* 间隔 3h42m17s，能扫过各种余数 */

            if (time_gmtime(probe, &back) != 0 || time_mktime(&back, &ep) != 0 || ep != probe) {
                rt_ok = 0;
                break;
            }
        }
        chk_add(&c, rt_ok);
    }

    /* 7. 2038 边界：最后一秒能算，下一秒就该拒绝，而不是回绕成 1901 年 */
    t.year = 2038; t.mon = 0; t.mday = 19; t.hour = 3; t.min = 14; t.sec = 7;
    chk_add(&c, mk(&t, &ep) && ep == 2147483647L);
    t.sec = 8;
    chk_add(&c, time_mktime(&t, &ep) != 0);

    /* 8. 非法字段：2 月 30 日、24 点、12 月，必须返回 -1 而不是"就近凑一天" */
    t.year = 2023; t.mon = 1; t.mday = 30; t.hour = 0; t.min = 0; t.sec = 0;
    chk_add(&c, time_mktime(&t, &ep) != 0);
    t.mday = 1; t.hour = 24;
    chk_add(&c, time_mktime(&t, &ep) != 0);
    t.hour = 0; t.mon = 12;
    chk_add(&c, time_mktime(&t, &ep) != 0);

    /* 9. 格式化：定宽 19 字节，月/日/时/分/秒都要补零 */
    t.year = 2024; t.mon = 1; t.mday = 9; t.hour = 7; t.min = 5; t.sec = 3;
    {
        char buf[20];
        unsigned int n = time_iso(&t, buf, sizeof(buf));

        chk_add(&c, n == 19
                    && buf[0] == '2' && buf[1] == '0' && buf[2] == '2' && buf[3] == '4'
                    && buf[5] == '0' && buf[6] == '2'
                    && buf[8] == '0' && buf[9] == '9'
                    && buf[11] == '0' && buf[12] == '7'
                    && buf[14] == '0' && buf[15] == '5'
                    && buf[17] == '0' && buf[18] == '3'
                    && buf[19] == '\0');
        chk_add(&c, time_iso(&t, buf, 10) == 0);            /* 缓冲区不够：明确拒绝 */
    }

    /* 10. 名字表：越界不给垃圾指针 */
    chk_add(&c, time_wday_name(6)[0] == 'S' && time_wday_name(-1)[0] == '-');
    chk_add(&c, time_mon_name(11)[0] == 'D' && time_mon_name(12)[0] == '-');

    if (total)
        *total = c.total;
    if (first_fail)
        *first_fail = c.first;
    return c.fails;
}
