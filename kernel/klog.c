/* core/klog.c —— 32 行内核日志环
 *
 * 数据源是 printk 的字符流（见 core/printk.c 的 emit()），这里不解析格式串，
 * 只按 '\n' 切行。因此"能进环的东西"和"能上屏的东西"永远一致，
 * 不会出现"屏幕上有一行、log 里没有"这种两套账。
 */
#include "linux/klog.h"
#include "linux/timer.h"

static char k_text[KLOG_LINES][KLOG_COLS];
static unsigned long k_tick[KLOG_LINES];
static unsigned int k_head;         /* 下一条写哪一格 */
static unsigned int k_count;        /* 0..KLOG_LINES */
static unsigned long k_overwrote;

/* 正在攒的那一行 */
static char pend[KLOG_COLS];
static unsigned int pend_len;
static unsigned long pend_tick;
static unsigned long k_dropped;

static void klog_flush(void)
{
    unsigned int i;

    if (pend_len == 0) {
        pend_tick = 0;              /* 空行也记一条，但时间戳归零，免得误读 */
    }
    for (i = 0; i < pend_len; i++)
        k_text[k_head][i] = pend[i];
    k_text[k_head][pend_len] = '\0';
    k_tick[k_head] = pend_tick;
    k_head = (k_head + 1) % KLOG_LINES;
    if (k_count < KLOG_LINES)
        k_count++;
    else
        k_overwrote++;
    pend_len = 0;
}

void klog_putc(char c)
{
    if (c == '\n') {
        klog_flush();
        return;
    }
    if (pend_len == 0)
        pend_tick = timer_ticks();  /* 行首那一瞬：整行共用一个 tick */
    if (pend_len < KLOG_COLS) {
        pend[pend_len++] = c;
    } else {
        k_dropped++;                /* 超宽只丢字符，不拆行 */
    }
}

unsigned int klog_count(void)
{
    return k_count;
}

const char *klog_get(unsigned int idx, unsigned long *tick)
{
    unsigned int slot;

    if (idx >= k_count)
        return 0;
    slot = (k_head + KLOG_LINES - k_count + idx) % KLOG_LINES;
    if (tick)
        *tick = k_tick[slot];
    return k_text[slot];
}

unsigned long klog_dropped(void)
{
    return k_dropped;
}

unsigned long klog_overwrote(void)
{
    return k_overwrote;
}
