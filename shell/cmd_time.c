/* 时间类命令：clock / date。
 *
 * 由 shell/cmd_info.c 再拆出来（纯搬运，逻辑未改）。
 * 为什么单独一层：这两条各自都是一整段“参数分派 + 拼一行输出”，
 * 和观测类（stat/irq/tasks/log/heap/kstress）不共享任何状态；
 * 拆走之后 cmd_info.c 只剩“看一眼计数器”，时钟相关的 include 也不用再挂着。
 * ============================================================== */
#include "linux/shell.h"
#include "linux/state.h"
#include "linux/console.h"
#include "linux/fmt.h"
#include "linux/cmds.h"
#include "linux/clock.h"
#include "linux/rtc.h"
#include "linux/timer.h"
#include "linux/time.h"

/* 带符号十进制（只给 `clock skew` 用）：失败返回 -1，不进位不猜。
 * 上限 700 是“把钟弄歪”的合理范围，顺便挡住溢出。 */
static int parse_int(const char *s, unsigned int len, int *out)
{
    int v = 0, neg = 0;
    unsigned int i = 0;

    if (len == 0)
        return -1;
    if (s[0] == '-' || s[0] == '+') {
        neg = (s[0] == '-');
        i = 1;
        if (len == 1)
            return -1;
    }
    for (; i < len; i++) {
        if (s[i] < '0' || s[i] > '9')
            return -1;
        v = v * 10 + (int)(s[i] - '0');
        if (v > 700)
            return -1;
    }
    *out = neg ? -v : v;
    return 0;
}

/* `clock`：两路时间对齐的证据与手动校准。
 *
 *   clock        一次看全：ep（校准后的绝对时间）、ms（这一秒走了多少毫秒）、
 *                t（tick 计数，hex）、d（最近一次 RTC-预测）、
 *                s/r/x（成功校准次数 / 越界拒绝次数 / 历史最大差值）
 *   clock sync   立刻校准一次；rc 后面跟 d=<差值>
 *
 * 为什么 d 值得看：TCG 下 tick 比真时间慢 30%，`d` 会是一串正数——
 * "tick 当秒表使"这件事错在哪，这个数字就是证据。
 */
int cmd_clock(void)
{
    char line[SHELL_OUT_COLS];
    unsigned int k;
    int rc;

    if (shell_argc > 1 && shell_argv_len[1] == 4
        && shell_argv_buf[1][0] == 's' && shell_argv_buf[1][1] == 'y'
        && shell_argv_buf[1][2] == 'n' && shell_argv_buf[1][3] == 'c') {
        rc = clock_resync();
        k = put_s(line, "clk sync rc=");
        if (rc == 0) {
            line[k++] = '0';
        } else {
            line[k++] = 'E';
            line[k++] = (char)('0' + (int)(-rc));
        }
        k += put_s(line + k, " d=");
        k += sdec(line + k, clock_last_delta());
        k += put_s(line + k, " s=");
        k += udec(line + k, clock_syncs());
        k += put_s(line + k, " r=");
        k += udec(line + k, clock_rejects());
        shell_out_puts(line, k);
        return SHELL_RC_OK;
    }

    /* clock skew <秒>：测试用的"把钟弄歪"，见 include/core/clock.h 的说明 */
    if (shell_argc > 2 && shell_argv_len[1] == 4
        && shell_argv_buf[1][0] == 's' && shell_argv_buf[1][1] == 'k'
        && shell_argv_buf[1][2] == 'e' && shell_argv_buf[1][3] == 'w') {
        int secs = 0, rc;

        rc = (parse_int(shell_argv_buf[2], shell_argv_len[2], &secs) == 0)
             ? clock_skew(secs) : -1;
        k = put_s(line, "clk skew rc=");
        if (rc == 0) {
            line[k++] = '0';
        } else {
            line[k++] = 'E';
            line[k++] = (char)('0' + (int)(-rc));
        }
        k += put_s(line + k, " err=");
        k += udec(line + k, (unsigned long)secs);
        shell_out_puts(line, k);
        return SHELL_RC_OK;
    }

    if (!clock_ready()) {
        shell_out_puts("clk no base (rtc unreadable)", 26);
        return SHELL_RC_OK;
    }
    k = put_s(line, "clk ep=");
    k += udec(line + k, (unsigned long)clock_epoch());
    k += put_s(line + k, " ms=");
    k += u3(line + k, clock_ms_of_second());
    k += put_s(line + k, " t=");
    k += hex6(line + k, timer_ticks());
    k += put_s(line + k, " d=");
    k += sdec(line + k, clock_last_delta());
    k += put_s(line + k, " s=");
    k += udec(line + k, clock_syncs());
    k += put_s(line + k, " r=");
    k += udec(line + k, clock_rejects());
    k += put_s(line + k, " x=");
    k += udec(line + k, clock_max_step());
    shell_out_puts(line, k);
    return SHELL_RC_OK;
}

/* `date`：日历层的三条入口，都落在一行里。
 *
 *   date            读 RTC 的完整日期，按 ISO 打出来（唯一依赖硬件的用法）
 *   date <纪元秒>    纯算术反算，给定时刻打 UTC 年月日——**可复现**，测试靠它
 *   date chk        纯算术自检（闰年/月末/两个锚点/往返/2038 边界/非法字段/格式化）
 *
 * 为什么把自检做成命令而不是"启动时打一行"：自检的价值在于**能单独按一次**，
 * 而启动屏的 24 行是按行算死的，多打一行就挤掉证据。
 */

int cmd_date(void)
{
    char line[SHELL_OUT_COLS];
    struct tm t;
    unsigned int k, n, bad = 0, fails, j;
    long ep = 0;

    /* ---- date chk：只看算术 ---- */
    if (shell_argc > 1 && shell_argv_len[1] == 3
        && shell_argv_buf[1][0] == 'c' && shell_argv_buf[1][1] == 'h'
        && shell_argv_buf[1][2] == 'k') {
        fails = time_selftest(&n, &bad);
        k = put_s(line, "date chk n=");
        k += udec(line + k, n);
        k += put_s(line + k, " ok=");
        k += udec(line + k, n - fails);
        if (fails) {
            k += put_s(line + k, " first=");
            k += udec(line + k, bad);
        }
        shell_out_puts(line, k);
        return SHELL_RC_OK;
    }

    /* ---- date <纪元秒>：从数字到年月日，结果与机器当前时间无关 ---- */
    if (shell_argc > 1) {
        unsigned long v = 0;
        int ok = (shell_argv_len[1] > 0) ? 1 : 0;

        for (j = 0; j < shell_argv_len[1]; j++) {
            char c = shell_argv_buf[1][j];

            if (c < '0' || c > '9') {
                ok = 0;
                break;
            }
            v = v * 10 + (unsigned long)(c - '0');
            if (v > 2147483647UL) {           /* 32 位 long 的边界，超过就不猜 */
                ok = 0;
                break;
            }
        }
        if (!ok || time_gmtime((long)v, &t) != 0) {
            shell_out_puts("date bad epoch (0..2147483647)", 30);
            return SHELL_RC_OK;
        }

        k = put_s(line, "utc ");
        n = time_iso(&t, line + k, SHELL_OUT_COLS - k);
        if (n == 0) {
            shell_out_puts("date iso failed", 16);
            return SHELL_RC_OK;
        }
        k += n;
        k += put_s(line + k, " ");
        k += put_s(line + k, time_wday_name(t.wday));
        k += put_s(line + k, " yd=");
        k += u3(line + k, (unsigned int)t.yday);
        k += put_s(line + k, " ep=");
        k += udec(line + k, (unsigned long)v);
        shell_out_puts(line, k);
        return SHELL_RC_OK;
    }

    /* ---- date：墙钟给的"现在"（tick 走时 + RTC 定锚/校准）；
     * 没有基准（开机那一刻 RTC 读不稳）才退回直读 RTC ——
     * 那样拿到的是"硬件的说法"，没经过走时校准，`clock` 命令里能看到 s=0。 ---- */
    if (clock_ready()) {
        ep = clock_epoch();
        if (time_gmtime(ep, &t) != 0) {
            shell_out_puts("date clk bad", 12);
            return SHELL_RC_OK;
        }
    } else if (rtc_now(&t) != 0 || time_mktime(&t, &ep) != 0) {
        shell_out_puts("date rtc unstable", 17);
        return SHELL_RC_OK;
    }

    k = put_s(line, "date ");
    n = time_iso(&t, line + k, SHELL_OUT_COLS - k);
    if (n == 0) {
        shell_out_puts("date iso failed", 16);
        return SHELL_RC_OK;
    }
    k += n;
    k += put_s(line + k, " ");
    k += put_s(line + k, time_wday_name(t.wday));
    k += put_s(line + k, " yd=");
    k += u3(line + k, (unsigned int)t.yday);
    k += put_s(line + k, " ep=");
    k += udec(line + k, (unsigned long)ep);
    shell_out_puts(line, k);
    return SHELL_RC_OK;
}
