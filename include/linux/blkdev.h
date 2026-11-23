/* ==============================================================
 * drivers/block/blkdev.h —— 块设备注册表
 *
 * 在这层之前，文件系统对着 ata_read(drive, lba, buf) 直接写：
 * 一块盘 = 一整卷，加一块盘或切一个分区都要改 fs。
 *
 * 这里只认「设备号 + 设备内相对块号」：
 *   dev 0/1 = ata 主/从盘（base = 0）
 *   将来分区注册成 dev 2/3，base 就是分区起点，fs 不用知道盘上
 *   有几个分区。
 *
 * 分工：ata 只管「把一个扇区从端口搬进内存」；blkdev 管「现在有
 * 哪几个可寻址的设备、这个号越不越界、账怎么记」。
 * ============================================================== */
#ifndef MINI_OS_BLKDEV_H
#define MINI_OS_BLKDEV_H

#define BLK_MAX_DEV 4
#define BLK_SECTOR  512

#define BLK_E_ARG   (-1)    /* 设备不存在 / lba 越过该设备上限 */
#define BLK_E_IO    (-2)    /* 底下的 ata 报错 */

/* 把两个 ata 盘登记成 dev 0/1；幂等，重复调用不会再登记。
 * 返回登记成功的设备数（坏盘不登记：不会有「存在但读不了」的格）。 */
int blkdev_init(void);

/* 往固定槽位登记一个设备。slot 取 0..BLK_MAX_DEV-1，
 * drive 是 ata 物理盘号，base 是块 0 落在盘上的绝对 LBA，
 * sectors 是这块设备能寻址的块数。返回 slot，<0 失败。 */
int blkdev_register(unsigned int slot, const char *name,
                    int drive, unsigned int base, unsigned int sectors);

/* 读/写设备内第 lba 块：0 = 成功，<0 = BLK_E_*。
 * 越界在解引用之前就拒（不把一次坏地址交给 ata 去等超时）。 */
int blkdev_read(unsigned int dev, unsigned int lba, void *buf);
int blkdev_write(unsigned int dev, unsigned int lba, const void *buf);

unsigned int blkdev_count(void);
unsigned int blkdev_sectors(unsigned int dev);   /* 越界返回 0 */
const char *blkdev_name(unsigned int dev);       /* 越界返回 0 */
int blkdev_drive(unsigned int dev);              /* 底层 ata 盘号；越界 -1 */

/* 账与设备表都是**全局符号**（不 static）：tools/blkdev.sh 用
 * nm + QMP xp 直接读，不需要为此开 shell 命令。 */
extern unsigned long blkdev_reads_n;
extern unsigned long blkdev_writes_n;
extern unsigned long blkdev_errors_n;
extern unsigned long blkdev_bound_denied_n;
extern unsigned long blkdev_count_n;
extern unsigned long blkdev_sectors_tab[BLK_MAX_DEV];

#endif /* MINI_OS_BLKDEV_H */
