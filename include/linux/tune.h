#ifndef SCHED_TUNE_H
#define SCHED_TUNE_H

/* 调度器的可调参数：一份普通的可写全局结构，运行时改了就立刻生效。
 *
 * 之前这些全是 #define 编译期常量：想验证“阈值确实在起作用”，唯一的办法是
 * 改代码、重编译、再跑一遍 QEMU。放到结构体里之后，内核自己就能改自己：
 * 自检脚本在同一个任务里把 restore_ticks 从 30 改成 60，同一段睡眠立刻
 * 不再回血，屏幕上直接出现“改之前 / 改了 / 改回来”三个值。
 *
 * 再往外一层是 `int 0x31` 系统调用（eax=字段号、ebx=新值、返回旧值），
 * 带范围校验：非法值（0、超上界、不认识的字段号）一律拒绝且不改值。
 *
 * 注意这是**单核无锁**的实现：改参数只允许在关中断的上下文中做
 * （中断门内部，或自检里显式 cli），没有原子性/可见性方面的额外机制。 */

/* 0x31 的字段号：前三个是阈值，后三个是各优先级默认时间片 */
#define TUNE_FIELD_AGE      0
#define TUNE_FIELD_DECAY    1
#define TUNE_FIELD_RESTORE  2
#define TUNE_FIELD_SLICE0   3
#define TUNE_FIELD_SLICE1   4
#define TUNE_FIELD_SLICE2   5
#define TUNE_FIELD_COUNT    6

struct sched_tune {
    unsigned long age_ticks;        /* 就绪排队多少 tick 升一级（老化） */
    unsigned long decay_quanta;     /* 连续跑满多少片降一级（衰减） */
    unsigned long restore_ticks;    /* 一次睡够多少 tick 还一级（长睡回血） */
    unsigned long slice[3];         /* 各优先级默认时间片，下标即优先级 */
};

extern struct sched_tune sched_tune;

/* 装回编译期那套默认值（3 / 4 / 30 / {8,4,2}）。 */
void sched_tune_defaults(void);

/* 装默认值 + 挂 0x31 门（main 进来调一次）。 */
void sched_tune_init(void);

/* 带校验地写一个字段：返回旧值，被拒绝返回 0（合法值都 ≥ 1，所以 0 一定是失败）。 */
unsigned long sched_tune_apply(unsigned long field, unsigned long value);

/* int 0x31 的处理体（boot/intr.asm 的 isr_tune 调用），返回满足 iret 的 esp。 */
unsigned long tune_syscall(unsigned long ctx);

/* ---------------------------------------------------------------
 * 白名单：每个入口只放行一部分字段
 * ---------------------------------------------------------------
 * 0x31 是内核自己用的口子，全部字段都放行（6 个位）；
 * 调用门是“将来给 ring3 用”的口子，只放行影响不大的字段：长睡回血阈值
 * 和三级时间片。老化/衰减直接决定“谁先跑”，不给外部改。
 * 位图第 n 位 = TUNE_FIELD_n。 */
#define TUNE_MASK_ALL       0x3Fu   /* 6 个字段全放行（0x31 用） */
#define TUNE_MASK_GATE      0x3Cu   /* RESTORE | SLICE0/1/2（调用门用） */

extern unsigned long tune_gate_mask;

/* ---------------------------------------------------------------
 * GDT 里的 386 调用门（不是中断门）
 * ---------------------------------------------------------------
 * 中断门只能由 int 指令触发，DPL 检查的是“int 的 DPL”；调用门由 `lcall`
 * 触发，DPL=3 表示 ring3 也能进。门本身放在 GDT 第 3 项（选择子 0x18），
 * 目标仍然是内核代码段。
 *
 * 从 ring0 调进来 CPL 不变、也不换栈；从 ring3 调进来才会换栈，新栈来自
 * TSS 的 ss0:esp0 —— 所以“门能跨特权级”这件事的成立条件是 **ltr 过的 TSS**，
 * 光把门的 DPL 改成 3 是不够的（没 TSS 时那一步就是 #GP(0)）。
 *
 * ABI：eax = 指向请求结构体的指针（不再是“字段号 + 值”两个寄存器）
 *     struct sys_req { unsigned long nr, a0, a1, a2; };   // 见 sys/syscall.h
 *     nr = 0 是 tune 本身（a0 = 字段号、a1 = 新值 → 返回旧值）；
 *     nr = 1 write / 2 exit / 3 ticks 由 sys/syscall.c 分派；
 *     nr = 0xFF 是老旁路：不带退出码的“我不回用户态了”。
 * ring3 给的指针必须落在 tune_gate_user_window 注册的窗口内，否则一次也不解引用。 */
#define TUNE_REQ_EXIT   0xFFUL

void sched_tune_gate_init(void);    /* 在 GDT 里摆好门（main 里紧跟 0x31 之后调） */
unsigned long tune_gate_apply(unsigned long field, unsigned long value);
unsigned long tune_gate_call(unsigned long ctx);   /* boot/intr.asm 的门桩调用它 */
unsigned long tune_gate_dpl(void);  /* 从 GDT 描述符里读回 DPL，自检用 */

/* 门的每字段可写上限（0 = 该字段不放行），自检把它打出来证明是表里的数 */
unsigned long tune_gate_cap(unsigned long field);

/* 最近一次过门时调用者的 CPL（0x3 = 真的从 ring3 打进来的），自检取证用 */
extern unsigned long tune_gate_last_cpl;

/* ---- 两个“从内核改字段”的薄包装（自检用；原来住在 init/main.c 里）----
 * 走 int 0x31 / GDT 调用门，而不是直接改 sched_tune 全局——自检要证明的是
 * “对外接口能改值”，不是“内核自己写得动自己的变量”。
 * tune_field_set 返回旧值（被拒时返回 0）；tune_gate_set 返回门的返回值。 */
unsigned long tune_field_set(unsigned long field, unsigned long value);
unsigned long tune_gate_set(unsigned long field, unsigned long value);
/* 注册 ring3 可传指针的窗口 [lo, hi)：只有窗口内的指针才允许 deref */
void tune_gate_user_window(unsigned long lo, unsigned long hi);


#endif
