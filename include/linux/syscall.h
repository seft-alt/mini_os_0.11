/* ==============================================================
 * sys/syscall.h — ring3 能问内核要什么
 *
 * 在这之前，门（GDT 0x18）只有一种语义："用 0xFF 请求别再回用户态"。
 * 一个程序跑得起来，但问不了任何事：不能打印、不能退出、也拿不到时间。
 *
 * 现在的 ABI（同一个门，eax = 指向请求结构体的指针）：
 *
 *     struct sys_req {
 *         unsigned long nr;        // 干什么
 *         unsigned long a0;        // 参数 0
 *         unsigned long a1;        // 参数 1
 *         unsigned long a2;        // 参数 2
 *     };                           // 16 字节
 *
 * 返回值一律写回现场里的 eax。`nr = 2`（exit）是个例外：它不返回，
 * 而是把返回帧改成"回内核"，执行流落到 user_return_stub。
 *
 * 指针校验：ring3 给的每个指针都必须落在 `sys_user_window` 注册的窗口内
 * （exec 在进 ring3 之前把用户栈上那一小段注册进去），并且 4 字节对齐、
 * 整个对象不越界。任何一条不满足就**一次也不解引用**，直接返回拒绝——
 * 让用户指针把内核带进 #PF 是最糟的失败方式（内核会以为是自己的 bug）。
 * ============================================================== */
#ifndef MINI_OS_SYS_SYSCALL_H
#define MINI_OS_SYS_SYSCALL_H

#define SYS_NR_TUNE   0     /* a0 = 字段号, a1 = 新值 → 返回旧值（见 sched/tune.h） */
#define SYS_NR_WRITE  1     /* a0 = 串首址, a1 = 长度(1..63) → 返回写出的字节数 */
#define SYS_NR_EXIT   2     /* a0 = 退出码 → 不返回，执行流回内核 */
#define SYS_NR_TICKS  3     /* 无参数 → 返回已收到的时钟 tick */
#define SYS_NR_OPEN   4     /* a0 = 名字首址(窗口内), a1 = 0 读 / 1 写 → fd, 失败 SYS_ERR */
#define SYS_NR_READ   5     /* a0 = fd, a1 = 缓冲首址, a2 = 长度(1..FD_IO_MAX) → 字节数 */
#define SYS_NR_FWRITE 6     /* a0 = fd, a1 = 缓冲首址, a2 = 长度 → 写入字节数 */
#define SYS_NR_CLOSE  7     /* a0 = fd → 0，失败 SYS_ERR */

#define SYS_NR_MAX    SYS_NR_CLOSE   /* 最大的合法号（更大的号一律拒绝） */

#define SYS_REQ_WORDS 4              /* 请求结构体的字数（16 字节） */
#define SYS_WRITE_MAX 63             /* 一次 write 最多 63 字节（够证据用，不会滚屏） */
#define SYS_FD_IO_MAX 64             /* 一次 read/fwrite 最多 64 字节：用户缓冲只有
                                      * 窗口里的 128 字节（[esp0-256, esp0-128)），
                                      * 这里卡住的是"整段必须落在窗口内"那条规则 */

/* 失败的统一返回：0xFFFFFFFF。为什么不返回负数——门返回的是 unsigned long，
 * "哪个负数"对用户程序没有意义，一个确定的哨兵更好比对（cmp eax, -1 一条）。
 * 具体错在哪（表满/没这个文件/方向不对）去看内核侧的 `fd` 命令与 fd_denied()。 */
#define SYS_ERR       0xFFFFFFFFUL

/* dispatch 的"别返回了"标记：门处理体看到它就改写返回帧（回内核）。
 * 用最高位当标记，正常返回值（旧值/tick/字节数）到不了那儿。 */
#define SYS_DO_EXIT   0x80000000UL

/* 没有程序调过 exit 时的哨兵（exec 拿它区分"退出码 0"与"根本没退出"） */
#define SYS_EXIT_NONE 0xFFFFFFFFUL

/* 注册 ring3 可传指针的窗口 [lo, hi)。exec 进 ring3 前调一次，回来后关成 (0,0)。
 * **窗口每槽位一份**：注册与校验都按"当前槽位"（task_self()）定位，
 * 所以两个任务同时下潜时互不覆盖。 */
void sys_user_window(unsigned long lo, unsigned long hi);

/* 把某个槽位的窗口清成 (0,0)：任务死亡、槽位复用时必须调，
 * 否则下一个任务会继承一个已经死掉的窗口（等于永久后门）。 */
void sys_user_window_reset(int slot);

/* 指针是否可解引用：对齐 + 落在窗口内 + 整个对象不越界（1 = 可，0 = 不可） */
int sys_ptr_ok(unsigned long ptr, unsigned long bytes);

/* 字符串是否可读：**不要求 4 字节对齐**（名字是字节串，不是字），
 * 扫到 NUL 为止，每个字节都得在窗口内且是可打印 ASCII，长度不超过 max。
 * 名字走这条路而不是 sys_ptr_ok，是因为"12 字节对齐的字符串"这种要求
 * 在汇编里没有意义，而"整段在窗口内 + 不许控制码"已经够了（1 = 可读）。 */
int sys_str_ok(unsigned long ptr, unsigned long max);

/* 一次"连指针都没让解引用"的拒绝（门处理体在校验失败时调，计入 denied） */
void sys_note_denied(void);

/* 门处理体的分发：号不对就拒绝（返回 0 并记一次 denied） */
unsigned long sys_dispatch(unsigned long nr, unsigned long a0,
                           unsigned long a1, unsigned long a2);

/* 证据接口 */
unsigned int  sys_calls(void);        /* 过门的总次数（含被拒） */
unsigned int  sys_denied(void);       /* 被拒的次数 */
unsigned int  sys_bytes_out(void);    /* write 累计写出的字节 */
unsigned long sys_last_nr(void);      /* 最近一次的号 */
unsigned long sys_exit_code(void);    /* 最近一次的退出码（SYS_EXIT_NONE = 没有） */

#endif /* MINI_OS_SYS_SYSCALL_H */
