/* ==============================================================
 * exec：把文件系统里的字节装进用户页，然后在 ring3 跑起来
 *
 * 这一层只做三件事，但每一件都必须按顺序做对，否则失败方式都很隐蔽：
 *   1. 页要**先要、再拷、最后放行**：page_alloc 给的页在恒等映射里是 U/S=0，
 *      拷完再 page_lease_open 才让它对 ring3 可见（顺序反了就是"用户取指即 #PF"）；
 *   2. 门的指针窗口必须覆盖"程序自己会传的那几样东西"——用户栈顶往下 128 字节：
 *      一段要 write 的字符串 + {nr, a0, a1, a2} 请求结构体。窗口给错，程序会卡在门里
 *      被判"拒绝"而不是崩，看起来像"程序没跑"，实际是"内核没让它说话"；
 *   3. 跑完必须**收窗口、还租约、还页**。少一步都会让 ring3 关掉之后还留着
 *      一张用户可访问的页，下一趟 exec 就会读到上一趟的残留。
 *
 * 协议（与 boot/uprog.asm 一致，内核侧只看不猜）：
 *   [esp0-24] magic 0x45584543 ("EXEC") —— 程序真的跑到了这一段
 *   [esp0-20] 结果值                    —— 程序算出来的东西
 *   [esp0-128] 一串要打印的字符         —— 走 sys_write
 *   [esp0-16] {nr, a0, a1, a2} 调用门请求 —— 最后一条是 nr=2（exit），把执行流送回内核
 * 退出后 user_enter 返回，exec 才拿得到这两个数（外加 syscall 层的退出码）。
 * ======================================================
 *
 * 跑在哪儿：任务里。`exec_run` 只做"读文件 + 要页 + 拷 + 放行 + 交接 + 建任务 + 等"，
 * 真正的 ring3 下潜发生在 `exec_task_body`（跑在新任务的栈上），收尾发生在
 * `exec_task_dtor`（跑在调度器换好的栈上、且只调一次，只记死因）。
 * **这一趟的页打包成一个 aspace 对象**（`include/mm/aspace.h`）：代码页/页数/
 * 栈页/入口/esp0 都在里面，job 只持一个指针。对象的所有权只有两处：
 *   装配失败 → `exec_run_job` 自己 free；
 *   成功     → 归任务，`exec_task_dtor` 在任务死后 free。
 * **页归任务**：spawn 之前 `task_user_offer(as->code, as->pages, as->stack)`
 * 把页记到 `task_t` 的 `ucode/upages/ustack`，任务一死由调度器在
 * `release_pending` 里逐页关租约、还页——"谁的任务，谁的页"。
 * ====================================================== */
#ifndef MINI_OS_EXEC_H
#define MINI_OS_EXEC_H

#include "linux/page.h"

#define EXEC_MAX_PAGES   4                   /* 一段程序最多占几页连续内存 */
#define EXEC_MAX_BYTES   (EXEC_MAX_PAGES * PAGE_SIZE_BYTES)
                                             /* 上限 16KB：文件本身 + ELF 的
                                              * p_memsz 都要落进这 4 页之内 */
#define EXEC_MAGIC      0x45584543UL         /* "EXEC"：程序写下的指纹 */
#define EXEC_EXIT_FIELD 0xFFUL               /* TUNE_REQ_EXIT */

/* ---- 参数怎么递到用户程序手里 ----
 *
 * 不占寄存器（`user_enter` 只负责 iret，入口处的通用寄存器没有约定），也不改
 * 调用门协议——参数和指纹一起摆在用户栈顶那 128 字节窗口里，程序用**自己的
 * 初始 esp** 去取（进 ring3 那一刻 esp == esp0，这是它唯一能依赖的事实）：
 *
 *   [esp0-128 + i*24, +24)   argv[i] 的字符串（最多 23 字节 + NUL，共 3 个）
 *   [esp0-56  + i*4 )        argv[i] 的绝对指针（指向上面那一格）
 *   [esp0-44]                argc
 *   [esp0-24] magic     [esp0-20] 结果值     （老协议不变）
 *   [esp0-16] {nr, a0, a1, a2}                （老协议不变）
 *
 * 为什么放这里而不是"压标准的 argv 栈帧"：窗口（sys_ptr_ok 的判据）就是这
 * 128 字节，字符串放在窗口外的话，程序连自己名字都 `write` 不出去。 */
#define EXEC_ARGV_MAX   3                    /* 最多几个参数（含 argv[0]） */
#define EXEC_ARG_LEN    23                   /* 单个参数最多几个字符（+ NUL = 24） */
#define EXEC_ARG_STRIDE 24                   /* 参数槽的步长 */

/* ---------------------------------------------------------------
 * 窗口的大小与布局（sys_ptr_ok 的唯一判据，用户程序和内核的共同契约）
 * ---------------------------------------------------------------
 * 窗口 = [esp0 - EXEC_WINDOW, esp0)，进 ring3 时 esp == esp0，
 * 所以"窗口里的某一格"在汇编里就是 `[esp - 偏移]`。
 *
 *   [esp0-16,  esp0)     调用门请求结构体 {nr, a0, a1, a2}
 *   [esp0-24] / [esp0-20] magic / 结果值
 *   [esp0-44]            argc
 *   [esp0-56,  esp0-44)  argv[0..2] 的绝对指针
 *   [esp0-128, esp0-56)  argv 字符串（3 格 × 24）
 *   [esp0-256, esp0-128) **I/O 缓冲**：fd 的 read/write 数据段（128 字节）
 *
 * 最后一格是这一版才有的：fd 的 read/write 要把数据落在用户自己的内存里，
 * 而 sys_ptr_ok 只认窗口内的指针——窗口原本 128 字节已经被 argv 占满，
 * 没有一段 64 字节以上的连续空位。扩到 256 是**最省事也最安全**的做法：
 * 多出来的 128 字节仍在同一个用户栈页里（页基 = esp0 - 4096），
 * 窗口变大不等于信任边界变松，只是"允许传的范围"从半页变成半页多一点。
 * ============================================================== */
#define EXEC_WINDOW      256UL               /* 窗口字节数 */
#define EXEC_IO_OFF      256UL               /* I/O 缓冲区的起点（相对 esp0 的负偏移） */
#define EXEC_IO_BYTES    128                 /* I/O 缓冲区长度（一次最多 64，留一半余量） */

/* 从 mosfs 读 name、装页、跑 ring3、收回。
 * 格式分派：文件头是 ELF32/ET_EXEC/EM_386 时按 include/fs/elf.h 的规则搬段、
 * 用 e_entry 作为起点；否则仍按裸二进制从页头开始跑（老路径不变）。
 * 页数由 `max(文件长度, elf_span)` 算出：一个段可以跨页，但整段必须落进
 * EXEC_MAX_PAGES 页连续内存（恒等映射下"物理连续"就是"虚拟连续"）。
 *
 * argc/argv：内核侧的字符串数组，最多 EXEC_ARGV_MAX 个、每个最多 EXEC_ARG_LEN
 * 字符；超出的会被截断（截断不报错，因为"名字太长"不该让整趟 exec 失败）。
 * argc<=0 或 argv==NULL 表示"没有参数"，此时程序读到的 argc 是 0。
 * argv 在 spawn 之前就拷进用户栈页，所以调用方返回后可以立刻复用那些字符串。
 *
 * 成功返回 0，并把 magic / 结果写回 out_magic / out_value；
 * 失败：-1 找不到文件、-2 文件空或超 EXEC_MAX_BYTES、-3 页不够（含租约表满）、
 *       -4 读不全、-5 没跑完（指纹不对）、-6 是 ELF 但装不进去、
 *       -7 并发上限（TASK_SLOTS 格 job 全被占着，明确失败不排队）。 */
int exec_run(const char *name, int argc, const char *const *argv,
             unsigned long *out_magic, unsigned long *out_value);

/* 现在有几格 job 正被占着。跑完应当回到 0——这是"job 表没有残留"的证据
 * （单槽时代这个数恒为 1 或 0，分槽之后它才有独立意义）。 */
int exec_slots_used(void);

/* 账：成功跑过几次、失败几次 */
unsigned int exec_count(void);
unsigned int exec_failures(void);

/* 最近一次程序调用 exit 的退出码；没调用过返回 0xFFFFFFFF（SYS_EXIT_NONE）。
 * 这一格和 magic 是两件事：magic 证明"跑到了那一段"，退出码是"它自己交回的数字"。 */
unsigned long exec_exit_code(void);

/* 最近一次跑在哪个任务槽位（0 = 没建起来）。这一格是"exec 走 task_spawn"
 * 的可数证据：不再由主上下文自己下潜，而是新任务里下潜、由调度器回收。 */
int exec_last_slot(void);

/* 那个任务的死因（TASK_DEATH_RETURN / EXIT / GUARD）：dtor 记下来的值。
 * 正常一趟应当是 RETURN（入口函数跑完 return），这说明 dtor 真的被交接了。 */
int exec_last_death(void);

/* 走 ELF 路径的次数（0 = 一直在跑裸二进制）。 */
unsigned int exec_elf_count(void);

/* 最近一趟的入口相对页头的偏移（裸二进制 0，ELF 是 e_entry）。 */
unsigned int exec_last_entry(void);

/* 最近一趟代码段占了几页（1 = 老的单页路径；> 1 = 真的有跨页段，
 * 靠 page_alloc_run 要来的一段连续页）。0 = 没跑成。 */
unsigned int exec_last_pages(void);

/* 最近一趟那个任务名下挂着几页用户页 = 代码页 + 用户栈页（单页程序是 2）。
 * 这几页在 spawn 之前就交接给了任务（task_user_offer），任务死时由调度器
 * 在换栈之后统一收——所以这个数字也是"页归任务"这条线的证据。 */
unsigned int exec_last_user_pages(void);
/* 最近一趟有几笔租约改记到了任务头上（= last_user_pages 的话就全交接成功）。
 * 少一笔说明那个任务的借用表满了，有一页仍挂在 KERNEL 名下靠 ttl 兜底。 */
unsigned int exec_last_handed(void);

/* 池里此刻还占着几格地址空间对象。跑完必须回到 0——`jb`（job 表）与
 * 这一个是"这一趟没漏东西"的一对证据。 */
unsigned int exec_live_aspaces(void);
#endif /* MINI_OS_EXEC_H */
