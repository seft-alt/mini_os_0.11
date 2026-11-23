/* ==============================================================
 * 计数器：只增不减的“跑了几次”统计（见 core/stat.c）
 *
 * 为什么单独一个模块：这些数分属 mm / arch / drivers / shell 四个地方，
 * 但它们要一起出现在同一张表上（屏幕上只放得下一个 `ovf=`，明细走 shell 的
 * `stat` 命令）。谁都可以 +1，但只有 stat.c 拥有存储——不需要把计数器
 * 硬塞进 page / exception / shell 的接口里，也就不必为了打点改那些签名。
 * ============================================================== */
#ifndef CORE_STAT_H
#define CORE_STAT_H

/* 槽位表：新增一项就加在这里，`stat` 命令的输出顺序由它决定 */
enum {
    STAT_PF = 0,      /* 补上了的缺页（PF 处理里 map 成功） */
    STAT_PF_FAIL,     /* 补不上的缺页（判 unhandled） */
    STAT_ALLOC,       /* 物理页分配成功次数（page_alloc / page_alloc_pair） */
    STAT_KILL,        /* 被 guard 页杀掉的栈溢任务 */
    STAT_CMD,         /* shell 成功分派过的命令 */
    STAT_IO,          /* ring3 的端口访问被 I/O 位图拒掉（#GP） */
    STAT_OVF,         /* 运行期排版溢出：一行写满 80 列才换行 */
    STAT_MAX
};

void stat_inc(unsigned int id);
unsigned long stat_read(unsigned int id);

/* 短名（`stat` 命令行里的键），越界返回 "?" */
const char *stat_name(unsigned int id);

#endif /* CORE_STAT_H */
