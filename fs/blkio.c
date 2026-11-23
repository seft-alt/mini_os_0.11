/* fs/blkio.c —— mosfs 的"块工作缓冲"：一次装一块盘，按字节摆字段。
 *
 * 从 fs/mosfs.c 拆出来的第一块。之所以能拆：这里全是"怎么把一块盘的字节
 * 读进 blk、怎么从 blk 里取/写 u16/u32、第 bi 个逻辑块落在哪个物理块"，
 * 与目录语义、路径规则没有关系。
 *
 * 一个工作缓冲是刻意的：`ls` 要遍历目录、`cat` 要顺序读，两个都反复读相邻
 * 扇区。缓存的复用交给 bcache（键是 LBA 而不是 inode）。
 * ====================================================== */
#include "linux/fsint.h"
#include "linux/bcache.h"

union fs_block blk;

/* 从原始字节里取 u32/u16：不走 struct，避开对齐与打包的坑 */
unsigned int u32_at(unsigned int off)
{
    return (unsigned int)blk.b[off]
         | ((unsigned int)blk.b[off + 1] << 8)
         | ((unsigned int)blk.b[off + 2] << 16)
         | ((unsigned int)blk.b[off + 3] << 24);
}

unsigned int u16_at(unsigned int off)
{
    return (unsigned int)blk.b[off] | ((unsigned int)blk.b[off + 1] << 8);
}

/* 写回 u16/u32：同样按字节摆，跟 u32_at/u16_at 是同一套约定 */
void u16_put(unsigned int off, unsigned int v)
{
    blk.b[off] = (unsigned char)(v & 0xFF);
    blk.b[off + 1] = (unsigned char)((v >> 8) & 0xFF);
}

void u32_put(unsigned int off, unsigned int v)
{
    blk.b[off] = (unsigned char)(v & 0xFF);
    blk.b[off + 1] = (unsigned char)((v >> 8) & 0xFF);
    blk.b[off + 2] = (unsigned char)((v >> 16) & 0xFF);
    blk.b[off + 3] = (unsigned char)((v >> 24) & 0xFF);
}

/* 读一个块进工作缓冲 blk：真正的盘访问交给 blkdev + bcache。
 * 返回 0 成功；未命中/失败由 bcache 计账，这里只把错误记进 n_errors。 */
int blk_read(unsigned int lba)
{
    if (bcache_read(fs_dev, lba, blk.b) != 0) {
        n_errors++;
        return -1;
    }
    return 0;
}

/* 把工作缓冲里的这一块写下去（写穿）。写成功后 bcache 会把同一块
 * 装进槽，所以紧接着的 ls/cat 读同一块必然命中——不需要作废缓存。 */
int blk_write(unsigned int lba)
{
    if (!sb.ok) {
        n_errors++;
        return -1;
    }
    if (bcache_write(fs_dev, lba, blk.b) != 0) {
        n_errors++;
        return -1;
    }
    n_writes++;
    return 0;
}

/* 第 bi 个数据块的块号。bi < 4 查 inode，否则查间接块的第 bi-4 项。
 * 返回 0 = 有这么一块（*out 是块号），-1 = 空洞或块号表坏了。
 * 注意：查间接块会把 blk 缓存换成间接块——调用方读完 *out 要立刻用，
 * 不能在这中间再夹一次别的 blk_read。 */
int blk_lookup(const unsigned int *blocks, unsigned int bi, unsigned int *out)
{
    if (bi >= FS_MAX_BLOCKS)
        return -1;
    if (bi < FS_DIRECT) {
        *out = blocks[bi];
        return *out ? 0 : -1;
    }
    if (blocks[FS_INDIRECT] == 0)
        return -1;
    if (blk_read(blocks[FS_INDIRECT]) != 0)
        return -1;
    *out = u32_at((bi - FS_DIRECT) * 4);
    return *out ? 0 : -1;
}

/* 保证第 bi 个数据块存在：没有就向位图要一块，必要时先要一个间接块。
 * 返回 0 = 成功（*out 是块号）、-3 = 越过上限、-4 = 位图没块、-5 = 读写失败。 */
int blk_ensure(unsigned int *blocks, unsigned int bi, unsigned int *out)
{
    unsigned int ib, nb, q;

    if (bi >= FS_MAX_BLOCKS)
        return -3;
    if (bi < FS_DIRECT) {
        if (blocks[bi] == 0) {
            nb = bitmap_alloc();
            if (!nb)
                return -4;
            blocks[bi] = nb;
        }
        *out = blocks[bi];
        return 0;
    }

    if (blocks[FS_INDIRECT] == 0) {
        ib = bitmap_alloc();
        if (!ib)
            return -4;
        blocks[FS_INDIRECT] = ib;
        if (blk_read(ib) != 0)
            return -5;
        for (q = 0; q < FS_BLOCK; q++)
            blk.b[q] = 0;                       /* 新块必须先清 0：里面的垃圾会被当成块号 */
        if (blk_write(ib) != 0)
            return -5;
    }

    ib = blocks[FS_INDIRECT];
    if (blk_read(ib) != 0)
        return -5;
    nb = u32_at((bi - FS_DIRECT) * 4);
    if (nb == 0) {
        unsigned int newb = bitmap_alloc();

        if (!newb)
            return -4;
        if (blk_read(ib) != 0)                  /* bitmap_alloc 把缓存换掉了，重读 */
            return -5;
        u32_put((bi - FS_DIRECT) * 4, newb);
        if (blk_write(ib) != 0)
            return -5;
        nb = newb;
    }
    *out = nb;
    return 0;
}
