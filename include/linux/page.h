/* ==============================================================
 * 物理页分配器与按需分页接口
 * ============================================================== */
#ifndef MINI_OS_PAGE_H
#define MINI_OS_PAGE_H

/* 清零位图与统计（.bss 不会自动清零，启动时必须显式调用一次） */
void page_init(void);

/* 一页的字节数（4KB）。任务栈/用户栈要算栈顶时用得到。 */
#define PAGE_SIZE_BYTES 4096

/* 分配一页（4KB，已清零），返回物理地址；分配器耗尽返回 0 */
unsigned int page_alloc(void);

/* 分配连续两页（先清页再返回），返回**低地址那一页**；找不到连续空页返回 0。
 * 任务栈要“栈页 + 下面一页 guard”紧挨着，才有“越界即撞墙”的效果。 */
unsigned int page_alloc_pair(void);

/* 分配连续 k 页（k ≤ PAGE_RUN_MAX，全部清零），返回**低地址那一页**；找不到返回 0。
 * 为什么要连续：恒等映射下“物理连续”就是“虚拟连续”，一段跨页的程序（ELF 的
 * p_memsz 越过 4KB）只有这样才是一个能用的地址区间。 */
#define PAGE_RUN_MAX 8
unsigned int page_alloc_run(unsigned int k);

/* 归还一页（只接受本分配器发出的、页对齐的地址，重复释放会被忽略） */
void page_free(unsigned int phys);

/* 归还 page_alloc_run 要来的一整段（内部就是 k 次 page_free，逐页校验） */
void page_free_run(unsigned int phys, unsigned int k);

/* 撤掉**恒等映射**里的一格（PTE 清零 + invlpg），返回原来的 PTE 供以后恢复。
 * 物理页本身仍归分配器管——这是“页还在、但没有虚拟地址指向它”的 guard 页。 */
unsigned int page_identity_unmap(unsigned int phys);

/* 把 page_identity_unmap 返回的原 PTE 写回并刷 TLB（归还 guard 页之前必须做） */
void page_identity_restore(unsigned int phys, unsigned int pte);

/* 把恒等映射里这一格标成 ring3 可访问（PTE 的 U/S 位 = 1），返回改后的 PTE。
 * 只改权限位，不动页框：物理页还是分配器的、虚拟地址还是同一个。
 * 注意 PDE 在 head 建映射时就带了 U/S（0x07），所以这里只补 PTE 那一级。 */
unsigned int page_mark_user(unsigned int phys);

/* 反向动作：把这一格的 U/S 收回去（`page_mark_user` 的逆操作），
 * “临时放行一页、用完关上”用的就是这一对。本来就没开 U 或没映射返回 0。 */
unsigned int page_mark_kernel(unsigned int phys);

/* 读回恒等映射里这一格的 PTE（自检要“确实改成用户可访问了”的证据） */
unsigned int page_identity_flags(unsigned int phys);

/* 把 [lo, hi) 里所有 PTE 的 U/S 清掉（head 建映射时全都带 0x07），
 * 返回“原本带 U/S、被清掉”的页数。之后就只剩 page_mark_user 放行过的页
 * 对 ring3 可见——这是“用户页隔离”唯一真正的开关。 */
unsigned int page_lock_kernel(unsigned int lo, unsigned int hi);

/* 锁定边界 = head 恒等映射的末尾（16MB）。整张恒等映射一次锁完，
 * 而不是只锁内核那 1MB：0x100000–0x1000000 同样是 0x07，留着就是绕过口。 */
#define KERNEL_LOCK_END 0x01000000U

/* 用量统计 */
unsigned int page_frames_total(void);
unsigned int page_frames_used(void);
unsigned int page_frames_free(void);

/* 按需分页窗口的起址与大小（供自检打印） */
void page_demand_window(unsigned int *base, unsigned int *size);

/* 缺页处理：给 addr 补一张页表/一页物理内存并刷 TLB。
 * 成功返回映射到的物理地址；窗口外、页已存在、分配器耗尽都返回 0。 */
unsigned int page_fault_map(unsigned int addr, unsigned int error);

/* 撤掉 addr 的映射并把物理页还给分配器，返回被释放的物理地址；未映射返回 0 */
unsigned int page_unmap(unsigned int addr);

#endif /* MINI_OS_PAGE_H */
