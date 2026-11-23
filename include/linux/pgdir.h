/* ==============================================================
 * mm/pgdir.h —— 每任务页目录：把"一张全局页表"换成"各看各的"
 *
 * 在 pgdir 之前，CR3 从头到尾都是物理 0 那一张页目录（head.asm 建好的
 * 4 个页表覆盖 0..16MB）。隔离只有一个开关：PTE 的 U/S 位
 * （`page_lock_kernel` 全清、`page_mark_user` 逐页放行）。
 *
 * 那个开关解决的是"用户能不能进内核那一页"，解决不了"用户能不能进**别的
 * 任务**那一页"。因为 U/S 位写在页表里，而页表是**所有任务共用**的一张：
 * 任务 A 的代码页被标成 user，任务 B 走的是同一张表，所以 B 也进得去。
 * 现象很具体：A 的栈在 0x29000，B 换个地址就能读写 A 的栈，而没有任何一处
 * 会报错。
 *
 * 这一层做的事：给每个槽位一份**自己的页目录**（4KB）+ 一份**自己的 PT0**
 * （4KB，覆盖 0..4MB，任务用户页全在这一段）。PDE 1..3（4MB..16MB）仍然
 * 指向内核那几张共享页表——那一片是恒等映射的空白内存，按任务复制没有意义。
 *
 * 于是"用户能不能进 A 的页"变成"B 的 PT0 里有没有那一格的 U/S"，
 * 而 PT0 是私有的，答案天然是否定的。
 *
 * 三条边界：
 *   1. slot 0（main）**不做**私有页目录：它本来就在内核态跑，而且 4MB 窗口
 *      这种"只在自检时用"的映射也只在内核页目录里建。`pgd_tab[0] = 0`，
 *      0 这个值在汇编里表示"不切 CR3"。
 *   2. 切 CR3 只发生在两处：`user_enter`（进 ring3）与回内核的桩
 *      （`user_return_stub` / `task_die`）。别在别的地方偷偷切——
 *      在错的时刻切，栈会瞬间变成"不可访问"，现象是三重错误。
 *   3. 任务死时把自己的 PT0 收干净（`pgdir_clear_user`）：
 *      US 位留在私有页表里虽然没人能用（下一任任务不会拿到这张表），
 *      但它是"这一格曾经是谁的"的残留，会让"跑完应当归零"这条证据失效。
 * ============================================================== */
#ifndef MINI_OS_MM_PGDIR_H
#define MINI_OS_MM_PGDIR_H

#include "linux/task.h"

/* 内核页目录的物理地址（head.asm 把它放在物理 0，与 pg_dir 符号同址） */
#define PGDIR_KERNEL 0U

/* 为 1..TASK_SLOTS-1 各建一份页目录 + 私有 PT0（各一页，来自页分配器）。
 * 0 = 全部建好；<0 = 页分配器不够（返回负的失败数） */
int pgdir_init(void);

/* 某个槽位的页目录物理地址；0 = 这个槽位用内核页目录（slot 0 或没建起来） */
unsigned int pgdir_of(int slot);

/* 现在正装着哪个页目录（读 CR3，证据用） */
unsigned int pgdir_current(void);

/* 切到某个槽位的页目录；没有私有页目录就切回内核。返回切换后的 CR3 */
unsigned int pgdir_activate(int slot);

/* 切回内核页目录（物理 0） */
void pgdir_kernel(void);

/* 任务切换：保存切出者的 CR3、装上进入者的。from/to 是槽位号。
 * 这是"CR3 属于任务上下文"这件事唯一的记账点——不记账的话，
 * A 在 ring3 被抢占、切到 B、再切回 A，CR3 会停在 B 的那张表上，
 * A 的下一条取指就 #PF（被当成越权，任务被杀）。 */
void pgdir_switch(int from, int to);

/* 把某个槽位私有 PT0 里的一格标成 user / 收回。phys 必须 4KB 对齐、落在
 * 0..4MB（私有 PT0 只覆盖这一段）。1 = 改成了，0 = 不在这一段/没建表。 */
int pgdir_set_user(int slot, unsigned int phys, int on);

/* 同上，外加"这页对 ring3 可写吗"：writable=0 放行但不给写（PTE 的 RW 清掉，
 * US 保留）——用户页写保护，代码页只读的落点。ring3 往只读页写会 #PF，
 * exception.c 的"用户态发起的缺页一律越权"会把它变成一次干净的任务死亡。
 * 只有 slot 0（没有私有表）做不到只读：那条路会按失败计数返回 0。 */
int pgdir_set_user_page(int slot, unsigned int phys, int on, int writable);

/* 把某个槽位私有 PT0 的所有 US 位收掉，返回收了几格 */
unsigned int pgdir_clear_user(int slot);

/* 某个槽位此刻有几格 US=1（证据："跑完应当归零"） */
unsigned int pgdir_user_pages(int slot);

/* ---------------------------------------------------------------
 * 证据接口
 * --------------------------------------------------------------- */
unsigned int pgdir_switches(void);   /* 切任务的次数（含切回内核） */
unsigned int pgdir_cr3_of(int slot); /* 记账表里这一格记的 CR3（0 = 内核） */
unsigned int pgdir_maps(void);       /* 累计 set_user 成功次数 */
unsigned int pgdir_fails(void);      /* 建表失败/参数被拒的次数 */

#endif /* MINI_OS_MM_PGDIR_H */
