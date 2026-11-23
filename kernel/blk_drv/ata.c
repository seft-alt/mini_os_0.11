/* drivers/ata.c —— 主 IDE 通道的 PIO 读/写一个扇区
 *
 * 端口（相对 0x1F0）：
 *   0 = 数据（16 位，一个扇区 = 256 次 inw/outw）
 *   1 = 错误（读）/ 特性（写）
 *   2 = 扇区数
 *   3/4/5 = LBA 低/中/高
 *   6 = 驱动器/头（bit4 选盘，bit5..7 = 0xA0 或 0xE0 的 LBA28 模式，低 4 位是 LBA24..27）
 *   7 = 状态（读）/ 命令（写）
 * 0x3F6 = 备用状态（读它不改变中断，正好用来做 400ns 延时）
 *
 * 时序上唯一容易写错的地方是"等谁"：发命令后先等 BSY 清，再看 ERR；
 * 真正搬数据前还要等 DRQ 置起。两次等待的方向相反，混起来就会读到全 0 或挂死。
 * 所有等待都带自旋上限——盘不在时宁可返回 -2，也不能把内核停在 hlt 里。
 *
 * 写（PIO 0x30）比读多一条纪律：**DRQ 是"盘要数据了"，不是"数据到了"**。
 * 所以在等 DRQ 之后立刻 outw 256 次（中间不能做别的），写完再等 BSY 落下。
 * 写完不做 CACHE FLUSH（0xE7）：QEMU 的 raw 后端是立即落盘的，真盘上那是下一步。 */
#include "linux/ata.h"
#include "asm/io.h"

#define ATA_BASE   0x1F0
#define ATA_ALT    0x3F6

#define R_DATA   (ATA_BASE + 0)
#define R_ERR    (ATA_BASE + 1)
#define R_NSECT  (ATA_BASE + 2)
#define R_LBA0   (ATA_BASE + 3)
#define R_LBA1   (ATA_BASE + 4)
#define R_LBA2   (ATA_BASE + 5)
#define R_DRV    (ATA_BASE + 6)
#define R_CMD    (ATA_BASE + 7)

#define ST_BSY   0x80
#define ST_DRDY  0x40
#define ST_DF    0x20
#define ST_DRQ   0x08
#define ST_ERR   0x01

#define CMD_READ   0x20
#define CMD_WRITE  0x30
#define CMD_IDENT  0xEC

#define ATA_SPIN 200000UL

static int present_mask;
static unsigned int sec_count[2];
static unsigned int n_reads, n_writes, n_errors;

/* 扇区缓冲：一块 512 字节的内存、两个名字（按 word 写、按 byte 读）。
 * 不是两个数组——两个数组会被"写进去的是这块、读出来的是那块"坑死。
 * 全局（static 但放在文件作用域）是故意的：测试要从符号里读回它，
 * 才能拿"内核读到的字节"和"镜像文件里的字节"逐位比对。 */
static union {
    unsigned short words[256];
    unsigned char bytes[ATA_SECTOR_BYTES];
} sec_buf;

/* 400ns 延时：读四次备用状态即可（每次读端口至少几十纳秒，四次足够过 400ns） */
static void ata_delay400(void)
{
    (void)inb(ATA_ALT);
    (void)inb(ATA_ALT);
    (void)inb(ATA_ALT);
    (void)inb(ATA_ALT);
}

/* 等 BSY 落下，返回最后一次读到的状态；超时返回 -1 */
static int ata_wait_bsy(int drive)
{
    unsigned long spin = 0;

    while (spin++ < ATA_SPIN) {
        unsigned char st = inb(R_CMD);

        if ((st & ST_BSY) == 0)
            return (int)st;
    }
    (void)drive;
    return -1;
}

/* 等 DRQ 置起（数据已经在 256 字的缓冲里），超时返回 -1 */
static int ata_wait_drq(void)
{
    unsigned long spin = 0;

    while (spin++ < ATA_SPIN) {
        unsigned char st = inb(R_CMD);

        if (st & ST_ERR)
            return -1;
        if ((st & ST_BSY) == 0 && (st & ST_DRQ))
            return (int)st;
    }
    return -1;
}

/* 选盘：0xA0 是主盘、0xB0 从盘；IDENTIFY 用 0xA0 型头 */
static void ata_select(int drive, unsigned int head_bits)
{
    outb(R_DRV, (unsigned char)(0xA0 | ((drive & 1) << 4) | head_bits));
    ata_delay400();
}

/* 认一个盘：返回 1 存在、0 不存在。顺带记扇区数 */
static int ata_identify(int drive)
{
    int i;
    unsigned int lo, hi;
    unsigned char st;

    ata_select(drive, 0);
    outb(R_NSECT, 0);
    outb(R_LBA0, 0);
    outb(R_LBA1, 0);
    outb(R_LBA2, 0);

    st = inb(R_CMD);
    if (st == 0x00 || st == 0xFF)            /* 无设备 / 浮空总线：一眼就能判死 */
        return 0;

    outb(R_CMD, CMD_IDENT);
    ata_delay400();

    if (ata_wait_bsy(drive) < 0)
        return 0;
    /* LBA 中/低非零 = 不是 ATA 设备（比如 ATAPI 光驱） */
    if (inb(R_LBA1) != 0 || inb(R_LBA2) != 0)
        return 0;
    if (ata_wait_drq() < 0)
        return 0;

    /* 读 256 个字：word 60/61 是 LBA28 总扇区数（低字在前） */
    lo = 0;
    hi = 0;
    for (i = 0; i < 256; i++) {
        unsigned short w = inw(R_DATA);

        if (i == 60)
            lo = w;
        else if (i == 61)
            hi = w;
    }
    sec_count[drive] = lo | (hi << 16);
    return 1;
}

int ata_probe(void)
{
    int d;

    present_mask = 0;
    n_reads = 0;
    n_writes = 0;
    n_errors = 0;
    sec_count[0] = 0;
    sec_count[1] = 0;

    for (d = 0; d < 2; d++) {
        if (ata_identify(d))
            present_mask |= (1 << d);
    }
    return present_mask;
}

int ata_present_mask(void)
{
    return present_mask;
}

unsigned int ata_sectors(int drive)
{
    if (drive < 0 || drive > 1)
        return 0;
    return sec_count[drive];
}

int ata_read(int drive, unsigned int lba, void *buf)
{
    int i, st;
    unsigned short *dst = (unsigned short *)buf;

    if (drive < 0 || drive > 1 || !(present_mask & (1 << drive))) {
        n_errors++;
        return -1;
    }
    if (!dst) {
        n_errors++;
        return -1;
    }

    st = ata_wait_bsy(drive);
    if (st < 0 || (st & ST_DRDY) == 0) {
        n_errors++;
        return -2;
    }

    outb(R_NSECT, 1);                       /* 只读一个扇区 */
    outb(R_LBA0, (unsigned char)(lba & 0xFF));
    outb(R_LBA1, (unsigned char)((lba >> 8) & 0xFF));
    outb(R_LBA2, (unsigned char)((lba >> 16) & 0xFF));
    /* 0xE0 = LBA28 模式；低 4 位是 LBA 的 bit24..27 */
    outb(R_DRV, (unsigned char)(0xE0 | ((drive & 1) << 4) | ((lba >> 24) & 0x0F)));
    ata_delay400();
    outb(R_CMD, CMD_READ);
    ata_delay400();

    if (ata_wait_bsy(drive) < 0) {
        n_errors++;
        return -2;
    }
    if (ata_wait_drq() < 0) {
        n_errors++;
        return -3;
    }

    for (i = 0; i < 256; i++)
        dst[i] = inw(R_DATA);

    n_reads++;
    return 0;
}

int ata_write(int drive, unsigned int lba, const void *buf)
{
    int i, st;
    const unsigned short *src = (const unsigned short *)buf;

    if (drive < 0 || drive > 1 || !(present_mask & (1 << drive))) {
        n_errors++;
        return -1;
    }
    if (!src || ((unsigned long)src & 1UL)) {        /* outw 要求 2 字节对齐 */
        n_errors++;
        return -1;
    }

    st = ata_wait_bsy(drive);
    if (st < 0 || (st & ST_DRDY) == 0) {
        n_errors++;
        return -2;
    }

    outb(R_NSECT, 1);                               /* 只写一个扇区 */
    outb(R_LBA0, (unsigned char)(lba & 0xFF));
    outb(R_LBA1, (unsigned char)((lba >> 8) & 0xFF));
    outb(R_LBA2, (unsigned char)((lba >> 16) & 0xFF));
    outb(R_DRV, (unsigned char)(0xE0 | ((drive & 1) << 4) | ((lba >> 24) & 0x0F)));
    ata_delay400();
    outb(R_CMD, CMD_WRITE);
    ata_delay400();

    if (ata_wait_bsy(drive) < 0) {
        n_errors++;
        return -2;
    }

    /* 写的方向：DRQ 置起 = 盘已经空出缓冲、等我们塞数据 */
    if (ata_wait_drq() < 0) {
        n_errors++;
        return -3;
    }

    for (i = 0; i < 256; i++)
        outw(R_DATA, src[i]);

    /* 写完等盘收工；ERR 置起说明这一扇区没落下去 */
    st = ata_wait_bsy(drive);
    if (st < 0 || (st & (ST_ERR | ST_DF)) != 0) {
        n_errors++;
        return -3;
    }

    n_writes++;
    return 0;
}

unsigned int ata_reads(void)
{
    return n_reads;
}

unsigned int ata_writes(void)
{
    return n_writes;
}

unsigned int ata_errors(void)
{
    return n_errors;
}

unsigned char *ata_buffer(void)
{
    return sec_buf.bytes;
}
