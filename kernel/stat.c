/* ==============================================================
 * stat：计数器存储（见 include/core/stat.h）
 *
 * 一张定长数组就是全部实现。刻意不做“原子加”：本项目的计数器都在
 * 关中断/单核顺序执行的语境里递增（PF 处理体、page_alloc、shell 分派），
 * 真需要并发时再加 lock xadd，现在加了只是噪音。
 * ============================================================== */
#include "linux/stat.h"

static unsigned long cells[STAT_MAX];

static const char *const names[STAT_MAX] = {
    "pf",        /* 补上的缺页 */
    "pf_fail",   /* 补不上的缺页 */
    "alloc",     /* 物理页分配 */
    "kill",      /* 被守卫杀掉的任务 */
    "cmd",       /* shell 命令 */
    "io",        /* ring3 端口访问被拒 */
    "ovf"        /* 排版溢出 */
};

void stat_inc(unsigned int id)
{
    if (id < STAT_MAX)
        cells[id]++;
}

unsigned long stat_read(unsigned int id)
{
    return id < STAT_MAX ? cells[id] : 0;
}

const char *stat_name(unsigned int id)
{
    return id < STAT_MAX ? names[id] : "?";
}
