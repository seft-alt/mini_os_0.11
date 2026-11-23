/* 把数写成字符串：十六进制 / 十进制 / 有符号 / 字面量与返回码。
 *
 * 由 shell/shell.c 拆分而来（纯搬运，逻辑未改）。
 * ============================================================== */
#include "linux/shell.h"
#include "linux/state.h"
#include "linux/console.h"
#include "linux/fmt.h"
#include "linux/cmds.h"

/* 两位十六进制（`disk` 的字节 dump 用，不补前缀） */
unsigned int hex2b(char *dst, unsigned int v)
{
    const char *d = "0123456789ABCDEF";

    dst[0] = d[(v >> 4) & 0xF];
    dst[1] = d[v & 0xF];
    return 2;
}

/* 三位十六进制，定宽写到 dst，返回写了几个字符。
 * 这里不用 %03X：shell 的输出要拼进 79 列缓冲，自己接字符比再绕一层格式串稳。
 * 超过 0xFFF 饱和成 FFF——宁可显示“很大”，也不能让一行被撑开。 */
unsigned int hex3(char *dst, unsigned long v)
{
    if (v > 0xFFFUL)
        v = 0xFFFUL;
    dst[0] = '0';
    dst[1] = 'x';
    dst[2] = "0123456789ABCDEF"[(v >> 8) & 0xFUL];
    dst[3] = "0123456789ABCDEF"[(v >> 4) & 0xFUL];
    dst[4] = "0123456789ABCDEF"[v & 0xFUL];
    return 5;
}

/* 四位十六进制（MBR 尾签名 0xAA55 这类值，0xFFF 装不下） */
unsigned int hex4(char *dst, unsigned int v)
{
    dst[0] = '0';
    dst[1] = 'x';
    dst[2] = "0123456789ABCDEF"[(v >> 12) & 0xF];
    dst[3] = "0123456789ABCDEF"[(v >> 8) & 0xF];
    dst[4] = "0123456789ABCDEF"[(v >> 4) & 0xF];
    dst[5] = "0123456789ABCDEF"[v & 0xF];
    return 6;
}

/* 六位十六进制定宽：计数器和 `hex3` 不一样，不能截断——时钟在 100Hz 下跑满
 * 0xFFF 只要 41 秒，截断之后“看起来没涨”的计数器比没有更糟。 */
unsigned int hex6(char *dst, unsigned long v)
{
    int shift;
    unsigned int k = 0;

    dst[k++] = '0';
    dst[k++] = 'x';
    for (shift = 20; shift >= 0; shift -= 4)
        dst[k++] = "0123456789ABCDEF"[(v >> shift) & 0xFUL];
    return k;
}

/* 八位十六进制（32 位值，比如 exec 的 magic 0x45584543——hex6 装不下） */
unsigned int hex8(char *dst, unsigned long v)
{
    int i;

    dst[0] = '0';
    dst[1] = 'x';
    for (i = 0; i < 8; i++)
        dst[2 + i] = "0123456789ABCDEF"[(v >> (28 - i * 4)) & 0xFUL];
    return 10;
}

/* 十进制（不定宽）：给 `tasks` 的 prio/slice/runs 用，这几位不会到 6 位那么长 */
unsigned int udec(char *dst, unsigned long v)
{
    char tmp[12];
    unsigned int n = 0, k = 0;

    if (v == 0) {
        dst[k++] = '0';
        return k;
    }
    while (v > 0 && n < sizeof(tmp)) {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (n > 0)
        dst[k++] = tmp[--n];
    return k;
}

/* 往 line 里抄一段字面量，返回新的写位置（只给 `fd` 命令用，省得堆转义） */
unsigned int put_lit(char *dst, unsigned int k, const char *s)
{
    while (*s != '\0')
        dst[k++] = *s++;
    return k;
}

/* 打印返回码：负数打成 E<n>（和 `clock sync rc=E2` 同一个口径：
 * 一个字母 + 绝对码，比 "-3" 更容易在 79 列里一眼认出"这是错误分支"） */
unsigned int put_rc(char *dst, unsigned int k, int rc)
{
    if (rc < 0) {
        dst[k++] = 'E';
        return k + udec(dst + k, (unsigned long)(-rc));
    }
    return k + udec(dst + k, (unsigned long)rc);
}

unsigned int put_s(char *dst, const char *s)
{
    unsigned int n = 0;

    while (s[n] && n < SHELL_OUT_COLS)
        dst[n] = s[n], n++;
    return n;
}

unsigned int u3(char *dst, unsigned int v)
{
    dst[0] = (char)('0' + (v / 100) % 10);
    dst[1] = (char)('0' + (v / 10) % 10);
    dst[2] = (char)('0' + v % 10);
    return 3;
}

/* 有符号十进制：只给"差值"用（+3 / -2）。`udec` 不认负号，别在那里凑。 */
unsigned int sdec(char *dst, int v)
{
    unsigned int k = 0;

    if (v < 0) {
        dst[k++] = '-';
        k += udec(dst + k, (unsigned long)(-v));
    } else {
        dst[k++] = '+';
        k += udec(dst + k, (unsigned long)v);
    }
    return k;
}
