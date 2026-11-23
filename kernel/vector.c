/* core/vector.c —— 每向量命中次数（存储在这里，规则只有“只增不减”）
 *
 * 谁可以 +1：异常处理体（0x0D/0x0E）、IRQ0/IRQ1 处理体、0x30/0x31 的入口。
 * 谁能读：shell 的 `irq` 命令（按向量号打印）。读的一方不需要知道谁在写，
 * 写的一方也不需要知道谁在读——这正是把它单独成一个模块的理由。
 */
#include "linux/vector.h"

static unsigned long hits[VEC_SLOTS];

void vec_hit(unsigned int vec)
{
    if (vec < VEC_SLOTS)
        hits[vec]++;
}

unsigned long vec_count(unsigned int vec)
{
    return vec < VEC_SLOTS ? hits[vec] : VEC_BAD;
}

unsigned long vec_total(void)
{
    unsigned int i;
    unsigned long n = 0;

    for (i = 0; i < VEC_SLOTS; i++)
        n += hits[i];
    return n;
}
