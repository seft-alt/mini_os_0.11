/* fs/pipe.c —— 管道（契约见 include/linux/pipe.h）
 *
 * 一句话：**两个 fd 指向同一个环，一个只能读、一个只能写**。
 *
 * fd 层不认识管道：`fd_pipe()` 只占两个格子、把 `ino` 填成槽号、把
 * `fop` 指向这里的 `pipe_fops`。剩下的（环怎么转、满了怎么办、
 * 两端谁先关）全在这个文件里，和 0.11 把 `read_pipe` / `write_pipe`
 * 放在 fs/pipe.c 是同一个分工。
 *
 * 环的记账只有三个数：`head`（写到这里）、`tail`（从这里读）、
 * `cnt`（环里现在几个字节）。`cnt` 是唯一的"满/空"判据——
 * 用 head==tail 判空会把"写满一圈"误判成空，这是环形缓冲的经典坑。
 */
#include "linux/fd.h"
#include "linux/pipe.h"
#include "linux/string.h"

struct pipe_ring {
    unsigned char buf[PIPE_BUF];
    unsigned int head;              /* 下一个写的位置 */
    unsigned int tail;              /* 下一个读的位置 */
    unsigned int cnt;               /* 环里有几个字节 */
    unsigned int readers;           /* 还没关的读端（正常是 0 或 1） */
    unsigned int writers;           /* 还没关的写端 */
    int used;
};

static struct pipe_ring pipes[PIPE_MAX];

static unsigned int n_live;         /* 活着的对数 */
static unsigned int n_created;      /* 累计建过 */
static unsigned int n_read;
static unsigned int n_written;
static unsigned int n_drop;         /* 满而没写进去的字节 */

/* ---------------------------------------------------------------
 * 槽管理：fd.c 的 fd_pipe 拿到一个干净槽就开始填两个格子
 * --------------------------------------------------------------- */
int pipe_new(void)
{
    unsigned int i;

    if (n_live >= PIPE_MAX)
        return -1;                  /* 表满：明确失败，不抢别人的槽 */
    for (i = 0; i < PIPE_MAX; i++) {
        if (pipes[i].used)
            continue;
        memset(&pipes[i], 0, sizeof(pipes[i]));
        pipes[i].used = 1;
        pipes[i].readers = 1;       /* 管道一定同时建两端：计数在这儿置 */
        pipes[i].writers = 1;
        n_live++;
        n_created++;
        return (int)i;
    }
    return -1;
}

void pipe_free(int ino)
{
    if (ino < 0 || ino >= PIPE_MAX)
        return;
    if (!pipes[ino].used)
        return;
    memset(&pipes[ino], 0, sizeof(pipes[ino]));
    n_live--;                       /* 只在两端都关时走到这里 */
}

/* ---------------------------------------------------------------
 * 三个回调
 * --------------------------------------------------------------- */
static int pipe_open(struct fd_file *f)
{
    (void)f;
    /* 计数在 pipe_new 里就置好了。fd_open 不认识管道名字，
     * 所以这里没有"再 ++ 一次"的机会，也就不需要防重入。 */
    return 0;
}

static int pipe_read(struct fd_file *f, void *buf, unsigned int len)
{
    struct pipe_ring *p;
    unsigned char *d = (unsigned char *)buf;
    unsigned int i;

    if (!(f->flags & FD_F_READ))
        return FD_E_BADFD;          /* 写端不许读 */
    if (!buf || len == 0)
        return FD_E_ARG;
    if (f->ino >= PIPE_MAX || !pipes[f->ino].used)
        return FD_E_BADFD;
    p = &pipes[f->ino];
    if (p->readers == 0)
        return FD_E_BADFD;          /* 读端已经关了：这一格不该还在用 */

    if (len > PIPE_BUF)             /* 一次最多一圈：短读由调用方循环 */
        len = PIPE_BUF;
    for (i = 0; i < len && p->cnt > 0; i++) {
        d[i] = p->buf[p->tail];
        p->tail = (p->tail + 1) % PIPE_BUF;
        p->cnt--;
    }
    n_read += i;
    return (int)i;                  /* 空环返回 0：非阻塞，不是 EOF */
}

static int pipe_write(struct fd_file *f, const void *buf, unsigned int len)
{
    struct pipe_ring *p;
    const unsigned char *s = (const unsigned char *)buf;
    unsigned int i;

    if (!(f->flags & FD_F_WRITE))
        return FD_E_BADFD;
    if (!buf || len == 0)
        return FD_E_ARG;
    if (f->ino >= PIPE_MAX || !pipes[f->ino].used)
        return FD_E_BADFD;
    p = &pipes[f->ino];
    if (p->writers == 0)
        return FD_E_BADFD;

    for (i = 0; i < len; i++) {
        if (p->cnt == PIPE_BUF) {   /* 满：**短写**，剩下的算丢弃 */
            n_drop += len - i;
            break;
        }
        p->buf[p->head] = s[i];
        p->head = (p->head + 1) % PIPE_BUF;
        p->cnt++;
    }
    n_written += i;
    return (int)i;
}

static int pipe_lseek(struct fd_file *f, unsigned int off)
{
    (void)f;
    (void)off;
    return FD_E_ARG;                /* 环没有"位置"：seek 是骗人 */
}

static void pipe_release(struct fd_file *f)
{
    struct pipe_ring *p;

    if (f->ino >= PIPE_MAX)
        return;
    p = &pipes[f->ino];
    if (!p->used)
        return;
    if ((f->flags & FD_F_READ) && p->readers > 0)
        p->readers--;
    if ((f->flags & FD_F_WRITE) && p->writers > 0)
        p->writers--;
    if (p->readers == 0 && p->writers == 0)
        pipe_free((int)f->ino);     /* 两端都关：槽回收，live 减一 */
}

const struct file_operations pipe_fops = {
    "pipe",
    pipe_open,
    pipe_read,
    pipe_write,
    pipe_lseek,
    pipe_release,
};

/* ---------------------------------------------------------------
 * 证据
 * --------------------------------------------------------------- */
unsigned int pipe_live(void)         { return n_live; }
unsigned int pipe_created(void)      { return n_created; }
unsigned int pipe_read_bytes(void)   { return n_read; }
unsigned int pipe_write_bytes(void)  { return n_written; }
unsigned int pipe_dropped(void)      { return n_drop; }
