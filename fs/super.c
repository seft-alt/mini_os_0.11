/* fs/super.c —— 挂载（超级块 + 根目录进缓存）与全盘计数
 *
 * 对应 Linux 0.11 的 fs/super.c：只回答"这块盘是不是我们的格式、根在哪、
 * 数据区从哪到哪"，不碰目录语义。挂载成功之后 sb.ok=1，其余四个文件才敢动。
 * ====================================================== */
#include "linux/fsint.h"
#include "linux/blkdev.h"
#include "linux/part.h"
#include "linux/bcache.h"

struct fs_super sb;
unsigned int fs_dev;                /* 当前挂载的设备号（整盘或某个分区） */
unsigned int n_errors;
unsigned int n_writes, n_creates, n_unlinks;

/* 根目录缓存：mount 时一次装进 8 项（名字/号/大小），之后 ls/lookup 全是内存操作。
 * 为什么必须缓存：目录块（LBA 5）与 inode 表块（LBA 2）交替读，一块的缓存会来回冲刷，
 * 一次 `ls` 就能把读块数从 1 抬到十几——实测 rd=31 就是这么来的。
 * 目录项本身不带 size，size 得回 inode 取，所以"装缓存"这一步是会读 inode 的。 */
struct fs_dent dent[FS_V1_MAX_FILES];
unsigned int ndent;

/* 挑挂哪个设备：候选顺序是「分区设备（槽 PART_SLOT_FIRST 起）→ 整盘
 * 设备（槽 0/1）」，第一块在设备内 LBA 0 处带着 MFS1 的赢。这样有分区表
 * 就按分区挂（文件系统看不见分区起点，换算全在 blkdev 的 base 里），
 * 没有分区表的老盘照旧整盘挂，两种盘一个入口。
 * 命中时把块留在工作缓冲 blk 里，fs_mount 不再重读。 */
static int fs_probe(void)
{
    unsigned int order[BLK_MAX_DEV];
    unsigned int n = 0, i, dv;

    for (dv = PART_SLOT_FIRST; dv < BLK_MAX_DEV; dv++)
        if (blkdev_sectors(dv) != 0)
            order[n++] = dv;
    for (dv = 0; dv < PART_SLOT_FIRST; dv++)
        if (blkdev_sectors(dv) != 0)
            order[n++] = dv;

    for (i = 0; i < n; i++) {
        fs_dev = order[i];
        if (blk_read(FS_BLOCK_LBA) != 0)    /* 超级块：设备内块 1 */
            continue;
        if (blk.b[0] == 'M' && blk.b[1] == 'F' &&
            blk.b[2] == 'S' && blk.b[3] == '1')
            return (int)order[i];
    }
    return -1;
}

int fs_mount(void)
{
    sb.ok = 0;
    blkdev_init();                      /* 幂等：两个 ata 盘登记成 dev 0/1 */
    bcache_init();                      /* 缓存与账从 0 起 */
    part_init();                        /* 有 MBR 就把主分区登记成 dev 2/3 */
    n_writes = 0;
    n_creates = 0;
    n_unlinks = 0;
    n_errors = 0;

    fs_dev = 0;
    if (fs_probe() < 0)                 /* 所有候选都没有 MFS1 超级块 */
        return -1;

    sb.block_size  = u32_at(4);
    sb.total       = u32_at(8);
    sb.inodes      = u32_at(12);
    sb.root        = u32_at(16);
    sb.inode_start = u32_at(20);
    sb.inode_per_blk = u32_at(24);
    sb.bitmap_start = u32_at(28);
    sb.data_start  = u32_at(32);
    sb.data_blocks = u32_at(36);

    if (sb.block_size != FS_BLOCK)
        return -4;
    if (sb.inode_per_blk == 0 || sb.inode_per_blk > FS_BLOCK / 32)
        return -5;
    if (sb.total == 0 || sb.total > blkdev_sectors(fs_dev))
        return -6;
    if (sb.data_start + sb.data_blocks > sb.total)
        return -7;
    if (sb.inodes > FS_V1_MAX_INO)
        return -8;
    /* 位图必须真的落在数据区之前，且不是 0：写路径从它要块 */
    if (sb.bitmap_start == 0 || sb.bitmap_start >= sb.data_start)
        return -10;

    sb.ok = 1;
    ndent = 0;
    if (root_load() != 0)                      /* 根目录进缓存，之后 ls/cat 不再读目录块 */
        return -9;
    return 0;
}

unsigned int fs_blocks(void) { return sb.total; }
unsigned int fs_inodes(void) { return sb.inodes; }
/* 读块数改成「真落盘的块数」（bcache 未命中），命中数另给一行：
 * 这两个数一起才说明缓存起了作用——rd 小、hit 大。 */
unsigned int fs_reads(void)  { return bcache_misses(); }
unsigned int fs_hits(void)   { return bcache_hits(); }
unsigned int fs_errors(void) { return n_errors; }
unsigned int fs_writes(void) { return n_writes; }
unsigned int fs_creates(void) { return n_creates; }
unsigned int fs_unlinks(void) { return n_unlinks; }
