/* sys/syscall.c —— 门的按号分派（ABI 与理由见 include/sys/syscall.h）
 *
 * 四条：tune 0 / write 1 / exit 2 / ticks 3。每一条都是一个"必须成立"的小契约：
 *
 *   tune  不自己实现校验，转手给 tune_gate_apply —— 白名单、可写上限、范围、
 *         跨字段一致性四层都在那儿，这一层不再抄一遍（抄两遍就会不一致）。
 *   write 只接受可打印 ASCII（0x20..0x7E）与 '\n'、'\t'。理由不是"安全"，
 *         是"证据可信"：往显存里塞控制码会把屏幕搞花，看到的就不是原文了。
 *   exit  只记一个数，然后把"改写返回帧"这件事交回门处理体（它才握着现场）。
 *   ticks 纯粹的只读——这是给用户程序一个"我还活着、时间在走"的观测量。
 *
 * 所有"用户的指针"都先过 sys_ptr_ok：对齐、窗口内、整段不越界。窗口由 exec
 * 在进 ring3 前注册成用户栈上的那一小段，回来后立刻关成 (0,0)。 */
#include "linux/syscall.h"
#include "linux/console.h"
#include "linux/timer.h"
#include "linux/tune.h"
#include "linux/task.h"
#include "linux/fd.h"
#include "linux/mosfs.h"

#define SYS_PRINTABLE_MIN 0x20
#define SYS_PRINTABLE_MAX 0x7E

/* 窗口**每槽位一份**：以前是 user_lo/user_hi 两个全局量，这在 ring3 能被
 * 抢占之后就站不住了——两个任务同时下潜时，后注册的那个会把前一个的窗口
 * 覆盖掉，于是钱一个任务的 write/tune 指针会按"别人的窗口"去校验（要么误放
 * 行到内核内存，要么把合法指针拒掉）。窗口是"这一趟 ring3 行程的私有约定"，
 * 所以它必须和行程一样按槽位分开。
 *
 * 注册和校验都用"当前槽位"（task_self()）：窗口只可能在任务自己的上下文里
 * 注册、也只可能在它自己的上下文里被用到，两端同一个键才不会错位。
 * 槽位复用/任务死亡时必须清掉（见 sys_user_window_reset 的调用点）——
 * 否则下一个任务会继承一个已经死掉的窗口，那是永久后门。 */
static unsigned long win_lo[TASK_SLOTS];
static unsigned long win_hi[TASK_SLOTS];

static unsigned int  n_calls;
static unsigned int  n_denied;
static unsigned int  n_bytes;
static unsigned long last_nr = SYS_EXIT_NONE;
static unsigned long exit_code = SYS_EXIT_NONE;

void sys_user_window(unsigned long lo, unsigned long hi)
{
    int slot = task_self();

    if (slot < 0 || slot >= TASK_SLOTS)
        return;
    win_lo[slot] = lo;
    win_hi[slot] = hi;
}

void sys_user_window_reset(int slot)
{
    if (slot < 0 || slot >= TASK_SLOTS)
        return;
    win_lo[slot] = 0;
    win_hi[slot] = 0;
}

int sys_ptr_ok(unsigned long ptr, unsigned long bytes)
{
    int slot = task_self();
    unsigned long hi;

    if (slot < 0 || slot >= TASK_SLOTS)
        return 0;
    hi = win_hi[slot];
    if (!ptr || (ptr & 3UL) != 0)
        return 0;
    if (!hi || ptr < win_lo[slot] || hi <= ptr)
        return 0;
    if (bytes > hi - ptr)      /* 写成 ptr + bytes > hi 会溢出，这里用减法 */
        return 0;
    return 1;
}

void sys_note_denied(void)
{
    n_calls++;
    n_denied++;
    last_nr = SYS_EXIT_NONE;     /* 连号都没读到（指针就被拒了） */
}

/* 名字串的校验：不要求对齐（字节串不是字），逐字节吃到 NUL 为止。
 * 每读一个字节之前都要确认它还在窗口内——"先算长度再校验"等于先踩一脚。 */
int sys_str_ok(unsigned long ptr, unsigned long max)
{
    int slot = task_self();
    unsigned long hi, i;

    if (slot < 0 || slot >= TASK_SLOTS)
        return 0;
    hi = win_hi[slot];
    if (!ptr || !hi || ptr < win_lo[slot] || hi <= ptr)
        return 0;
    for (i = 0; i <= max; i++) {
        unsigned char c;

        if (ptr + i >= hi)          /* 没遇到 NUL 就撞到窗口边 */
            return 0;
        if (i == max)               /* max 个字符还没有 NUL：太长 */
            return 0;
        c = ((volatile unsigned char *)ptr)[i];
        if (c == 0)
            return i > 0 ? 1 : 0;   /* 空名字不算合法 */
        if (c < 0x21 || c > 0x7E)
            return 0;
    }
    return 0;
}


/* write 的字符集检查：单看一个字节 */
static int sys_char_ok(unsigned char c)
{
    if (c == '\n' || c == '\t')
        return 1;
    return c >= SYS_PRINTABLE_MIN && c <= SYS_PRINTABLE_MAX;
}

static unsigned long sys_write(unsigned long ptr, unsigned long len)
{
    char buf[SYS_WRITE_MAX];
    unsigned long i;

    if (len == 0 || len > SYS_WRITE_MAX)
        return 0;
    if (!sys_ptr_ok(ptr, len))
        return 0;
    for (i = 0; i < len; i++) {
        unsigned char c = ((volatile unsigned char *)ptr)[i];
        if (!sys_char_ok(c))
            return 0;               /* 有一个字节不能打，整条拒掉（不做替换） */
        buf[i] = (char)c;
    }
    shell_con_puts(buf, (unsigned int)len);
    n_bytes += (unsigned int)len;
    return len;
}

/* fd 号 → 内核的 fd 层。四件事共用一条规则：**用户的指针一律先验后解引用**，
 * 失败一律返回 SYS_ERR（不区分原因，原因看内核侧 fd_denied() 与 `fd` 命令）。
 *
 * 名字：走 sys_str_ok（字节串，不要求对齐）。
 * 缓冲：走 sys_ptr_ok（4 字节对齐 + 整段在窗口内），长度封顶 FD_IO_MAX——
 *       窗口只有 256 字节、留给 I/O 的那一段是 128，超过就别想"整段在里面"。 */
static unsigned long sys_open(unsigned long nptr, unsigned long flags)
{
    char name[FS_NAME_MAX + 1];
    unsigned long i;
    int rc;

    if (flags > 1)                          /* 0 = 读，1 = 写（不开放读写混开） */
        return SYS_ERR;
    if (!sys_str_ok(nptr, FS_NAME_MAX))
        return SYS_ERR;
    for (i = 0; ; i++) {
        unsigned char c = ((volatile unsigned char *)nptr)[i];

        name[i] = (char)c;
        if (c == 0)
            break;
    }
    rc = fd_open(task_self(), name, flags ? FD_F_WRITE : FD_F_READ);
    return rc < 0 ? SYS_ERR : (unsigned long)rc;
}

static unsigned long sys_fd_read(unsigned long fd, unsigned long buf, unsigned long len)
{
    int rc;

    if (len == 0 || len > SYS_FD_IO_MAX || !sys_ptr_ok(buf, len))
        return SYS_ERR;
    rc = fd_read(task_self(), (int)fd, (void *)buf, (unsigned int)len);
    return rc < 0 ? SYS_ERR : (unsigned long)rc;
}

static unsigned long sys_fd_write(unsigned long fd, unsigned long buf, unsigned long len)
{
    int rc;

    if (len == 0 || len > SYS_FD_IO_MAX || !sys_ptr_ok(buf, len))
        return SYS_ERR;
    rc = fd_write(task_self(), (int)fd, (const void *)buf, (unsigned int)len);
    return rc < 0 ? SYS_ERR : (unsigned long)rc;
}

unsigned long sys_dispatch(unsigned long nr, unsigned long a0,
                           unsigned long a1, unsigned long a2)
{
    unsigned long r;

    n_calls++;
    last_nr = nr;

    switch (nr) {
    case SYS_NR_TUNE:
        r = tune_gate_apply(a0, a1);
        if (r == 0)
            n_denied++;             /* 0 不是合法旧值（所有字段都 ≥ 1） */
        return r;
    case SYS_NR_WRITE:
        r = sys_write(a0, a1);
        if (r == 0)
            n_denied++;             /* 长度/指针/字符集任一条不过都是 0 */
        return r;
    case SYS_NR_EXIT:
        exit_code = a0;             /* 退出码：先记账，改写返回帧由门处理体做 */
        return SYS_DO_EXIT;
    case SYS_NR_TICKS:
        return timer_ticks();       /* 只读，tick=0 也是合法值，不当成拒绝 */
    case SYS_NR_OPEN:
        r = sys_open(a0, a1);
        if (r == SYS_ERR)
            n_denied++;
        return r;
    case SYS_NR_READ:
        r = sys_fd_read(a0, a1, a2);
        if (r == SYS_ERR)
            n_denied++;
        return r;
    case SYS_NR_FWRITE:
        r = sys_fd_write(a0, a1, a2);
        if (r == SYS_ERR)
            n_denied++;
        return r;
    case SYS_NR_CLOSE:
        r = fd_close(task_self(), (int)a0) == 0 ? 0 : SYS_ERR;
        if (r == SYS_ERR)
            n_denied++;
        return r;
    default:
        n_denied++;
        return 0;
    }
}

unsigned int  sys_calls(void)     { return n_calls; }
unsigned int  sys_denied(void)    { return n_denied; }
unsigned int  sys_bytes_out(void) { return n_bytes; }
unsigned long sys_last_nr(void)   { return last_nr; }
unsigned long sys_exit_code(void) { return exit_code; }
