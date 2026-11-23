/* fs/block_dev.c —— 块设备的 file_operations（Linux 0.11 同名文件）
 *
 * 0.11 的 block_dev.c 干两件事：`block_read` / `block_write` 按块搬字节，
 * 以及维护一张 `blkdevs[]` 登记表。这里的登记表已经在 drivers/block 里
 * （`blkdev.c`），所以这一层只剩"把 fd 的 off 翻译成 (dev, lba, 扇内偏移)"。
 *
 * 设备：`/dev/hd` = **第一个有扇区的 blkdev 槽**。
 * 为什么不是写死的槽 0：槽号 = ATA 盘号，而这台机器的数据盘挂在
 * IDE 从盘（`-drive if=ide,index=1`）上，也就是槽 1；槽 0（主盘）根本不存在。
 * 写死 0 会在 open 时报 FD_E_IO，看起来像"设备层坏了"，其实只是找错了号。
 * 选"第一个非空槽"既对这台机器成立，也不需要谁去配置。
 *
 * 语义：
 *   read  —— 任意 offset、任意长度，跨扇区逐块拼（读到的就是盘上原始字节，
 *            **绕过 mosfs 与 inode**；块缓存是写穿的，所以两者不会看到不同的内容）；
 *   write —— 只收整扇区（off 与 len 都必须是 512 的整数倍），
 *            半扇区改写需要读-改-写，那是下一版的事；
 *   lseek —— 上限是这个设备的字节容量；
 *   size  —— open 时用 blkdev_sectors(dev) * 512 填好，没有盘就 open 失败。
 */
#include "linux/fd.h"
#include "linux/blkdev.h"
#include "linux/ata.h"

#define HD_SEC     BLK_SECTOR

/* 单扇区中转缓冲：hd_read/hd_write 都要"一次一扇区"地过手，
 * 用自己的一块，绝不借 ata_buffer()——那是 `disk` 命令与 ata 层的共享区，
 * 借了就会出现"源和目的同一个缓冲"的覆盖。512 字节落在 .bss。 */
static unsigned char hd_sec[HD_SEC];

static int hd_pick(void)
{
    unsigned int i;

    for (i = 0; i < BLK_MAX_DEV; i++)
        if (blkdev_sectors(i) > 0)
            return (int)i;
    return -1;
}

static int hd_open(struct fd_file *f)
{
    int dev = hd_pick();
    unsigned int sec;

    if (dev < 0)
        return FD_E_IO;                 /* 一个块设备都没登记 */
    sec = blkdev_sectors((unsigned int)dev);
    f->size = sec * HD_SEC;             /* 原始字节容量（2048 扇区 = 1MB） */
    return 0;
}

static int hd_read(struct fd_file *f, void *buf, unsigned int len)
{
    unsigned char *out = (unsigned char *)buf;
    unsigned int got = 0;
    int dev;

    if (!(f->flags & FD_F_READ))
        return FD_E_BADFD;
    if (!buf || len == 0 || len > FD_IO_MAX)
        return FD_E_ARG;
    if (f->off >= f->size)
        return 0;                       /* 已有的偏移已经是末尾 */
    if (len > f->size - f->off)
        len = f->size - f->off;

    dev = hd_pick();
    if (dev < 0)
        return FD_E_IO;

    while (got < len) {
        unsigned int pos = f->off + got;
        unsigned int lba = pos / HD_SEC;
        unsigned int in = pos % HD_SEC;
        unsigned int n = HD_SEC - in;
        unsigned int i;

        if (n > len - got)
            n = len - got;
        if (blkdev_read((unsigned int)dev, lba, hd_sec) != 0)
            return got > 0 ? (int)got : FD_E_IO;
        for (i = 0; i < n; i++)
            out[got + i] = hd_sec[in + i];
        got += n;
    }
    f->off += got;
    return (int)got;
}

static int hd_write(struct fd_file *f, const void *buf, unsigned int len)
{
    const unsigned char *in = (const unsigned char *)buf;
    unsigned int done;
    int dev;

    if (!(f->flags & FD_F_WRITE))
        return FD_E_BADFD;
    if (!buf || len == 0 || len > FD_IO_MAX)
        return FD_E_ARG;
    /* 整扇区才收：半扇区要 read-modify-write，这一版不做 */
    if ((f->off % HD_SEC) != 0 || (len % HD_SEC) != 0)
        return FD_E_ARG;
    if (len > f->size - f->off)
        return FD_E_ARG;

    dev = hd_pick();
    if (dev < 0)
        return FD_E_IO;

    for (done = 0; done < len; done += HD_SEC) {
        unsigned int j;

        for (j = 0; j < HD_SEC; j++)    /* 拷进中转缓冲，再整块交给 blkdev */
            hd_sec[j] = in[done + j];
        if (blkdev_write((unsigned int)dev, (f->off + done) / HD_SEC,
                         hd_sec) != 0)
            return done > 0 ? (int)done : FD_E_IO;
    }
    f->off += len;
    return (int)len;
}

static int hd_lseek(struct fd_file *f, unsigned int off)
{
    if (off > f->size)
        return FD_E_ARG;
    f->off = off;
    return 0;
}

const struct file_operations hd_fops = {
    "hd",
    hd_open,
    hd_read,
    hd_write,
    hd_lseek,
    0,
};
