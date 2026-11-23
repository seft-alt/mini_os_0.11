/* ==============================================================
 * 内核堆：kalloc / kfree
 *
 * 只有页级分配器（page_alloc 发 4096 字节的整页）时，任何"大小不固定"的
 * 结构都只能退化成静态数组（tasks[5]、kbd_queue[4][8]、变量表 6 格）。
 * 这一层要解决的就是这件事：**按字节要内存，按字节还**。
 *
 * 设计上的四个取舍（都是"先能用、边界写清楚"）：
 *   1. 页仍然是唯一来源：堆本身不占 .bss，**第一次 kalloc 才向 page_alloc
 *      要一页**（所以开机顺序、帧计数、24 行自检屏一个数字都不动）；
 *   2. 块头 8 字节（size + magic），返回给调用方的是块头之后那块；
 *      对齐 8，最小块 16 字节；
 *   3. 空闲块进一张 64 格的表，分配时 first-fit；不做相邻合并（碎片会留着，
 *      但每次 kfree 都能被下一次 kalloc 复用，先把"能用"做出来）；
 *   4. 整页永不还回 page_alloc（那是"回收空页"策略，等真的有压力再说）。
 * ============================================================== */
#ifndef MINI_OS_HEAP_H
#define MINI_OS_HEAP_H

/* 初始化（只清账，不分配任何页）。必须早于第一次 kalloc。 */
void heap_init(void);

/* 要 size 字节（内部对齐 8）。失败返回 0（并且 heap_fails 加一）。 */
void *kalloc(unsigned int size);

/* 还回去。p 必须是 kalloc 的返回值；0 直接忽略。 */
void kfree(void *p);

/* ---- 证据 ---- */
unsigned int heap_pages(void);        /* 已从 page_alloc 要到的页数 */
unsigned int heap_used_bytes(void);   /* 当前发出去的总字节（含块头） */
unsigned int heap_live_blocks(void);  /* 当前未归还的块数 */

/* 自检：分配 n 块（每块 size 字节）、写图案、校验、全部归还。
 * 返回校验失败的字节数（0 = 全对）。n 上限 16。 */
unsigned long heap_selftest(unsigned int n, unsigned int size);

#endif /* MINI_OS_HEAP_H */
