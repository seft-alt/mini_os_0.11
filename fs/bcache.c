/* fs/bcache.c —— 8 槽块缓存（契约见 include/fs/bcache.h）
 *
 * 数据本体是**从页分配器要的一页 4KB**（8 × 512 正好），不是 .bss
 * 里的静态数组。原因很实际：kernel-fit 要求映像尾部离 PAGES_BASE
 * 至少留一页；把 4KB 静态数组塞进 .bss，__kernel_end 会直接贴边。
 * 一页缓存常驻，代价是分配器少一页；换来的是读数上的判据（命中/
 * 未命中），而不是靠"单块缓存"赌访问顺序。
 *
 * 每格的归属（dev/lba/valid）另放静态小表：512 字节全给数据，
 * 没地方塞元信息。拿不到页时退化成一格不留：read/write 直接走
 * blkdev，统计照记。
 * ============================================================== */
#include "linux/bcache.h"
#include "linux/blkdev.h"
#include "linux/page.h"

static struct {
    unsigned int dev;
    unsigned int lba;
    unsigned int valid;
} tags[BCACHE_SLOTS];

static unsigned char *pool;     /* 一页，页对齐、2 字节对齐都满足 */
static unsigned int pool_phys;

unsigned long bcache_hits_n;
unsigned long bcache_misses_n;
unsigned long bcache_write_n;
unsigned long bcache_fail_n;

static unsigned char *line_bytes(unsigned int s)
{
    return pool + s * BCACHE_SECTOR;
}

/* dev 参与取槽：两块盘上的同一个 LBA 不会挤到同一格，
 * 否则「主盘 LBA1 与从盘 LBA1」会互相冒充（内容完全不同）。 */
static unsigned int line_slot(unsigned int dev, unsigned int lba)
{
    return (lba + dev * 7u) & (BCACHE_SLOTS - 1u);
}

static void copy_bytes(unsigned char *dst, const unsigned char *src)
{
    unsigned int i;

    for (i = 0; i < BCACHE_SECTOR; i++)
        dst[i] = src[i];
}

void bcache_init(void)
{
    unsigned int i;

    if (!pool) {                    /* 首次：向分配器要一页（page_alloc 已清零） */
        pool_phys = page_alloc();
        if (pool_phys)
            pool = (unsigned char *)pool_phys;
    }
    for (i = 0; i < BCACHE_SLOTS; i++)
        tags[i].valid = 0;
    bcache_hits_n = 0;
    bcache_misses_n = 0;
    bcache_write_n = 0;
    bcache_fail_n = 0;
}

int bcache_read(unsigned int dev, unsigned int lba, void *buf)
{
    unsigned int s = line_slot(dev, lba);
    unsigned char *dst = (unsigned char *)buf;

    if (!buf)
        return -1;
    if (!pool) {                    /* 没要到页：直读，只记未命中 */
        if (blkdev_read(dev, lba, buf) != 0) {
            bcache_misses_n++;
            bcache_fail_n++;
            return -1;
        }
        bcache_misses_n++;
        return 0;
    }
    if (tags[s].valid && tags[s].dev == dev && tags[s].lba == lba) {
        bcache_hits_n++;
        copy_bytes(dst, line_bytes(s));
        return 0;
    }

    /* 未命中：先读进槽，成功后再认这格是它的。读失败就把这格标无效，
     * 免得下一次命中拿到半截内容。 */
    if (blkdev_read(dev, lba, line_bytes(s)) != 0) {
        tags[s].valid = 0;
        bcache_misses_n++;
        bcache_fail_n++;
        return -1;
    }
    tags[s].dev = dev;
    tags[s].lba = lba;
    tags[s].valid = 1;
    bcache_misses_n++;
    copy_bytes(dst, line_bytes(s));
    return 0;
}

int bcache_write(unsigned int dev, unsigned int lba, const void *buf)
{
    unsigned int s = line_slot(dev, lba);
    const unsigned char *src = (const unsigned char *)buf;

    if (!buf)
        return -1;
    if (blkdev_write(dev, lba, buf) != 0) {
        tags[s].valid = 0;          /* 盘没写进去，缓存不能比盘新 */
        bcache_fail_n++;
        return -1;
    }
    if (pool) {
        copy_bytes(line_bytes(s), src);
        tags[s].dev = dev;
        tags[s].lba = lba;
        tags[s].valid = 1;
    }
    bcache_write_n++;
    return 0;
}

int bcache_sync(void)
{
    return 0;                           /* 写穿：没有待回写的块 */
}

unsigned int bcache_hits(void)
{
    return (unsigned int)bcache_hits_n;
}

unsigned int bcache_misses(void)
{
    return (unsigned int)bcache_misses_n;
}

unsigned int bcache_writes(void)
{
    return (unsigned int)bcache_write_n;
}
