/* ==============================================================
 * 内核日志环（`log` 命令的数据源）
 *
 * 截获 printk 输出的每一行，连同"那一行开始时的 tick"一起存进一个 32 行环。
 * 为什么值得有这条环：屏幕只有 24 行、用户页还要滚动，启动那一刻的证据
 * 一旦被后面的输出顶掉就再也没有了；环是内存，滚不掉。
 *
 * 三条边界（都在代码里体现，不只是注释）：
 *   1. 行宽上限 KLOG_COLS：超出部分的字符直接丢（计 dropped），不换行——
 *      换行会把"一条日志"拆成两条，反而更难读；
 *   2. 环满就覆盖最旧的一条：这是"最近发生了什么"，不是"全部历史"；
 *   3. 时间戳取**行首**那一瞬（行中间被打断时不会前后两半用两个 tick）。
 * ============================================================== */
#ifndef MINI_OS_KLOG_H
#define MINI_OS_KLOG_H

#define KLOG_LINES 32
#define KLOG_COLS  67       /* `log` 打印时前面还有 " t=0xNNNNNN "（11 列），合计 ≤79 */

/* printk 的每个字符都会进来；'\n' 表示一行结束、落环。 */
void klog_putc(char c);

/* 已经记了多少行（最多 KLOG_LINES） */
unsigned int klog_count(void);

/* 取第 idx 条（0 = 最旧）；tick 通过出参返回。越界返回 0。 */
const char *klog_get(unsigned int idx, unsigned long *tick);

/* 因为超宽被丢掉的字符数 / 因为环满被覆盖的行数（证据） */
unsigned long klog_dropped(void);
unsigned long klog_overwrote(void);

#endif /* MINI_OS_KLOG_H */
