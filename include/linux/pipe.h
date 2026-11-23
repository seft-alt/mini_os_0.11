/* fs/pipe.h —— 管道：没有 inode 的读端 / 写端（Linux 0.11 的同名文件）
 *
 * 0.11 的 `fs/pipe.c` 把管道挂在 inode 上：create_pipe 造一对 inode，
 * 数据放在 inode 指向的一块页（PIPE_SIZE = 4096），读端空转时 `sleep_on`。
 *
 * 这个内核没有 inode 池，也还没有"带原因的等待与唤醒"，所以先做最小那半：
 *
 *   - 固定 8 个槽的静态表，每槽一个 512 字节环（正好一个扇区，便于讲故事）；
 *   - 建管道**不造 inode**：`fd_pipe()` 直接占两个 fd 格子，`ino` 存槽号；
 *   - **非阻塞**：空返回 0、满返回短写；0 表示"这次没有"，不是 EOF——
 *     和 `/dev/tty` 的 read 语义一致；
 *   - 引用计数按方向分开：读端写端各自 release，两端都没了才回收槽。
 *
 * 顺序与 0.11 的差异是**故意的**：阻塞要 wait/wakeup + 唤醒原因，
 * 那是 tty 与 shell 管道共用的一步，放在这里会把两件事搅在一起。
 */
#ifndef MINI_OS_FS_PIPE_H
#define MINI_OS_FS_PIPE_H

#include "linux/fd.h"           /* FD_E_*、struct file_operations */

#define PIPE_MAX  8             /* 全表最多同时 8 对 */
#define PIPE_BUF  512           /* 每对一个环，正好一个扇区 */

/* 槽的申请与回收（只有 fs/fd.c 的 fd_pipe 与 pipe_release 调）。
 * pipe_new 起手就把 readers/writers 各记一次——管道一定同时建两端。 */
int  pipe_new(void);
void pipe_free(int ino);

/* 证据（`pipe` 命令与 tools/pipe.sh 的判据都从这里取） */
unsigned int pipe_live(void);          /* 现在还活着的对数（关完该回 0） */
unsigned int pipe_created(void);       /* 累计建过几对 */
unsigned int pipe_read_bytes(void);    /* 累计读出的字节 */
unsigned int pipe_write_bytes(void);   /* 累计写进的字节 */
unsigned int pipe_dropped(void);       /* 因为环满而没收下的字节 */

#endif /* MINI_OS_FS_PIPE_H */
