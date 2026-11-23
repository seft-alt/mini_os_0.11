/* ==============================================================
 * 抢占式调度接口（3 级优先级 + 老化保底 + 时间片 + 排序睡眠队列 + 槽位复用）
 *   slot 0 = main 的空转上下文；slot 1..4 = worker
 *   状态：UNUSED / READY（在就绪队列里）/ SLEEPING（在睡眠队列里）/ DEAD（刚退出）
 *   优先级：0 = 低，2 = 高；时间片以 tick 计，传 0 表示按优先级取默认值（8/4/2）
 * ============================================================== */
#ifndef MINI_OS_TASK_H
#define MINI_OS_TASK_H

/* 槽位表大小：0 = main，1..4 = worker。对外公开，因为有些表要按"每任务一份"
 * 开（exec 的 job 格、syscall 的用户窗口），它们得能在编译期定长度。 */
#define TASK_SLOTS 5

/* 清空所有槽位与两个队列，并安装 0x30 让出门。可多次调用（每次换一组任务） */
void task_reset(void);

/* 抢占式创建：找空闲槽位、分配一页栈、按 prio/slice 入就绪队列。
 * entry 只接受一个 void* 参数（arg 会被压进新栈，return 之后自动 task_exit）。
 * entry 传 NULL 直接返回 0，且不占用槽位和栈页（新任务从 eip=0 起跑会三重故障）。
 * slice 传 0 = 用该优先级的默认时间片。成功返回槽位号（1..4）；失败返回 0。 */
int task_spawn(void (*entry)(void *arg), void *arg, int prio, unsigned long slice);

/* 任务是怎么死的：作为 dtor 的第二个参数交给调用方。
 * 三个值就是三条真实的死亡路径，缺一条代理方就该怀疑自己写错了。 */
#define TASK_DEATH_RETURN 0     /* 入口函数 return，掉进 task_return_stub */
#define TASK_DEATH_EXIT   1     /* 任务自己调 task_exit */
#define TASK_DEATH_GUARD  2     /* 栈溢出撞 guard 页，被缺页处理杀掉 */

/* 同上，但把 arg 的所有权一起交出去：任务死掉（正常 return / task_exit / 被
 * guard 页杀掉都算）之后，调度器会在换栈之后调用 dtor(arg, reason) 恰好一次，
 * reason 取自上面三个 TASK_DEATH_*。
 *   dtor 为空 → arg 仍归调用方。
 *   dtor 非空 → spawn 成功那一刻起 arg 归任务，任务死后由 dtor 收尾，
 *               调用方不许再用，dtor 也不该假设自己跑在任务上下文里。
 * 为什么要把死因传进去：同一个 arg 在不同死法下该做的事不一样（正常收工要
 * 记账，被守卫杀掉要打点告警，被取消要回滚）。不传死因，dtor 只能一律当
 * “正常结束”处理，guard 杀掉的任务会悄悄走正常收尾。 */
int task_spawn_owned(void (*entry)(void *arg), void *arg,
                     void (*dtor)(void *arg, int reason), int prio, unsigned long slice);

/* 参数析构已经被调用过几次（自检用；只数带 dtor 的任务） */
unsigned long task_dtor_calls(void);

/* 打开抢占调度：下一次时钟中断就开始轮转 */
void task_sched_on(void);

/* 还有 worker 没跑完（READY 或 SLEEPING）返回 1；全部退出后回 0 */
int task_active(void);

/* 已发生多少次任务切换（不含最后回 main 那一次）；抢占与否会有少量浮动 */
unsigned long task_switches(void);

/* 归还最后一个僵尸任务的栈页（回到 main 自己栈上之后调用才安全） */
void task_reclaim(void);

/* 睡眠 ms 毫秒后醒来：进睡眠队列（按到期 tick 排序）、当前任务立刻让出 CPU */
void task_sleep(unsigned long ms);

/* ---- 等事件（0.11 的 sleep_on / wake_up 那一层）----
 * task_sleep 等的是"时间到了"，这里等的是"某个条件成立了"：任务不进就绪队列、
 * 也不进睡眠堆，只把自己的槽位挂在 chan 上，然后立刻让出 CPU；谁改变了条件，
 * 谁调 task_wakeup(chan) 把它搬回就绪队列。
 *
 * 为什么带 cond 而不是让调用方自己 while 检查：单核但不关中断时存在
 * "检查完条件 → 还没睡下 → 对方跑完并 wakeup → 自己睡死"的丢失唤醒窗口。
 * task_wait_chan 在**关中断里**做"再查一次 cond + 入睡"两件事，窗口就没了。
 *   cond(ctx) 非 0 → 条件已成立，直接返回 0（没睡）；
 *   真的睡了并被打醒 → 返回 1；
 *   不能等（slot 0 是 main 的空转上下文、chan 为空）→ 返回 -1。
 * 醒来后中断状态交给调用方：调用方该 cli 就自己 cli。 */
int task_wait_chan(void *chan, int (*cond)(void *ctx), void *ctx);

/* 把等在这个通道上的任务全部搬回就绪队列，返回叫醒了几个。
 * 关中断扫表并恢复调用方的中断状态，因此可以在任意上下文调用（任务/中断）。 */
int task_wakeup(void *chan);

/* 累计等待次数 / 累计唤醒次数 / 现在还有几个槽位挂着等（自检证据） */
unsigned int task_waits(void);
unsigned int task_wakeups(void);
int task_wait_depth(void);

/* 槽位还没走到 UNUSED（READY / SLEEPING / WAIT / DEAD 都算"占着槽"）。
 * 自检等任务真正退场时用它，比对比状态名字方便。 */
int task_alive(int slot);

/* 交出 CPU 但仍在就绪态：本轮时间片直接算完（防止“反复 yield 洗时间片”），
 * 排队回自己那一级队尾，然后立刻换人 */
void task_yield(void);

/* 退出当前任务：槽位立刻标记为空闲，栈由调度器在换栈之后回收；本函数不返回 */
void task_exit(void);

/* 与 task_exit 出口相同，但把死因记成 TASK_DEATH_GUARD。
 * 只给缺页处理用：guard 命中后把现场 eip 改成它，iret 之后任务从这条路径退出，
 * 于是 dtor 能拿到“我是被栈溢出杀掉的”，而不是被当成正常收工。 */
void task_guard_exit(void);

/* 当前正在跑的任务槽位号（0 = main） */
int task_self(void);

/* 某个槽位的键盘归属槽（0 = 共享/没领到）；调度器换人时用它把键盘搬过去。
 * 标签在任务创建时领（`kbd_owner_attach()`），任务回收时还（`kbd_owner_detach()`）。 */
int task_kbd_tag_of(int slot);

/* 就绪队列（三级合计）/ 睡眠队列当前长度（自检用） */
int task_rq_depth(void);
int task_sq_depth(void);

/* 某个任务被记账的 tick 数：配合 slice 证明“时间片按 tick 递减” */
unsigned long task_ticks_of(int slot);

/* 当前优先级：老化会临时抬、衰减会持久压，自检里用它观察降级是否发生 */
int task_prio_of(int slot);

/* 槽位上限（0..task_slots()-1）：shell 的 `tasks` 命令按它遍历，不写死 5 */
int task_slots(void);

/* 状态名："run"（正在跑）/ "ready" / "sleep" / "dead" / "unused"，非法槽位回 NULL。
 * TASK_* 是调度器内部编码，shell 只拿名字，不碰数字。 */
const char *task_state_name(int slot);

/* 时间片配置值、本轮剩余、被选中次数——`tasks` 命令的一行就靠这几个数 */
unsigned long task_slice_of(int slot);
unsigned long task_left_of(int slot);
unsigned long task_runs_of(int slot);

/* 当前任务栈页的最低地址（恒等映射下即物理地址）；再往下写就进 guard 页 */
unsigned long task_stack_bottom(void);

/* ---- 用户页归任务（不是归调用方）----
 * exec 装上来的“代码页（可能是连续几页）+ 用户栈页”在 spawn 之前交给调度器，
 * 下一次 task_create 会把它们记进新槽位。之后：
 *   - 任务活着时，环 3 就靠这几页跑；
 *   - 任务死时，调度器在**换栈之后**（release_pending）逐页还租约、还页。
 * 这样“这笔资源归谁”只有一条线：谁的任务，谁的页。
 * code/pages/stack 全 0 表示交回“没有待交接的页”。 */
void task_user_offer(unsigned int code, unsigned int pages, unsigned int stack);

/* 某个槽位名下还挂着几页用户页（代码页 + 栈页；0 = 没有） */
unsigned int task_user_pages_of(int slot);

/* 某个槽位从 ring3 回来时会用的内核栈顶（0 = 该槽位没用）。
 * slot 0 是内核默认栈（tss_default_esp0()），worker 各自一张任务页——
 * 这正是“TSS.esp0 跟着任务走”的可数证据。 */
unsigned long task_esp0_of(int slot);

/* ---- 每个任务自己的“借用表” ----
 * 租约回收原来要线性扫 8 个全局槽位，扫到谁都算谁的；这里把“我借了哪几页”
 * 记在任务自己身上：借一条记一条，回收按表逐页还，还完这条就从表里划掉。
 * 表只有 TASK_LEASES 格，满了 open 就明确失败（计 denied），不会静默挤掉老的。 */
#define TASK_LEASES 4

/* 给 slot 记一笔“借了 phys”。已经在表里就不重复记。
 * 返回 1 成功；0 = 槽位非法、phys=0 或表满。 */
int task_lease_note(unsigned int slot, unsigned int phys);

/* 划掉一条（不移动其它条目，保持下标稳定）。返回 1 = 真的删掉了一条。 */
int task_lease_forget(unsigned int slot, unsigned int phys);

/* 取第 idx 格记的页（0 = 空） */
unsigned int task_lease_at(unsigned int slot, int idx);

/* 历史最高水位：某任务在这张表里同时记了几页（自检用） */
unsigned int task_lease_peak(void);

/* addr 落在某个任务的 guard 页里就返回它的槽位号，否则 -1（缺页处理用） */
int task_guard_victim(unsigned long addr);

/* 时钟中断里调用：ctx 是当前压栈现场地址，返回要恢复的栈指针 */
unsigned long sched_tick(unsigned long ctx);

/* 软中断 0x30 里调用（task_sleep / task_yield / task_exit 借它让出），不累 tick */
unsigned long task_yield_handler(unsigned long ctx);

#endif /* MINI_OS_TASK_H */
