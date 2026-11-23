/* ==============================================================
 * printk：格式化 + 输出
 *
 * 分层的理由：vga.c 只管“一个字符怎么落到显存和 CRTC 光标”，
 * 这里只管“一串字符怎么变成字节流”。格式化器是手写的：内核里没有 libc，
 * 也不该为了打一行字把 %f、宽度标志、长度修饰符整套搬进来。
 *
 * 目前只认四种转换和一种宽度写法（见 include/core/printk.h）：
 * 少了什么就直接原样输出，宁可打错也不静默丢字——屏幕上丢字比打错更难查。
 * ============================================================== */
#include <stdarg.h>
#include "linux/printk.h"
#include "linux/klog.h"
#include "linux/vga.h"
#include "linux/serial.h"
#include "linux/timer.h"

#define HEX_UPPER "0123456789ABCDEF"
#define HEX_LOWER "0123456789abcdef"

/* 一个字符的三条腿：屏幕（老行为）+ 串口（脱屏调试）+ 日志环（`log` 的数据源）。
 * 三条都走这里，所以"谁能看见"不会再分成两套账；串口未初始化时自己会丢字。
 *
 * emit_screen 是给 `printk_dev` 留的开关：那 24 行自检屏是按 24 行算死的，
 * 启动阶段想留日志就只能往串口和环里写，一个字符都不能落到显存上。 */
static int emit_screen = 1;

static void emit(char c)
{
    if (emit_screen)
        vga_putc(c);
    serial_putc(c);
    klog_putc(c);
}

/* 十六进制：按 digits 位补零后输出（v 已经按位右移取过，倒序填表） */
static void put_hex(unsigned long v, int width, int upper)
{
    char buf[12];
    int n = 0;
    int i;
    const char *digits = upper ? HEX_UPPER : HEX_LOWER;

    do {
        buf[n++] = digits[v & 0xFUL];
        v >>= 4;
    } while (v != 0);

    for (i = n; i < width; i++)
        emit('0');
    for (i = n - 1; i >= 0; i--)
        emit(buf[i]);
}

static void put_udec(unsigned long v)
{
    char buf[12];
    int n = 0;
    int i;

    do {
        buf[n++] = (char)('0' + (int)(v % 10));
        v /= 10;
    } while (v != 0);

    for (i = n - 1; i >= 0; i--)
        emit(buf[i]);
}

static void put_sdec(int v)
{
    unsigned long mag;

    if (v < 0) {
        emit('-');
        mag = (unsigned long)(-(long)v);
    } else {
        mag = (unsigned long)v;
    }
    put_udec(mag);
}

static void vformat(const char *fmt, va_list ap)
{
    while (*fmt) {
        if (*fmt != '%') {
            emit(*fmt++);
            continue;
        }

        fmt++;                                  /* 跳过 '%' */
        if (*fmt == '%') {                      /* "%%" */
            emit('%');
            fmt++;
            continue;
        }

        /* 宽度：只认 "0" 前缀 + 1..2 位数字（%02X / %08X / %03X） */
        int pad_zero = 0;
        int width = 0;

        if (*fmt == '0') {
            pad_zero = 1;
            fmt++;
            while (*fmt >= '0' && *fmt <= '9' && width < 10) {
                width = width * 10 + (*fmt - '0');
                fmt++;
            }
        }
        (void)pad_zero;                         /* 目前只给十六进制用，最小位数即补零 */

        switch (*fmt) {
        case 's': {
            const char *s = va_arg(ap, const char *);

            if (!s)
                s = "(null)";
            while (*s)
                emit(*s++);
            break;
        }
        case 'c':
            emit((char)va_arg(ap, int));
            break;
        case 'd':
            put_sdec(va_arg(ap, int));
            break;
        case 'u':
            put_udec((unsigned long)va_arg(ap, unsigned int));
            break;
        case 'x':
            put_hex((unsigned long)va_arg(ap, unsigned long), width, 0);
            break;
        case 'X':
            put_hex((unsigned long)va_arg(ap, unsigned long), width, 1);
            break;
        case 't': {
            /* 时间戳：`t=0xNNNNNN `（6 位十六进制定宽，1 tick = 10ms）。
             * 定宽的理由和 `irq`/`tasks` 一样：不定宽会让日志列错位，
             * 而日志的目的就是"能对着看"。不吃参数，也不带尾随空格——
             * 尾随空格留给调用方写，免得只在某些行多一个空格。 */
            emit('t');
            emit('=');
            emit('0');
            emit('x');
            put_hex((unsigned long)timer_ticks(), 6, 0);
            break;
        }
        case '\0':                              /* 格式串以 '%' 结尾：原样收尾 */
            fmt--;
            break;
        default:                                /* 不认识的转换：原样打出来 */
            emit('%');
            emit(*fmt);
            break;
        }
        if (*fmt)
            fmt++;
    }
}

int printk(const char *fmt, ...)
{
    va_list ap;

    if (!fmt)
        return 0;
    va_start(ap, fmt);
    emit_screen = 1;
    vformat(fmt, ap);
    va_end(ap);
    return 0;
}

/* 只进串口 + 日志环，不进显存。用途只有一个：启动阶段的日志。
 * 24 行自检屏是按行算死的，多一行 printk 就会把证据挤走；所以"想留痕又不想动屏"
 * 的写入一律走这里。 */
int printk_dev(const char *fmt, ...)
{
    va_list ap;

    if (!fmt)
        return 0;
    va_start(ap, fmt);
    emit_screen = 0;
    vformat(fmt, ap);
    emit_screen = 1;                        /* 复位，别把后面的 printk 也关掉 */
    va_end(ap);
    return 0;
}
