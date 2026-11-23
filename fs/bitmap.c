/* fs/bitmap.c —— 数据块位图（对应 Linux 0.11 的 fs/bitmap.c）。
 *
 * 约定：bit i = 块号 i 被占用，i 从 data_start 扫到 data_start + data_blocks。
 * 一块位图 512 字节 = 4096 位，够 2MB 的盘（我们只有 1MB，用不满）。
 * 这里只记"账"（位图那一个块），块的读写仍走 blkio 的工作缓冲。
 * ====================================================== */
#include "linux/fsint.h"

/* 从位图里要一个数据块：找到第一个 0 位，置 1、写回、返回块号；0 = 盘满 */
unsigned int bitmap_alloc(void)
{
    unsigned int i;

    if (blk_read(sb.bitmap_start) != 0)
        return 0;
    for (i = sb.data_start; i < sb.data_start + sb.data_blocks; i++) {
        unsigned int byte = i / 8;
        unsigned int bit = i % 8;

        if (byte >= FS_BLOCK)
            break;                              /* 位图自己的范围到头了 */
        if (!(blk.b[byte] & (unsigned char)(1u << bit))) {
            blk.b[byte] |= (unsigned char)(1u << bit);
            if (blk_write(sb.bitmap_start) != 0)
                return 0;
            return i;
        }
    }
    return 0;                                   /* 盘满 */
}

/* 把块号 b 在位图里清位（一次读、一次写）。b == 0 时什么都不做。 */
int bitmap_clear(unsigned int b)
{
    if (b == 0)
        return 0;
    if (blk_read(sb.bitmap_start) != 0)
        return -1;
    blk.b[b / 8] &= (unsigned char)~(1u << (b % 8));
    return blk_write(sb.bitmap_start) ? -1 : 0;
}
