/* ==============================================================
 * ATA PIO：主通道（0x1F0）两个盘，LBA28 读/写一个扇区
 *
 * 读与写的等待方向相反（读是"等数据到"、写是"等盘要"），所以它们是两个函数；
 * 之前只做读，是因为文件系统还是只读的。写路径接上 mosfs 的 create/write 之后
 * 才出现——没有它，"把用户程序的输出存回盘"这件事就无从谈起。
 *
 * 返回值约定：
 *   ata_probe()      位掩码，bit0 = 主盘(master)，bit1 = 从盘(slave)，0 = 都没有
 *   ata_read()       0 = 成功；-1 = 那个盘不在；-2 = 等 BSY/DRQ 超时；-3 = 命令报错
 *   ata_write()      同上，另外 -1 还包括 buf 不是 2 字节对齐
 * ============================================================== */
#ifndef MINI_OS_ATA_H
#define MINI_OS_ATA_H

#define ATA_SECTOR_BYTES 512

/* 探测两个盘，返回存在位掩码；顺带把每个盘的 LBA28 扇区数记下来 */
int ata_probe(void);

/* 存在位掩码（ata_probe 的结果，不必再探测一次） */
int ata_present_mask(void);

/* 扇区数（来自 IDENTIFY 的 word 60/61）；盘不存在返回 0 */
unsigned int ata_sectors(int drive);

/* 读一个扇区到 buf（必须 2 字节对齐、512 字节可写） */
int ata_read(int drive, unsigned int lba, void *buf);

/* 写一个扇区（buf 必须 2 字节对齐、512 字节可读）。PIO 0x30，写完等 BSY 落、查 ERR/DF */
int ata_write(int drive, unsigned int lba, const void *buf);

/* 账：读/写成功几次、失败几次——shell `disk` 与测试都读它 */
unsigned int ata_reads(void);
unsigned int ata_writes(void);
unsigned int ata_errors(void);

/* 内核自带的扇区缓冲（shell `disk` 用它，测试也能从符号直接读出来比对） */
unsigned char *ata_buffer(void);

#endif /* MINI_OS_ATA_H */
