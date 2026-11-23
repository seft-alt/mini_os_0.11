/* ==============================================================
 * drivers/block/part.h —— MBR 主分区扫描
 *
 * 整盘设备（blkdev 槽 0/1）只覆盖整块盘。盘上有分区表时，分区的
 * 块 0 并不是盘上的块 0——文件系统得知道自己从哪儿开始，就得有人
 * 先读 LBA 0、把每条主分区登记成独立设备。
 *
 * 这里只做这一件事：验签名 0xAA55、逐条查 start/count 落在盘内，
 * 从 PART_SLOT_FIRST 起往 blkdev 登记。登记完后槽 2/3 就是「分区
 * 设备」，base 是它在盘上的起点，读写时由 blkdev 自动加偏移，
 * 文件系统仍然只用「设备内块号」。
 *
 * 不认 GPT、不跟扩展分区链：玩具内核的 MBR 只要 4 条主分区。
 * ============================================================== */
#ifndef MINI_OS_PART_H
#define MINI_OS_PART_H

#define PART_MAX         4      /* MBR 分区表固定 4 条 */
#define PART_SLOT_FIRST  2      /* 分区设备的第一个 blkdev 槽 */
#define PART_MBR_SIG_LO  0x55   /* 扇区末尾 0x55 0xAA */
#define PART_MBR_SIG_HI  0xAA

/* 扫所有已登记的整盘设备（槽 0..PART_SLOT_FIRST-1）：认出 MBR 就把
 * 主分区登记到槽 2 起，槽位不够或越界的条目记进 skipped。
 * 幂等：blkdev_register 对同一 (drive, base) 重复登记不叠加。
 * 返回当前登记着的分区数。 */
int part_init(void);

/* 账与表都是全局符号（不 static）：tools/part.sh 用 nm + QMP xp 读。 */
extern unsigned long part_count_n;      /* 当前登记着的分区数 */
extern unsigned long part_skipped_n;    /* 分区表里有、但放不下或越界的条数 */
extern unsigned long part_mbr_n;        /* 认出的 MBR 个数 */
extern unsigned long part_type_tab[PART_MAX];   /* 各槽的分区类型字节 */

#endif /* MINI_OS_PART_H */
