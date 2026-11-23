/* ==============================================================
 * 调度器内部接口（只给 sched/task.c 与 sched/queue.c 用）
 *
 * 对外 API 仍是 include/sched/task.h（`task_spawn` / `task_sleep` / … 全部没动）。
 * 这里放的是“两个文件共享但不想公开”的东西：
 *
 *   task_t / tasks[]  —— 任务表本体。**定义在 task.c**，queue.c 只读写它。
 *   cur / active / switches —— 调度器的三个全局量，同样定义在 task.c。
 *   queue.c 提供的队列操作 —— rq_* / sq_* / aging_pass / wake_sleepers，
 *                             以及时间片取值的 slice_for 与历史惩罚记账。
 *
 * 为什么队列状态不搬到这里而是搬进 queue.c：rq/sq 的下标只有 queue.c 用，
 * 放在它自己的文件里就没人能绕过去手改；task.c 只通过函数看深度/重置。
 * ============================================================== */
#ifndef MINI_OS_TASKINT_H
#define MINI_OS_TASKINT_H

#include "linux/task.h"          /* TASK_SLOTS / TASK_LEASES / TASK_DEATH_* 在这里 */

#define TASK_PRIO_LEVELS 3       /* 0 = 低，2 = 高；数组下标即优先级 */

#define TASK_UNUSED     0
#define TASK_READY      1
#define TASK_SLEEPING   2       /* 睡到某个 tick（睡眠堆里，wake_tick 为键） */
#define TASK_WAIT       4       /* 等事件：不进任何队列，只靠 task_wakeup(chan) 叫醒 */
#define TASK_DEAD       3

typedef struct {
    unsigned long esp;             /* 保存的栈指针（指向 popa 保存区） */
    unsigned int stack;            /* 栈页物理地址（也是恒等映射的虚拟地址） */
    unsigned int guard;            /* 栈页下面那一页：恒等映射被撤掉，栈溢出即 #PF */
    unsigned int guard_pte;        /* 撤映射时记下的原 PTE，归还前要写回 */
    unsigned long wake_tick;       /* 睡眠到期 tick */
    unsigned long runs;            /* 被选中几次（调试用） */
    unsigned long ticks;           /* 占用 CPU 的 tick 数（时间片公平性证据） */
    unsigned long slice;           /* 时间片（tick） */
    unsigned long left;            /* 本轮剩余时间片 */
    unsigned long wait;            /* 就绪但没轮到的 tick 数（老化用） */
    unsigned long hogs;            /* 连续跑满的时间片数（衰减用，自愿让出时清零） */
    void *chan;                    /* TASK_WAIT 时：等的是哪个通道（0 = 没在等） */
    void *arg;                     /* 传给入口函数的参数 */
    void (*dtor)(void *arg, int reason);  /* 参数所有权在任务死后交给它（可为 NULL） */
    int state;
    int death;                     /* 死因 TASK_DEATH_*（dtor 的第二个参数，见 task_die） */
    int prio;                      /* 当前优先级（可能被老化临时提升） */
    int base_prio;                 /* 落回级别：老化从这里往上借，衰减把这里往下压 */
    unsigned int leases[TASK_LEASES];  /* 我借了哪几页（phys，0 = 空）——回收按这张表走 */
    int kbd_tag;                   /* 我的键盘归属槽（创建时领，0 = 共享/没领到） */

    /* ---- “从 ring3 进 ring0”的那张栈，归任务自己 ----
     * 以前 TSS.esp0 是一张全局专用栈，于是“同时只能有一个 ring3 行程”。
     * 现在每个 worker 在创建时要一页当自己的进内核栈，调度器换人时
     * 把 TSS.esp0 换成这个任务的栈顶（slot 0 仍用 tss_default_esp0()）。 */
    unsigned int esp0_stack;       /* 那一页的物理地址（0 = 没有） */
    unsigned long esp0;            /* 它的栈顶：换到这个任务时写进 TSS */

    /* ---- 这个任务名下的用户页（exec 装上来的代码页与用户栈）----
     * 归任务而不是归 job：任务一死，调度器在换栈之后统一收（还租约 + 还页）。
     * 0 表示这个任务没有用户页（普通内核任务）。 */
    unsigned int ucode;            /* 代码段第一页 */
    unsigned int upages;           /* 代码段页数 1..EXEC_MAX_PAGES */
    unsigned int ustack;           /* 用户栈页 */
} task_t;

extern task_t tasks[TASK_SLOTS];
extern int cur;                    /* 当前在跑的槽位号 */
extern int active;                 /* 抢占是否已打开 */
extern unsigned long switches;     /* 已发生的切换次数 */

/* ---- queue.c 的门面（task.c 只用这些，不碰 rq/sq 的下标） ---- */

/* 清空就绪队列与睡眠队列（task_reset 用） */
void queue_reset(void);
int  rq_depth_total(void);
int  sq_depth(void);

/* 就绪队列：每级一条环形数组 */
int  rq_contains(int slot);
void rq_push(int slot);
void rq_remove(int slot);
int  rq_top(int pop);              /* peek（pop=0）/ 摘下来（pop=1）；空返回 -1 */

/* 睡眠队列：wake_tick 为键的小顶堆 */
void sq_insert(int slot);
void sq_pop(void);

/* 每 tick 跑一次：老化（就绪太久升一级）、到点唤醒 */
void aging_pass(void);
void wake_sleepers(void);

/* 时间片：任务自己给了用自己的，给 0 就按**当前**优先级取默认值 */
unsigned long slice_for(int slot);

/* 历史惩罚：一片真跑满了才记一笔，连续若干片就把 base_prio 压一级 */
void note_quantum_burn(int slot);

#endif /* MINI_OS_TASKINT_H */
