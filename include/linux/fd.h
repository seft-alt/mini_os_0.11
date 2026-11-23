/* ==============================================================
 * fs/fd.h —— 文件描述符层：把 mosfs 的 (ino, offset) 收成"一个整数"
 *
 * 在 fd 这层之前，读文件只有一种姿势：调用方自己拿着 `ino` 和 `offset`，
 * 每读一段就自己加偏移。这在 shell 里没问题（一个 for 循环就够了），
 * 但对 ring3 说不通——用户程序不该知道 inode 号，更不该自己维护偏移：
 * 那等于把"文件读到哪儿了"这个状态放到最不可信的地址空间里。
 *
 * fd 层就是那个状态的家：**每槽位一张表**（和 tasks[] 一一对应），
 * 格子里放 { 名字, ino, 快照 size, 当前 offset, 读/写标志 }。
 * 用户程序只拿得到下标（0..FD_MAX-1），所有真实动作仍由 fs_* 完成——
 * 这一层不碰盘，只记账、做边界判断。
 *
 * 为什么"每槽位一份"而不是"每进程一份"：这个内核里槽位就是进程，
 * 而且 TASK_SLOTS 是编译期常量，静态表能直接落进 .bss（不需要堆，
 * 也就不需要"表本身的生存期"这套问题）。代价是槽位满就明确失败。
 *
 * 三个必须成立的不变量：
 *   1. 一个 fd 只归一个槽位。拿别人的 fd 号，返回 FD_E_BADFD，
 *      绝不去看那一格——这是"fd 也是访问控制对象"的那半条。
 *   2. 任务死掉时整表清空（fd_close_all(slot)，接在 sys_user_window_reset
 *      旁边）。否则下一个任务会继承"别人打开的文件 + 别人的偏移"。
 *   3. 读模式的 size 在 open 那一刻快照，之后不再刷新；
 *      写模式每写一次刷新（追加会改 size，不刷会停在旧末尾）。
 *      两条语义不同是**故意的**：读端看到的边界开文件时就定了，这叫一致视图。
 * ============================================================== */
#ifndef MINI_OS_FS_FD_H
#define MINI_OS_FS_FD_H

#include "linux/task.h"         /* TASK_SLOTS */
#include "linux/mosfs.h"           /* FS_NAME_MAX */

#define FD_MAX      4           /* 每个任务同时能开几个（够 cat/exec/写一条链） */

/* open 接受**路径**（v1.1 起 mosfs 有多级目录）：上限给到 40 字节，
 * 足够 `/a/b/c/d.txt` 这种深度；单段名仍是最常见的情形。 */
#define FD_PATH_MAX 40

/* fd 这一层**不设小块上限**：一次读多大由调用方定（内核的 cat 一次 256 字节，
 * 用户态被系统调用边界 SYS_FD_IO_MAX 卡在 64，因为它的缓冲只有窗口那么大）。
 * 这里只封顶到"一个文件的全部"，那已经是盘上能有的最大值了。
 * 把两个上限分开是一次真实踩坑的结果：第一版让 fd 层也限 64，
 * 内核的 cat 于是一个 169 字节的文件要读 3 趟，每趟重读一遍 inode 块，
 * fs.sh 的"读块次数 ≤16"当场红掉——限制放错了边界。 */
#define FD_IO_MAX   FS_MAX_BYTES

/* open 的标志位（不是 POSIX 的 O_RDONLY/O_WRONLY 那套数值，没必要照抄） */
#define FD_F_READ   0x01
#define FD_F_WRITE  0x02

/* 失败码：一律负数。门把它们原样交给 ring3，用户程序比 -1 就知道成败。 */
#define FD_E_ARG    (-1)        /* 参数不对（名字空、标志非法、长度为 0/超限） */
#define FD_E_FULL   (-2)        /* 这个槽位的表满了 */
#define FD_E_NOENT  (-3)        /* 文件不在（读模式）或建不出来（写模式） */
#define FD_E_BADFD  (-4)        /* fd 号越界 / 没打开 / 不是这个槽位的 / 方向不对 */
#define FD_E_IO     (-5)        /* 底下的 fs_* 报错 */

/* ---------------------------------------------------------------
 * 设备号：`ino` 这一格在设备文件里存的是它，不是 inode 号
 * --------------------------------------------------------------- */
#define DEV_NONE    0
#define DEV_TTY     1           /* `/dev/tty`：串口写 + 键盘环读 */
#define DEV_HD      2           /* `/dev/hd`：块设备原始扇区（槽 0 = 数据盘） */

struct fd_file;

/* ===============================================================
 * struct file_operations：一个 fd 的"怎么能动它"
 *
 * 这是 Linux 0.11 的 `fs/file_dev.c` / `fs/char_dev.c` / `fs/block_dev.c`
 * 那层抽象的最小版：fd 层只管"这一格归谁、开到哪儿了"，
 * 具体怎么读怎么写，全部转手给这里的函数指针。
 *
 * 五个回调都有明确分工：
 *   open    设备初始化（普通文件不需要：ino/size 在 open 时已经查好了）
 *   read/write 真正动字节，返回字节数或 FD_E_*；方向检查由各自实现做
 *   lseek  只改 off，越界怎么算由各自实现定
 *   release 关掉时的收尾（现在都还是空的，留着是因为下一层 tty/pipe 要用）
 *
 * `kind` 只用于打证据（`dev` 命令与 `fd` 命令）——它是字符串常量，
 * 不占表空间。
 * =============================================================== */
struct file_operations {
    const char *kind;
    int  (*open)(struct fd_file *f);
    int  (*read)(struct fd_file *f, void *buf, unsigned int len);
    int  (*write)(struct fd_file *f, const void *buf, unsigned int len);
    int  (*lseek)(struct fd_file *f, unsigned int off);
    void (*release)(struct fd_file *f);
};

/* fd 表里的一格。三个实现（file_dev / char_dev / block_dev）都要能
 * 看到它，所以结构定义在头里，表本身仍是 fd.c 的 static。 */
struct fd_file {
    char name[FD_PATH_MAX + 1];     /* open 时抄一份：刷新 size 与打证据都要用 */
    unsigned int ino;               /* 普通文件 = inode 号；设备文件 = DEV_* */
    unsigned int size;              /* 读模式：open 时的快照；写模式：每写一次刷新 */
    unsigned int off;
    int flags;
    int used;
    const struct file_operations *fop;
};

/* 四套实现（各在自己的文件里定义） */
extern const struct file_operations file_fops;  /* fs/file_dev.c 普通文件 */
extern const struct file_operations tty_fops;   /* fs/char_dev.c  /dev/tty */
extern const struct file_operations hd_fops;    /* fs/block_dev.c /dev/hd */
extern const struct file_operations pipe_fops;  /* fs/pipe.c     管道（0.11 同名） */

/* 建一对管道 fd：读端写端各占一格，`*rd`/`*wr` 拿号；返回 0 或 FD_E_*。
 * 为什么不走 fd_open：管道没有名字（0.11 里它是 inode，我们这里没有 inode 池），
 * 也不该被"打开"——它是一次成对创建的。 */
int fd_pipe(int slot, int *rd, int *wr);

/* 打开一个文件，返回 fd（0..FD_MAX-1）或上面的负值。
 * 写模式会自动建（fs_create 幂等：已存在就沿用旧 ino，不重复建）。 */
int fd_open(int slot, const char *name, int flags);

/* 从当前 offset 读一段：返回读到的字节数（0 = 已经到末尾），<0 = 失败 */
int fd_read(int slot, int fd, void *buf, unsigned int len);

/* 往文件尾追加一段（mosfs v1 只有追加，没有覆盖写）：返回写入字节数，<0 = 失败 */
int fd_write(int slot, int fd, const void *buf, unsigned int len);

/* 关一个 fd：返回 0，<0 = 失败 */
int fd_close(int slot, int fd);

/* 把某个槽位整表关掉（任务死亡 / 槽位复用）。返回关掉了几个。
 * 只会清这一个槽位——别把"清自己"写成"清全部"。 */
int fd_close_all(int slot);

/* 把某个 fd 的偏移挪到 off（0..FD_MAX_BYTES 之外一律 FD_E_ARG）。
 * 存在的理由是"读文件的第 N 段"不必每次从 0 开始重读。 */
int fd_seek(int slot, int fd, unsigned int off);

/* ---------------------------------------------------------------
 * 证据接口（shell 的 `fd` 命令与 tools/fd.sh 的判据都从这里取数）
 * --------------------------------------------------------------- */
unsigned int fd_used(int slot);        /* 这个槽位现在还开着几个 */
unsigned int fd_open_now(void);        /* 全表现开着的总数（跑完该回 0） */
unsigned int fd_opens(void);           /* 累计打开成功次数 */
unsigned int fd_denied(void);          /* 累计拒绝次数（每个失败码一次） */
unsigned int fd_read_bytes(void);      /* 累计读出的字节 */
unsigned int fd_write_bytes(void);     /* 累计写进的字节 */

/* 取某一格的信息（给 shell 打证据用；越界或没打开返回 0=失败）。
 * 指针都可以给 0，表示"这一项不要"。 */
int fd_info(int slot, int fd, char **name, unsigned int *ino,
            unsigned int *off, unsigned int *size, int *flags);

/* 这一格是哪种文件：`file` / `tty` / `hd`；没打开返回 `none`。
 * 证据要能一眼看出"这个 fd 后面接的是谁"，不然 `dev` 命令的
 * sz/sig 到底是谁给的说不清。 */
const char *fd_kind(int slot, int fd);

/* 往 `/dev/tty` 写出去的总字节（fs/char_dev.c 里那个计数器）。
 * 判据为什么不是"串口日志里有没有那几个字节"：日志里混着 printk，
 * 分不清谁写的；计数器才是一手事实。 */
unsigned int tty_out_bytes(void);

#endif /* MINI_OS_FS_FD_H */
