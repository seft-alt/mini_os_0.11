/* fs/fd.c —— 文件描述符表（契约见 include/fs/fd.h）
 *
 * 这一层只做三件事：
 *   1. 表本身：哪一格归哪个槽位、开着没有（**别人的格子绝不解引用**）；
 *   2. 名字分派：`/dev/tty`、`/dev/hd` 走设备表，其它走盘上的文件；
 *   3. 记账：累计 open/拒绝/读/写字节，给 `fd` 命令与回归脚本当证据。
 *
 * 真正的读写在 `fop` 后面：普通文件在 fs/file_dev.c，字符设备在
 * fs/char_dev.c，块设备在 fs/block_dev.c。这一层不再自己碰 fs_read /
 * fs_write —— 那正是 Linux 0.11 里 `fs/file_dev.c` 才该干的事。
 */
#include "linux/fd.h"
#include "linux/pipe.h"
#include "linux/string.h"

/* 静态表：TASK_SLOTS × FD_MAX。槽位就是进程，所以这里不需要"分配表本身"，
 * 也就不需要给表加生存期（那是 kmalloc 那套东西要解决的）。 */
static struct fd_file table[TASK_SLOTS][FD_MAX];

static unsigned int n_opens;
static unsigned int n_denied;
static unsigned int n_read;
static unsigned int n_written;

/* ---------------------------------------------------------------
 * 设备表：名字 → 设备号 + 一套 fops
 *
 * 只有两个名字，硬编码比"注册表"划算：注册表要解决重名、槽位、注销，
 * 而这里两个名字是编译期就定死的。等 tty/pipe 真的多起来再换注册表。
 * 名字必须**精确匹配**（含结尾 NUL），不做前缀匹配——`/dev/ttyx`
 * 不能被当成 `/dev/tty`。
 * --------------------------------------------------------------- */
struct dev_ent {
    const char *name;
    unsigned int len;
    unsigned int devno;
    const struct file_operations *fop;
};

static const struct dev_ent devs[] = {
    { "/dev/tty", 8, DEV_TTY, &tty_fops },
    { "/dev/hd",  7, DEV_HD,  &hd_fops  },
};

#define DEV_COUNT ((unsigned int)(sizeof(devs) / sizeof(devs[0])))

static const struct dev_ent *dev_find(const char *name)
{
    unsigned int i;

    for (i = 0; i < DEV_COUNT; i++) {
        unsigned int j;

        for (j = 0; j <= devs[i].len; j++) {
            if (name[j] != devs[i].name[j])
                break;
            if (j == devs[i].len && name[j] == '\0')
                return &devs[i];
        }
    }
    return 0;
}

static int fd_slot_ok(int slot)
{
    return slot >= 0 && slot < TASK_SLOTS;
}

/* 取一格：号越界/没打开/别人的 → 0。**别人的格子绝不解引用**，
 * 这是"fd 也是访问控制对象"的一条硬边界。 */
static struct fd_file *fd_get(int slot, int fd)
{
    if (!fd_slot_ok(slot))
        return 0;
    if (fd < 0 || fd >= FD_MAX)
        return 0;
    if (!table[slot][fd].used)
        return 0;
    return &table[slot][fd];
}

/* 名字要不要收：非空、不超过 FD_PATH_MAX、不含控制码/空格。
 * '/' 是允许的（v1.1 起这里收的是路径，不是单段名）；段与段合不合法
 * 交给 fs_path_lookup 判——"名字里有没有斜杠"不该在这里抄第二遍。 */
static int fd_name_ok(const char *name)
{
    unsigned int i;

    if (!name || name[0] == '\0')
        return 0;
    for (i = 0; i < FD_PATH_MAX; i++) {
        unsigned char c = (unsigned char)name[i];

        if (c == 0)
            return 1;
        if (c < 0x21 || c > 0x7E)
            return 0;
    }
    return 0;               /* 40 个字符还没见到 NUL：比 FD_PATH_MAX 还长 */
}

int fd_open(int slot, const char *name, int flags)
{
    struct fd_file *e = 0;
    unsigned int ino = 0, size = 0;
    const struct file_operations *fop = 0;
    const struct dev_ent *de;
    int i, rc;

    if (!fd_slot_ok(slot) || !fd_name_ok(name)) {
        n_denied++;
        return FD_E_ARG;
    }
    if (flags != FD_F_READ && flags != FD_F_WRITE &&
        flags != (FD_F_READ | FD_F_WRITE)) {
        n_denied++;
        return FD_E_ARG;
    }

    for (i = 0; i < FD_MAX; i++) {
        if (!table[slot][i].used) {
            e = &table[slot][i];
            break;
        }
    }
    if (!e) {
        n_denied++;
        return FD_E_FULL;           /* 不排队、不抢别人的：明确失败 */
    }

    /* 先查设备表，再查盘。顺序是刻意的：`/dev/tty` 这种名字如果
     * 盘上也有个同名文件，设备必须赢——否则 `exec` 的程序一 open
     * 控制台，打开的是个普通文件，谁也看不出错在哪。 */
    de = dev_find(name);
    if (de) {
        fop = de->fop;
        ino = de->devno;
        size = 0;                   /* 设备的容量由 open 回调自己填 */
    } else if (flags & FD_F_WRITE) {
        unsigned int mode = 0;

        /* 写路径也吃路径：fs_create 自己会把 "d1/x" 切成父 + 末段。
         * 父不存在/父是文件/父目录满，都由它按失败码回来。 */
        /* fs_create 是幂等的：0 = 新建，1 = 已经在了（沿用旧 ino，不重复建项） */
        rc = fs_create(name, &ino);
        if (rc < 0) {
            n_denied++;
            return FD_E_NOENT;
        }
        if (fs_path_lookup(name, &ino, &mode, &size) != 0 || mode != 1) {
            n_denied++;
            return FD_E_NOENT;      /* 建完却查不到：盘上的账不对，别硬撑 */
        }
        fop = &file_fops;
    } else {
        unsigned int mode = 0;

        if (fs_path_lookup(name, &ino, &mode, &size) != 0) {
            n_denied++;
            return FD_E_NOENT;
        }
        if (mode != 1) {            /* 目录不能当文件打开 */
            n_denied++;
            return FD_E_NOENT;
        }
        fop = &file_fops;
    }

    memset(e, 0, sizeof(*e));
    strcpy(e->name, name);          /* fd_name_ok 已经保证 ≤ FD_PATH_MAX 个字符 + NUL */
    e->ino = ino;
    e->size = size;
    e->off = 0;
    e->flags = flags;
    e->fop = fop;
    e->used = 1;

    if (fop->open) {
        rc = fop->open(e);
        if (rc < 0) {               /* 设备说自己不可用：把格子退回去 */
            memset(e, 0, sizeof(*e));
            n_denied++;
            return rc;
        }
    }
    n_opens++;
    return (int)(e - &table[slot][0]);      /* 下标就是 fd 号 */
}

/* 三件事都同一套骨架：取格 → 交给 fop → 记账。
 * 方向的判断在 fop 里（tty 允许双向、文件按 flags、hd 只读），
 * 因为"能不能读"是**文件类型**的属性，不是 fd 层的属性。 */
int fd_read(int slot, int fd, void *buf, unsigned int len)
{
    struct fd_file *e = fd_get(slot, fd);
    int r;

    if (!e) {
        n_denied++;
        return FD_E_BADFD;
    }
    r = e->fop->read(e, buf, len);
    if (r < 0) {
        n_denied++;
        return r;
    }
    n_read += (unsigned int)r;
    return r;
}

int fd_write(int slot, int fd, const void *buf, unsigned int len)
{
    struct fd_file *e = fd_get(slot, fd);
    int r;

    if (!e) {
        n_denied++;
        return FD_E_BADFD;
    }
    r = e->fop->write(e, buf, len);
    if (r < 0) {
        n_denied++;
        return r;
    }
    n_written += (unsigned int)r;
    return r;
}

int fd_seek(int slot, int fd, unsigned int off)
{
    struct fd_file *e = fd_get(slot, fd);
    int r;

    if (!e) {
        n_denied++;
        return FD_E_BADFD;
    }
    r = e->fop->lseek(e, off);
    if (r < 0) {
        n_denied++;
        return r;
    }
    return r;
}

int fd_close(int slot, int fd)
{
    struct fd_file *e = fd_get(slot, fd);

    if (!e) {
        n_denied++;
        return FD_E_BADFD;
    }
    if (e->fop->release)
        e->fop->release(e);         /* 现在都是空实现，接口先留着 */
    memset(e, 0, sizeof(*e));
    return 0;
}

int fd_close_all(int slot)
{
    int i, n = 0;

    if (!fd_slot_ok(slot))
        return 0;
    for (i = 0; i < FD_MAX; i++) {
        if (table[slot][i].used) {
            if (table[slot][i].fop->release)
                table[slot][i].fop->release(&table[slot][i]);
            memset(&table[slot][i], 0, sizeof(table[slot][i]));
            n++;
        }
    }
    return n;
}

unsigned int fd_used(int slot)
{
    unsigned int n = 0;
    int i;

    if (!fd_slot_ok(slot))
        return 0;
    for (i = 0; i < FD_MAX; i++)
        if (table[slot][i].used)
            n++;
    return n;
}

unsigned int fd_open_now(void)
{
    unsigned int n = 0, s;

    for (s = 0; s < TASK_SLOTS; s++)
        n += fd_used((int)s);
    return n;
}

unsigned int fd_opens(void)       { return n_opens; }
unsigned int fd_denied(void)      { return n_denied; }
unsigned int fd_read_bytes(void)  { return n_read; }
unsigned int fd_write_bytes(void) { return n_written; }

int fd_info(int slot, int fd, char **name, unsigned int *ino,
            unsigned int *off, unsigned int *size, int *flags)
{
    struct fd_file *e = fd_get(slot, fd);

    if (!e)
        return 0;
    if (name)  *name  = e->name;
    if (ino)   *ino   = e->ino;
    if (off)   *off   = e->off;
    if (size)  *size  = e->size;
    if (flags) *flags = e->flags;
    return 1;
}

const char *fd_kind(int slot, int fd)
{
    struct fd_file *e = fd_get(slot, fd);

    if (!e)
        return "none";
    return e->fop->kind;
}

/* ---------------------------------------------------------------
 * 管道：一次占两格（读端 + 写端），不经过 fd_open
 *
 * 为什么 fd 表满与管道表满都报 FD_E_FULL：两者都是"这个上限已经到了"，
 * 调用方唯一能做的都是先关掉别的东西。分开加两个错误码对谁都没用。
 * --------------------------------------------------------------- */
int fd_pipe(int slot, int *rd, int *wr)
{
    struct fd_file *r = 0, *w = 0;
    int a = -1, b = -1, i, ino;

    if (!fd_slot_ok(slot) || !rd || !wr) {
        n_denied++;
        return FD_E_ARG;
    }
    /* 先找两个空槽再申请环：反过来的话环表满时会把找槽的活白干，
     * 而且失败路径上还得回滚一个环。 */
    for (i = 0; i < FD_MAX; i++) {
        if (table[slot][i].used)
            continue;
        if (a < 0) {
            a = i;
        } else {
            b = i;
            break;
        }
    }
    if (b < 0) {
        n_denied++;
        return FD_E_FULL;           /* 必须两个格子：一个建不了管道 */
    }

    ino = pipe_new();
    if (ino < 0) {
        n_denied++;
        return FD_E_FULL;           /* 8 对已经都活着 */
    }

    r = &table[slot][a];
    w = &table[slot][b];

    memset(r, 0, sizeof(*r));
    strcpy(r->name, "|pipe");       /* 名字只给 `fd` 命令看：管道没有路径 */
    r->ino = (unsigned int)ino;     /* 设备文件同款：ino 存的是槽号，不是 inode */
    r->size = PIPE_BUF;             /* 容量，不是文件大小 */
    r->flags = FD_F_READ;
    r->fop = &pipe_fops;
    r->used = 1;

    memset(w, 0, sizeof(*w));
    strcpy(w->name, "|pipe");
    w->ino = (unsigned int)ino;
    w->size = PIPE_BUF;
    w->flags = FD_F_WRITE;
    w->fop = &pipe_fops;
    w->used = 1;

    n_opens += 2;                   /* 两个格子都是"打开"过的 */
    *rd = a;
    *wr = b;
    return 0;
}
