/* fs/file_dev.c —— 盘上普通文件的 file_operations（Linux 0.11 同名文件）
 *
 * 0.11 里 file_read / file_write 干的就是这件事：拿到 `filp->f_pos` 与
 * `inode->i_size`，调 bmap 找块、bread/bwrite 动字节，最后把 pos 推上去。
 * 这里换成了 mosfs 的 fs_read / fs_write，但分工一模一样——**偏移归这一层，
 * 块映射归文件系统**。
 *
 * 三条语义在搬过来时原样保留（fd.sh / vfs.sh 都靠它们判）：
 *   1. 读模式的 size 是 open 那一刻的快照，之后不再刷新（一致视图）；
 *   2. 写模式每写一次刷新 size，并把 off 顶到新末尾（mosfs 只有追加）；
 *   3. EOF 返回 0，不记拒绝；只有底下真报错才返回 FD_E_IO。
 */
#include "linux/fd.h"
#include "linux/mosfs.h"

static int file_read(struct fd_file *f, void *buf, unsigned int len)
{
    int r;

    if (!(f->flags & FD_F_READ))
        return FD_E_BADFD;          /* 以写模式打开的 fd 不许读 */
    if (!buf || len == 0 || len > FD_IO_MAX)
        return FD_E_ARG;
    if (f->off >= f->size)
        return 0;                   /* EOF：0 是合法结果，不当拒绝计数 */
    if (len > f->size - f->off)     /* 不许读过快照的末尾（减法，不用加法判溢出） */
        len = f->size - f->off;

    r = fs_read(f->ino, f->off, buf, len);
    if (r < 0)
        return FD_E_IO;
    f->off += (unsigned int)r;
    return r;
}

static int file_write(struct fd_file *f, const void *buf, unsigned int len)
{
    unsigned int ino = 0, size = 0;
    int r;

    if (!(f->flags & FD_F_WRITE))
        return FD_E_BADFD;
    if (!buf || len == 0 || len > FD_IO_MAX)
        return FD_E_ARG;

    r = fs_write(f->ino, buf, len);     /* mosfs v1：只能往尾部追加 */
    if (r <= 0)
        return FD_E_IO;

    /* 追加改了 inode 的 size，表里那份快照必须跟着走：不刷的话下一次
     * 读会以为还在旧末尾，读 0 字节（"写了却读不到"）。
     * 名字可能是路径，所以这里也走 fs_path_lookup。 */
    {
        unsigned int mode = 0;

        if (fs_path_lookup(f->name, &ino, &mode, &size) == 0 && mode == 1) {
            f->size = size;
            f->off = size;              /* 写模式的"当前位置"就在文件尾 */
        }
    }
    return r;
}

static int file_lseek(struct fd_file *f, unsigned int off)
{
    /* 允许 seek 到恰好 size（"下一次读就是 EOF"），再往后就没意义了 */
    if (off > f->size)
        return FD_E_ARG;
    f->off = off;
    return 0;
}

const struct file_operations file_fops = {
    "file",
    0,                  /* open：普通文件不需要——ino/size 在 fd_open 里已经查好 */
    file_read,
    file_write,
    file_lseek,
    0,                  /* release：没有要收的资源 */
};
