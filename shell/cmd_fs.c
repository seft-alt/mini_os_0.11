/* 文件系统与磁盘类命令：disk / fs / ls / cat / fd / write / rm。
 *
 * 由 shell/shell.c 拆分而来（纯搬运，逻辑未改）。
 * ============================================================== */
#include "linux/shell.h"
#include "linux/state.h"
#include "linux/console.h"
#include "linux/fmt.h"
#include "linux/cmds.h"
#include "linux/ata.h"
#include "linux/mosfs.h"
#include "linux/fd.h"
#include "linux/pipe.h"

/* `disk`：ATA 这一层的验收——有盘没有、多大、读一个扇区读回什么。
 * 默认读 LBA 0，`disk 12` 读第 12 个扇区（十进制，图省事）。
 * 三行输出各管一件事：谁是盘（mask/扇区数）、读成功没有（rc/账）、读到什么
 * （前 8 字节 + MBR 尾签名 0x55AA）。签名不在就说明"读到了别的扇区"或"全是 0"。 */
int cmd_disk(void)
{
    char line[SHELL_OUT_COLS];
    unsigned int k = 0, lba = 0, j, sec;
    int mask = ata_present_mask();
    int drv = (mask & 1) ? 0 : 1;
    int rc;
    unsigned char *b;
    unsigned short sig = 0;

    if (mask == 0) {
        shell_out_puts("ata none", 8);
        return SHELL_RC_OK;
    }

    if (shell_argc > 1) {
        for (j = 0; j < shell_argv_len[1]; j++) {
            char c = shell_argv_buf[1][j];

            if (c < '0' || c > '9')
                break;
            lba = lba * 10 + (unsigned int)(c - '0');
        }
    }

    sec = ata_sectors(drv);
    line[k++] = 'a'; line[k++] = 't'; line[k++] = 'a'; line[k++] = ' ';
    line[k++] = 'm'; line[k++] = '='; line[k++] = '0'; line[k++] = 'x';
    line[k++] = (char)('0' + ((mask & 1) ? 1 : 0));
    line[k++] = ' ';
    line[k++] = 's'; line[k++] = '='; line[k++] = '0'; line[k++] = 'x';
    line[k++] = (char)('0' + ((mask & 2) ? 1 : 0));
    line[k++] = ' ';
    line[k++] = 's'; line[k++] = 'e'; line[k++] = 'c'; line[k++] = '=';
    k += hex6(line + k, sec);
    shell_out_puts(line, k);

    rc = ata_read(drv, lba, ata_buffer());
    k = 0;
    line[k++] = 'd'; line[k++] = 'r'; line[k++] = 'v'; line[k++] = '=';
    line[k++] = (char)('0' + drv);
    line[k++] = ' ';
    line[k++] = 'l'; line[k++] = 'b'; line[k++] = 'a'; line[k++] = '=';
    k += hex6(line + k, lba);
    line[k++] = ' ';
    line[k++] = 'r'; line[k++] = 'c'; line[k++] = '=';
    /* rc 是负数（-1..-3）时打成一个固定串，避免"%"负数"在最小格式化器里没有 */
    if (rc == 0) {
        line[k++] = '0';
    } else {
        line[k++] = 'E';
        line[k++] = (char)('1' + (-rc - 1));
    }
    line[k++] = ' ';
    line[k++] = 'r'; line[k++] = 'd'; line[k++] = '=';
    k += udec(line + k, ata_reads());
    line[k++] = ' ';
    line[k++] = 'e'; line[k++] = 'r'; line[k++] = '=';
    k += udec(line + k, ata_errors());
    shell_out_puts(line, k);

    if (rc != 0)
        return SHELL_RC_OK;

    b = ata_buffer();
    k = 0;
    line[k++] = 'd'; line[k++] = '0'; line[k++] = ':';
    for (j = 0; j < 8; j++) {
        line[k++] = ' ';
        k += hex2b(line + k, b[j]);
    }
    shell_out_puts(line, k);

    /* 尾签名：字节 510 = 0x55、511 = 0xAA，按小端拼成一个字 = 0xAA55
     * （教科书里两种写法都见过：写成字节序列 55 AA，写成字值 AA55；
     * 这里按字值打，因为"小端机器上读出来是多少"才是不需要翻译的事实）。 */
    sig = (unsigned short)((b[511] << 8) | b[510]);
    k = 0;
    line[k++] = 's'; line[k++] = 'i'; line[k++] = 'g'; line[k++] = '=';
    k += hex4(line + k, sig);
    shell_out_puts(line, k);
    return SHELL_RC_OK;
}

/* `fs`：挂上了没有、多少块、多少 inode、读过几块（读次数是"缓存有没有起作用"的证据）。 */
int cmd_fs(void)
{
    char line[SHELL_OUT_COLS];
    unsigned int k = 0;

    line[k++] = 'f'; line[k++] = 's'; line[k++] = ' ';
    line[k++] = 'o'; line[k++] = 'k'; line[k++] = '=';
    line[k++] = '0'; line[k++] = 'x';
    line[k++] = (char)('0' + ((fs_blocks() != 0) ? 1 : 0));
    line[k++] = ' ';
    line[k++] = 'b'; line[k++] = '=';
    k += udec(line + k, fs_blocks());
    line[k++] = ' ';
    line[k++] = 'i'; line[k++] = '=';
    k += udec(line + k, fs_inodes());
    line[k++] = ' ';
    line[k++] = 'r'; line[k++] = 'd'; line[k++] = '=';
    k += udec(line + k, fs_reads());
    line[k++] = ' ';
    line[k++] = 'e'; line[k++] = 'r'; line[k++] = '=';
    k += udec(line + k, fs_errors());
    line[k++] = ' ';
    line[k++] = 'w'; line[k++] = 'r'; line[k++] = '=';
    k += udec(line + k, fs_writes());
    line[k++] = ' ';
    line[k++] = 'c'; line[k++] = 'r'; line[k++] = '=';
    k += udec(line + k, fs_creates());
    line[k++] = ' ';
    line[k++] = 'u'; line[k++] = 'n'; line[k++] = '=';
    k += udec(line + k, fs_unlinks());
    line[k++] = ' ';
    line[k++] = 'h'; line[k++] = 'i'; line[k++] = 't'; line[k++] = '=';
    k += udec(line + k, fs_hits());
    shell_out_puts(line, k);
    return SHELL_RC_OK;
}

/* `ls [路径]`：不带参数列根目录（走 mount 时那次 root_load 的内存表，零块读）；
 * 带路径就解析出目录 inode，再逐个读目录项——多级目录的证据就在这条上。 */
int cmd_ls(void)
{
    char line[SHELL_OUT_COLS];
    unsigned int n, i;

    if (shell_argc >= 2) {
        char path[FD_PATH_MAX + 1];
        unsigned int dir = 0, mode = 0, dsize = 0, k, j;

        for (k = 0; k < shell_argv_len[1] && k < FD_PATH_MAX; k++)
            path[k] = shell_argv_buf[1][k];
        path[k] = '\0';
        if (fs_path_lookup(path, &dir, &mode, &dsize) != 0) {
            shell_out_puts("ls: no such path", 17);
            return SHELL_RC_OK;
        }
        if (mode != 2) {
            shell_out_puts("ls: not a directory", 20);
            return SHELL_RC_OK;
        }
        n = fs_dir_count_at(dir);
        if (n == 0) {
            shell_out_puts("ls: empty", 9);
            return SHELL_RC_OK;
        }
        for (i = 0; i < n; i++) {
            char name[FS_NAME_MAX + 1];
            unsigned int ino, m, sz, k2 = 0;

            if (!fs_dir_entry_at(dir, i, name, &ino, &m, &sz))
                continue;
            for (j = 0; name[j] != '\0' && k2 < 20; j++)
                line[k2++] = name[j];
            while (k2 < 20)
                line[k2++] = ' ';
            k2 += udec(line + k2, sz);
            shell_out_puts(line, k2);
        }
        return SHELL_RC_OK;
    }

    n = fs_dir_count();
    if (n == 0) {
        shell_out_puts("ls: empty or not mounted", 24);
        return SHELL_RC_OK;
    }
    for (i = 0; i < n; i++) {
        char name[FS_NAME_MAX + 1];
        unsigned int ino, size, k = 0, j;

        if (!fs_dir_entry(i, name, &ino, &size))
            continue;
        for (j = 0; name[j] != '\0' && k < 20; j++)
            line[k++] = name[j];
        while (k < 20)                          /* 名字列宽固定 20，后面才好对 */
            line[k++] = ' ';
        k += udec(line + k, size);
        shell_out_puts(line, k);
    }
    return SHELL_RC_OK;
}

/* `mkdir <路径>`：父目录必须在、末段建新目录。rc=0 建好、rc=1 已存在、
 * rc=E1 父不存在、rc=E2 父不是目录/目录满、rc=E3 读写失败、rc=E5 inode 用完。 */
int cmd_mkdir(void)
{
    char path[FD_PATH_MAX + 1];
    char line[SHELL_OUT_COLS];
    unsigned int k = 0, j;
    int rc;

    if (shell_argc < 2) {
        shell_out_puts("mkdir <path>", 12);
        return SHELL_RC_OK;
    }
    for (j = 0; j < shell_argv_len[1] && j < FD_PATH_MAX; j++)
        path[j] = shell_argv_buf[1][j];
    path[j] = '\0';

    rc = fs_mkdir(path);
    k = put_lit(line, 0, "mkdir ");
    for (j = 0; path[j] != '\0' && k < 24; j++)
        line[k++] = path[j];
    k = put_lit(line, k, " rc=");
    k = put_rc(line, k, rc);
    shell_out_puts(line, k);
    return SHELL_RC_OK;
}

/* `cat <名字>`：**走 fd 层**（打开 → 读到 EOF → 关），按块读、按 '\n' 断行、
 * 超 79 列硬折。为什么不用 `printk` 直接打：那会把字符写到显存证据页的
 * 当前光标处，而 shell 的输出去向是用户页的环形区（shell_out_puts 才管滚动）。
 *
 * 为什么改走 fd 而不是继续 fs_lookup + fs_read 两段式：这是"用户程序和 shell
 * 共用同一条读路径"的证据来源。两条路径并存的话，fd 层只在 ring3 被验证，
 * 内核侧的 off/关闭语义就没人跑过；共用一条，`cat` 每跑一次就在给 fd 层做回归。 */
int cmd_cat(void)
{
    char name[FD_PATH_MAX + 1];
    char line[SHELL_OUT_COLS];
    unsigned char buf[256];             /* 一次读 256：读块次数与
                                         * "先查 inode 再读数据"的老路径一致 */
    unsigned int k = 0, j;
    int fd, n;

    if (shell_argc < 2) {
        shell_out_puts("cat <path>", 10);
        return SHELL_RC_OK;
    }
    for (j = 0; j < shell_argv_len[1] && j < FD_PATH_MAX; j++)
        name[j] = shell_argv_buf[1][j];
    name[j] = '\0';

    fd = fd_open(task_self(), name, FD_F_READ);
    if (fd < 0) {
        shell_out_puts("cat: no such file", 17);
        return SHELL_RC_OK;
    }

    while ((n = fd_read(task_self(), fd, buf, 256)) > 0) {
        int i;

        for (i = 0; i < n; i++) {
            char c = (char)buf[i];

            if (c == '\n') {
                shell_out_puts(line, k);
                k = 0;
                continue;
            }
            if (c == '\r')
                continue;
            if ((unsigned char)c < 0x20 || (unsigned char)c >= 0x7F)  /* 不可打印一律打成 '.'：
                                                  * 屏上是 79 列的文本，控制字符
                                                  * 会把整行扫成空白，反而看不出错 */
                c = '.';
            if (k >= SHELL_OUT_COLS) {
                shell_out_puts(line, k);
                k = 0;
            }
            line[k++] = c;
        }
    }
    if (k > 0)
        shell_out_puts(line, k);
    fd_close(task_self(), fd);          /* 读出错也关：不收就是漏一格给下一条命令 */
    return SHELL_RC_OK;
}

/* `fd`：fd 层的一行证据。
 *   fd           每个槽位开着几个，加累计 open/拒绝/读/写字节
 *   fd <名字>    跑一趟"打开 → 读 16 字节 → 关"，三行数字
 * 带名字这一路的用处是**内核侧也能被验证**：ring3 那条路径有 tools/fd.sh
 * 的探针兜着，但"表真能打开一个盘上的文件、off 真的跟着走"这件事不该
 * 只由用户态来证明——内核自己走一遍，两条路径才算都对上同一份账。 */
int cmd_fd(void)
{
    char name[FD_PATH_MAX + 1];
    char line[SHELL_OUT_COLS];
    unsigned char buf[16];              /* 只读 16 字节：够看 off 有没有跟着走 */
    unsigned int k, i, j, ino = 0, off = 0, size = 0;
    int fd, n;

    if (shell_argc < 2) {
        k = put_lit(line, 0, "fd");
        for (i = 0; i < TASK_SLOTS; i++) {
            line[k++] = ' ';
            line[k++] = 's';
            line[k++] = (char)('0' + (int)i);
            line[k++] = '=';
            k += udec(line + k, (unsigned long)fd_used((int)i));
        }
        k = put_lit(line, k, " o=");
        k += udec(line + k, (unsigned long)fd_opens());
        k = put_lit(line, k, " on=");
        k += udec(line + k, (unsigned long)fd_open_now());
        k = put_lit(line, k, " rd=");
        k += udec(line + k, (unsigned long)fd_read_bytes());
        k = put_lit(line, k, " wr=");
        k += udec(line + k, (unsigned long)fd_write_bytes());
        k = put_lit(line, k, " dn=");
        k += udec(line + k, (unsigned long)fd_denied());
        shell_out_puts(line, k);
        return SHELL_RC_OK;
    }

    for (j = 0; j < shell_argv_len[1] && j < FD_PATH_MAX; j++)
        name[j] = shell_argv_buf[1][j];
    name[j] = '\0';

    fd = fd_open(task_self(), name, FD_F_READ);
    k = put_lit(line, 0, "fd open rc=");
    k = put_rc(line, k, fd < 0 ? fd : 0);
    if (fd >= 0) {
        char *nm = 0;

        k = put_lit(line, k, " fd=");
        k += udec(line + k, (unsigned long)fd);
        if (fd_info(task_self(), fd, &nm, &ino, &off, &size, 0)) {
            const char *kd = fd_kind(task_self(), fd);

            k = put_lit(line, k, " ino=");
            k += udec(line + k, (unsigned long)ino);
            k = put_lit(line, k, " sz=");
            k += udec(line + k, (unsigned long)size);
            k = put_lit(line, k, " k=");
            for (i = 0; kd[i] != '\0' && k < SHELL_OUT_COLS - 1; i++)
                line[k++] = kd[i];
        }
    }
    shell_out_puts(line, k);
    if (fd < 0)
        return SHELL_RC_OK;

    n = fd_read(task_self(), fd, buf, 16);
    k = put_lit(line, 0, "fd read n=");
    k = put_rc(line, k, n);
    if (n > 0 && fd_info(task_self(), fd, 0, 0, &off, 0, 0)) {
        k = put_lit(line, k, " off=");
        k += udec(line + k, (unsigned long)off);
    }
    shell_out_puts(line, k);

    k = put_lit(line, 0, "fd close rc=");
    k = put_rc(line, k, fd_close(task_self(), fd));
    shell_out_puts(line, k);
    return SHELL_RC_OK;
}

/* `write <名字> <文本...>`：文件不存在就建，存在就往后追加一行（文本 + '\n'）。
 * 打印 rc / ino / 现在的 size / 这一趟落了几块：size 是"追加真的到了 inode 里"
 * 的证据，块数是"位图与新块真的走了一趟 ATA 写"的证据。 */
int cmd_write(void)
{
    char name[FD_PATH_MAX + 1];
    char text[132];
    char line[SHELL_OUT_COLS];
    unsigned int ino = 0, size = 0, k = 0, j, tl = 0;
    unsigned int w0, c0, w1, c1;
    int rc;

    if (shell_argc < 3) {
        shell_out_puts("write <path> <text>", 19);
        return SHELL_RC_OK;
    }
    for (j = 0; j < shell_argv_len[1] && j < FD_PATH_MAX; j++)
        name[j] = shell_argv_buf[1][j];
    name[j] = '\0';

    for (j = 2; j < shell_argc && tl < sizeof(text) - 1; j++) {
        unsigned int b;

        if (j > 2 && tl < sizeof(text) - 1)
            text[tl++] = ' ';
        for (b = 0; b < shell_argv_len[j] && tl < sizeof(text) - 1; b++)
            text[tl++] = shell_argv_buf[j][b];
    }
    text[tl++] = '\n';                       /* 每次追加都自成一行，cat 才看得懂 */

    w0 = fs_writes();
    c0 = fs_creates();
    rc = fs_create(name, &ino);              /* 0 = 新建，1 = 已有（改追加） */
    if (rc < 0) {
        shell_out_puts("write: create failed", 21);
        return SHELL_RC_OK;
    }
    rc = fs_write(ino, text, tl);
    w1 = fs_writes();
    c1 = fs_creates();

    line[k++] = 'w'; line[k++] = 'r'; line[k++] = 'i'; line[k++] = 't'; line[k++] = 'e';
    line[k++] = ' ';
    for (j = 0; name[j] != '\0' && k < 20; j++)
        line[k++] = name[j];
    line[k++] = ' ';
    line[k++] = 'r'; line[k++] = 'c'; line[k++] = '=';
    if (rc < 0) {
        line[k++] = 'E';
        line[k++] = (char)('0' + (unsigned int)(-rc) % 10);
    } else {
        line[k++] = '0';
    }
    line[k++] = ' ';
    line[k++] = 'i'; line[k++] = '=';
    k += udec(line + k, ino);
    if (fs_lookup(name, &ino, &size)) {
        line[k++] = ' ';
        line[k++] = 's'; line[k++] = 'z'; line[k++] = '=';
        k += udec(line + k, size);
    }
    line[k++] = ' ';
    line[k++] = 'w'; line[k++] = '=';
    k += udec(line + k, w1 - w0);
    line[k++] = ' ';
    line[k++] = 'n'; line[k++] = '=';
    k += udec(line + k, c1 - c0);
    shell_out_puts(line, k);
    return SHELL_RC_OK;
}

/* `rm <路径>`：文件调 fs_unlink，目录调 fs_rmdir（只删得掉空的）。
 * 打印 rc 与这一趟之后 `ls` 还剩几项——"名字真的从目录里消失了"才看得见。 */
int cmd_rm(void)
{
    char name[FD_PATH_MAX + 1];
    char line[SHELL_OUT_COLS];
    unsigned int k = 0, j, n0, n1;
    int rc, rootpath = 1;

    if (shell_argc < 2) {
        shell_out_puts("rm <path>", 10);
        return SHELL_RC_OK;
    }
    for (j = 0; j < shell_argv_len[1] && j < FD_PATH_MAX; j++)
        name[j] = shell_argv_buf[1][j];
    name[j] = '\0';

    /* n0 -> n1 是"根目录还剩几项"，带 '/' 的名字动的是子目录，
     * 这个数就没有意义了：印 0 -> 0，别拿它当证据。 */
    for (j = 0; name[j] != '\0'; j++)
        if (name[j] == '/')
            rootpath = 0;

    n0 = rootpath ? fs_dir_count() : 0;
    rc = fs_unlink(name);
    if (rc == -2)
        rc = fs_rmdir(name);                 /* 是目录：只删空的，非空仍是 -2 */
    n1 = rootpath ? fs_dir_count() : 0;

    line[k++] = 'r'; line[k++] = 'm'; line[k++] = ' ';
    for (j = 0; name[j] != '\0' && k < 20; j++)
        line[k++] = name[j];
    line[k++] = ' ';
    line[k++] = 'r'; line[k++] = 'c'; line[k++] = '=';
    if (rc == 0) {
        line[k++] = '0';
    } else {
        line[k++] = 'E';
        line[k++] = (char)('0' + (unsigned int)(-rc) % 10);
    }
    line[k++] = ' ';
    line[k++] = 'n'; line[k++] = '=';
    k += udec(line + k, n0);
    line[k++] = '-'; line[k++] = '>';
    k += udec(line + k, n1);
    shell_out_puts(line, k);
    return SHELL_RC_OK;
}

/* `dev`：设备节点（`/dev/tty`、`/dev/hd`）走 fd 层的证据。三行各管一件事：
 *
 *   dev tty w=<N>  —— 往 `/dev/tty` 写 4 个字节，N 是 tty 层自己的计数器。
 *                     写的是 COM1（脱屏），所以屏上不会多出字符；"写成功"
 *                     靠这个数，不靠 grep 串口日志（日志里混着 printk）。
 *   dev hd sz=<N> n=<M> sig=<0xAA55>
 *                  —— 打开 `/dev/hd`、读 LBA0 的 512 字节；sz 是 open 时
 *                     按扇区数算出的容量，sig 是盘上真实的 MBR 尾签名。
 *                     这三项一起说明"设备层真的翻译成了扇区读"。
 *   dev bad=E<N> on=<k>
 *                  —— `/dev/xx` 既不在设备表、盘上也没有 → E3；
 *                     on 必须回到 0，否则设备打开的格子没收干净。
 */
int cmd_dev(void)
{
    char line[SHELL_OUT_COLS];
    char *nm = 0;
    unsigned char *b;
    unsigned int k, w0, w1, size = 0, sig = 0;
    int fd, n = 0;

    /* ---- 1. /dev/tty：写 4 字节 ---- */
    w0 = tty_out_bytes();
    fd = fd_open(task_self(), "/dev/tty", FD_F_WRITE);
    if (fd >= 0) {
        fd_write(task_self(), fd, "tty\n", 4);
        fd_close(task_self(), fd);
    }
    w1 = tty_out_bytes();
    k = put_lit(line, 0, "dev tty w=");
    k += udec(line + k, w1 - w0);
    shell_out_puts(line, k);

    /* ---- 2. /dev/hd：读 LBA0 ---- */
    b = ata_buffer();
    fd = fd_open(task_self(), "/dev/hd", FD_F_READ);
    k = put_lit(line, 0, "dev hd sz=");
    if (fd >= 0) {
        fd_info(task_self(), fd, &nm, 0, 0, &size, 0);
        k += udec(line + k, size);
        n = fd_read(task_self(), fd, b, 512);
        k = put_lit(line, k, " n=");
        k = put_rc(line, k, n);
        if (n >= 512) {
            sig = (unsigned int)((b[511] << 8) | b[510]);
            k = put_lit(line, k, " sig=");
            k += hex4(line + k, (unsigned short)sig);
        }
        fd_close(task_self(), fd);
    } else {
        k = put_lit(line, k, "E");
        k += udec(line + k, (unsigned long)(-fd));
        k = put_lit(line, k, " n=0 sig=0000");
    }
    shell_out_puts(line, k);

    /* ---- 3. 不存在的设备名 ---- */
    fd = fd_open(task_self(), "/dev/xx", FD_F_READ);
    k = put_lit(line, 0, "dev bad=E");
    k += udec(line + k, fd < 0 ? (unsigned long)(-fd) : 0);
    k = put_lit(line, k, " on=");
    k += udec(line + k, (unsigned long)fd_open_now());
    shell_out_puts(line, k);
    return SHELL_RC_OK;
}

/* `pipe`：管道这一层的自检——建一对 → 5 字节来回 → 写满测短写 → 两端关。
 *
 * 两行证据：
 *   pipe r=<k> w=<m> n=5 ok=1
 *                  —— 读端/写端各拿一个 fd 号，写 5 字节后读回来逐字节对上；
 *                     ok=1 表示环里出来的就是进去的那 5 个字节。
 *   pipe big w=512 d=88 rd=512 live=0
 *                  —— 第二对：写 600 字节只收下 512（一个环的容量），
 *                     其余 88 字节记"丢弃"；再读 512 证明环里真是满的；
 *                     两端关掉后 live 归 0，说明槽没泄漏。
 */
int cmd_pipe(void)
{
    char line[SHELL_OUT_COLS];
    const char *mark = "pipe\n";
    unsigned char buf[8];
    unsigned char big[600];
    unsigned int k, i, w0, w1, d0, d1;
    int rfd = -1, wfd = -1, rc, n, m, same = 1;

    /* ---- 1. 5 字节来回：写进去、读回来、逐字节对上 ---- */
    rc = fd_pipe(task_self(), &rfd, &wfd);
    if (rc < 0) {
        k = put_lit(line, 0, "pipe E");
        k += udec(line + k, (unsigned long)(-rc));
        k = put_lit(line, k, " live=");
        k += udec(line + k, pipe_live());
        shell_out_puts(line, k);
        return SHELL_RC_UNKNOWN;
    }
    fd_write(task_self(), wfd, mark, 5);
    n = fd_read(task_self(), rfd, buf, sizeof(buf));
    if (n != 5)
        same = 0;
    for (i = 0; i < (unsigned int)n && i < 5; i++)
        if (buf[i] != (unsigned char)mark[i])
            same = 0;
    fd_close(task_self(), rfd);
    fd_close(task_self(), wfd);

    k = put_lit(line, 0, "pipe r=");
    k += udec(line + k, (unsigned long)rfd);
    k = put_lit(line, k, " w=");
    k += udec(line + k, (unsigned long)wfd);
    k = put_lit(line, k, " n=");
    k += udec(line + k, (unsigned long)n);
    k = put_lit(line, k, " ok=");
    k += udec(line + k, (unsigned long)same);
    shell_out_puts(line, k);

    /* ---- 2. 写满：600 字节只进 512，其余记丢弃 ---- */
    for (i = 0; i < (unsigned int)sizeof(big); i++)
        big[i] = 'x';

    rc = fd_pipe(task_self(), &rfd, &wfd);
    if (rc < 0) {
        k = put_lit(line, 0, "pipe big E");
        k += udec(line + k, (unsigned long)(-rc));
        shell_out_puts(line, k);
        return SHELL_RC_UNKNOWN;
    }
    w0 = pipe_write_bytes();
    d0 = pipe_dropped();
    n = fd_write(task_self(), wfd, big, (unsigned int)sizeof(big));  /* 短写 */
    w1 = pipe_write_bytes() - w0;
    d1 = pipe_dropped() - d0;
    m = fd_read(task_self(), rfd, big, PIPE_BUF);   /* 环里那满的 512 */
    fd_close(task_self(), rfd);
    fd_close(task_self(), wfd);

    k = put_lit(line, 0, "pipe big w=");
    k += udec(line + k, w1);
    k = put_lit(line, k, " d=");
    k += udec(line + k, d1);
    k = put_lit(line, k, " rd=");
    k += udec(line + k, (unsigned long)m);
    k = put_lit(line, k, " live=");
    k += udec(line + k, pipe_live());
    shell_out_puts(line, k);
    return SHELL_RC_OK;
}