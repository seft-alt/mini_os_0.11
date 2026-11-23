/* ==============================================================
 * mm 内部常量：页框大小、页表项标志、按需分页窗口
 *
 * 只给 mm/page.c（位图分配器）与 mm/paging.c（页表项的写入者）用。
 * 对外 API 仍是 include/mm/page.h（page_alloc / page_fault_map / … 全部没动）。
 * ============================================================== */
#ifndef MINI_OS_PAGEINT_H
#define MINI_OS_PAGEINT_H

#define PAGE_SIZE       4096
#define PTE_FLAGS       0x07          /* 存在 | 可写 | 特权级3 */
#define PTE_KERNEL      0x03          /* 存在 | 可写 | 只有 CPL0 能访问 */

#define DEMAND_BASE     0x02000000    /* 32MB，页目录项 8 覆盖的第一个字节 */
#define DEMAND_SIZE     0x00400000    /* 一张页表 = 4MB 窗口 */

#endif /* MINI_OS_PAGEINT_H */
