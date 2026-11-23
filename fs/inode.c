/* fs/inode.c —— inode 的读写、分配与回收（对应 Linux 0.11 的 fs/inode.c）。
 *
 * v1 的 inode 固定 32 字节：mode(2) / links(2) / size(4) / blocks[5](20)，
 * 全部按字节摆（见 include/fs/mosfs.h 的布局说明）。
 * 回收（free_inode）也放这里：它是 inode 侧的"还块 + 标空"，删文件与删目录共用。
 * ====================================================== */
#include "linux/fsint.h"

/* 读 inode：找块 + 块内偏移，然后按固定字段切出来 */
int inode_read(unsigned int ino, unsigned int *mode, unsigned int *size,
               unsigned int *blocks)
{
    unsigned int lba, off, j;

    if (!sb.ok || ino >= sb.inodes)
        return -1;
    lba = sb.inode_start + ino / sb.inode_per_blk;
    off = (ino % sb.inode_per_blk) * 32;
    if (blk_read(lba) != 0)
        return -1;

    *mode = u16_at(off);
    *size = u32_at(off + 4);
    for (j = 0; j < FS_INODE_BLOCKS; j++)
        blocks[j] = u32_at(off + 8 + j * 4);
    return 0;
}

/* 写回一个 inode（字段按字节摆，与 u16_at/u32_at 同一套约定） */
int inode_write(unsigned int ino, unsigned int mode, unsigned int size,
                const unsigned int *blocks)
{
    unsigned int lba, off, j;

    if (!sb.ok || ino >= sb.inodes || !blocks)
        return -1;
    lba = sb.inode_start + ino / sb.inode_per_blk;
    off = (ino % sb.inode_per_blk) * 32;
    if (blk_read(lba) != 0)
        return -1;

    u16_put(off, mode);
    u16_put(off + 2, 1);                /* links：v1 不做硬链接，恒为 1 */
    u32_put(off + 4, size);
    for (j = 0; j < FS_INODE_BLOCKS; j++)
        u32_put(off + 8 + j * 4, blocks[j]);

    return blk_write(lba);
}

/* 找一个空闲 inode（mode == 0）。0 号与 1 号（根目录）跳过 */
unsigned int inode_alloc(void)
{
    unsigned int zero[FS_INODE_BLOCKS] = { 0, 0, 0, 0, 0 };
    unsigned int ino;

    for (ino = 2; ino < sb.inodes; ino++) {
        unsigned int lba = sb.inode_start + ino / sb.inode_per_blk;
        unsigned int off = (ino % sb.inode_per_blk) * 32;

        if (blk_read(lba) != 0)
            return 0;
        if (u16_at(off) == 0) {
            if (inode_write(ino, 1, 0, zero) != 0)
                return 0;
            return ino;
        }
    }
    return 0;                                   /* inode 用完了 */
}

/* 把一个 inode 标空（mode=0、size=0、blocks 全 0）。 */
int inode_zero(unsigned int ino)
{
    unsigned int zero[FS_INODE_BLOCKS] = { 0, 0, 0, 0, 0 };

    return inode_write(ino, 0, 0, zero);
}

/* 删文件的"还块"半边：直接块与间接块里的号逐个还位图，最后把 inode 标空。
 * 这里是先账后名：位图归零在最前，最坏残局是"目录里还剩一个指向空 inode
 * 的名字"（ls 显示 0 字节、cat 说没有），而不是"名字没了、块永远占着"。
 * 失败码：-2 不是普通文件 / -3 inode 读不出 / -4 位图 / -5 inode 写。 */
int free_inode(unsigned int ino)
{
    unsigned int mode, size, blocks[FS_INODE_BLOCKS], j, k;

    if (inode_read(ino, &mode, &size, blocks) != 0)
        return -3;
    if (mode != 1)
        return -2;
    for (j = 0; j < FS_DIRECT; j++)
        if (blocks[j] && bitmap_clear(blocks[j]) != 0)
            return -4;
    if (blocks[FS_INDIRECT]) {
        unsigned int ib = blocks[FS_INDIRECT];

        for (k = 0; k < FS_IND_PER_BLK; k++) {
            unsigned int b;

            if (blk_read(ib) != 0)              /* bitmap_clear 把缓存换掉了，重读 */
                return -4;
            b = u32_at(k * 4);
            if (b && bitmap_clear(b) != 0)
                return -4;
        }
        if (bitmap_clear(ib) != 0)              /* 间接块自己也要还 */
            return -4;
    }
    return inode_zero(ino) == 0 ? 0 : -5;
}
