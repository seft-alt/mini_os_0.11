/* ==============================================================
 * 32 位 TSS 与 ring3 段（让调用门的特权级切换真的能发生）
 * ============================================================== */
#ifndef MINI_OS_TSS_H
#define MINI_OS_TSS_H

/* 填 TSS、在 GDT 里摆好 TSS/ring3 代码段/ring3 数据段、ltr 装载。
 * 必须在任何“从 ring3 进来”的操作之前调用一次（main 里紧跟 sched_tune_gate_init）。 */
void tss_init(void);

/* 换“从 ring3 进 ring0”时用的内核栈顶。每次换到另一个任务时都要设一次
 * （调度器按 task_t.esp0 换；见 sched/task.c 的 schedule）。 */
void tss_set_esp0(unsigned long esp0);
unsigned long tss_esp0(void);

/* TSS.cr3：硬件任务切换（task gate / ljmp 到 TSS 描述符）时 CPU 会装这一格。
 * 本内核不用 task gate，所以它**没有人在读**；但让一格“没有人在读”的字段
 * 停在 0（= 内核页目录）而在别处写着“任务有自己的表”，就是埋一颗雷：将来
 * 谁加 task gate，切进来的任务会拿到内核表，隔离当场失效。
 * 所以在唯一换表的地方（mm/pgdir.c 的 pgdir_load）同步写这一格，
 * 保证它永远等于“CPU 此刻正在用的那张表”。 */
void tss_set_cr3(unsigned int cr3);
unsigned int tss_cr3(void);

/* 内核自带的那张默认栈的栈顶（tss_init 时装进去的）。
 * 只有 slot 0（main）用它；worker 各自有一张自己的进内核栈页。
 * 它的地址落在内核映像区（< 0x20000），而任务页在 >= 0x20000——自检就靠
 * 这个区间差区分“还在用默认栈”和“已经换成本任务自己的栈”。 */
unsigned long tss_default_esp0(void);

/* TR 寄存器里真装着的选择子（ltr 之后应为 0x30） */
unsigned long tss_tr(void);

/* I/O 位图基址（iomap_base）：等于 TSS 结构大小 0x68 时表示“这张表里没有位图”，
 * 于是 CPL3 的 in/out 一律 #GP。它是一道**权限**，不是配置细节，所以单独暴露。 */
unsigned long tss_iomap_base(void);

/* 从 GDT 描述符里读回 TSS 的基址与访问字节（类型 0x89 → 装载后变 0x8B） */
unsigned long tss_gdt_base(void);
unsigned long tss_gdt_type(void);

#endif /* MINI_OS_TSS_H */
