/* core/kv.c —— 机器可读证据通道（契约见 include/core/kv.h）
 *
 * 实现刻意做小：一个静态缓冲 + 就地追加，不分配、不加锁、不建 key 表。
 * 这一层可能被 shell 命令调用（IF 开着、随时可能被时钟打断），所以
 * 它在被打断时"缓冲里是半条记录"是可以接受的——打断它的只有中断处理体，
 * 而那些处理体不调用 kv（只 printk 到屏幕/串口各自的路径），因此不会
 * 出现两条记录交织。若将来中断里也要发 KV，就必须先把当前行拷走或关中断。
 */
#include "linux/kv.h"
#include "linux/serial.h"

#define KV_MAX      240U        /* 一行的上限（含 "KV <tag>" 与换行） */
#define KV_OVF_TAIL "\x20ovf=1" /* 截断标记：脚本据此拒绝拿这行断言 */

static char buf[KV_MAX];
static unsigned int n;          /* 已经写了几个字节 */
static int open_line;           /* 1 = 有未结束的记录 */
static int ovf;                 /* 这一行被截断过 */
static unsigned int lines, overflows;

static void put(char c)
{
    if (n + 1 >= KV_MAX) {      /* 留一格给 '\n' */
        ovf = 1;
        return;
    }
    buf[n++] = c;
}

static void puts_(const char *s)
{
    while (*s)
        put(*s++);
}

static void put_dec(unsigned long v)
{
    char tmp[12];
    unsigned int k = 0;

    if (v == 0) {
        put('0');
        return;
    }
    while (v > 0 && k < sizeof(tmp)) {
        tmp[k++] = (char)('0' + (int)(v % 10UL));
        v /= 10UL;
    }
    while (k > 0)
        put(tmp[--k]);
}

static void put_hex8(unsigned long v)
{
    int shift;

    put('0');
    put('x');
    /* 8 位而不是屏幕上那 6 位：机器通道的头号职责是**不丢信息**。
     * magic 这类 32 位量按 6 位打会变成 0x584543（高两位被截掉），
     * 脚本拿它比对就会误判——这正好是"人看的排版当接口"的反例。 */
    for (shift = 28; shift >= 0; shift -= 4)
        put("0123456789ABCDEF"[(v >> shift) & 0xFUL]);
}

void kv_begin(const char *tag)
{
    n = 0;
    ovf = 0;
    open_line = 1;
    puts_("KV ");
    puts_(tag ? tag : "-");
}

void kv_dec(const char *key, unsigned long v)
{
    if (!open_line)
        return;
    put(' ');
    puts_(key);
    put('=');
    put_dec(v);
}

void kv_hex(const char *key, unsigned long v)
{
    if (!open_line)
        return;
    put(' ');
    puts_(key);
    put('=');
    put_hex8(v);
}

void kv_code(const char *key, int rc)
{
    if (!open_line)
        return;
    put(' ');
    puts_(key);
    put('=');
    if (rc == 0) {
        put('0');
    } else if (rc < 0) {
        /* 与屏幕同一个口径：E1..E9 / EA..；超过 -9 就退化成十进制负数，
         * 那种情况说明调用方把"错误码"当"errno"用了，脚本看得见。 */
        if (-rc <= 9) {
            put('E');
            put((char)('0' + (-rc)));
        } else {
            put('-');
            put_dec((unsigned long)(-rc));
        }
    } else {
        put_dec((unsigned long)rc);
    }
}

void kv_str(const char *key, const char *val)
{
    if (!open_line)
        return;
    put(' ');
    puts_(key);
    put('=');
    puts_(val ? val : "-");
}

void kv_end(void)
{
    if (!open_line)
        return;
    if (ovf)
        puts_(KV_OVF_TAIL);
    buf[n++] = '\n';
    buf[n] = '\0';
    serial_puts(buf);           /* 只走串口：屏幕上的 24 行证据区一个字节不动 */
    lines++;
    if (ovf)
        overflows++;
    open_line = 0;
    n = 0;
}

unsigned int kv_lines(void)      { return lines; }
unsigned int kv_overflowed(void) { return overflows; }
