/* fs/char_dev.c —— 字符设备的 file_operations（Linux 0.11 同名文件）
 *
 * 0.11 的 char_dev.c 里是 `rw_ttyx` / `rw_tty` / `rw_memory` / `rw_kmem`
 * 四个函数，一个设备一对 rw。这里只有第一个设备：`/dev/tty`。
 *
 * 语义（和 POSIX tty 差得远，但每条都能验）：
 *   write —— 逐字节交给 COM1。**不写屏幕**：显存第 0 页是证据页、第 1 页
 *            归 shell 的环形区，再插一个写者进去，屏幕证据就不再可信了。
 *            控制台的"可见输出"仍然只有 shell 一个写者，`/dev/tty` 走的是
 *            脱屏通道——这和 printk 一个字符抄两份是同一个理由。
 *   read  —— 非阻塞：从键盘的**共享环（tag 0）**取一个字符，没有就返回 0
 *            （0 字节 = 这次没读到，不是 EOF、也不记拒绝）。一次一字节，
 *            因为环本身是按字符排的。
 *   lseek —— 拒绝。字符流没有"位置"这个概念，接受 seek 才是骗人。
 */
#include "linux/fd.h"
#include "linux/keyboard.h"
#include "linux/serial.h"

static unsigned int tty_out;

static int tty_open(struct fd_file *f)
{
    f->size = 0;                    /* tty 没有"容量"：size 只用于给 fd 打印 */
    return 0;
}

static int tty_read(struct fd_file *f, void *buf, unsigned int len)
{
    int c;

    if (!(f->flags & FD_F_READ))
        return FD_E_BADFD;          /* 以写模式打开的控制台不许读 */
    if (!buf || len == 0)
        return FD_E_ARG;

    c = kbd_getchar();              /* 共享环；没字符返回 -1 */
    if (c < 0)
        return 0;                   /* 非阻塞：这一次读到 0 字节 */
    ((char *)buf)[0] = (char)c;
    return 1;
}

static int tty_write(struct fd_file *f, const void *buf, unsigned int len)
{
    const char *p = (const char *)buf;
    unsigned int i;

    if (!(f->flags & FD_F_WRITE))
        return FD_E_BADFD;
    if (!buf || len == 0)
        return FD_E_ARG;

    for (i = 0; i < len; i++)
        serial_putc(p[i]);          /* serial 未初始化时这只是空操作（ser_on=0） */
    tty_out += len;
    return (int)len;
}

static int tty_lseek(struct fd_file *f, unsigned int off)
{
    (void)f;
    (void)off;
    return FD_E_ARG;                /* 字符流不可定位 */
}

const struct file_operations tty_fops = {
    "tty",
    tty_open,
    tty_read,
    tty_write,
    tty_lseek,
    0,
};

/* 证据：往 /dev/tty 一共写出去多少字节（`dev` 命令打印，回归脚本判这个数）。
 * 为什么不看串口日志当判据：日志里有 printk 自己的输出，`tty` 写进去的
 * 那几个字节混在里面，靠 grep 分不清是谁写的——计数器才是一手事实。 */
unsigned int tty_out_bytes(void)
{
    return tty_out;
}
