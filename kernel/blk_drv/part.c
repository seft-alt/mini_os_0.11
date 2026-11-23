/* drivers/part.c —— MBR 主分区扫描（契约见 include/drivers/block/part.h）
 *
 * MBR 只有 512 字节，读一次就够，所以在文件里放一个静态缓冲：
 * 正好一扇区，也不会把内核栈吃掉 512 字节。
 * ============================================================== */
#include "linux/part.h"
#include "linux/blkdev.h"

unsigned long part_count_n;
unsigned long part_skipped_n;
unsigned long part_mbr_n;
unsigned long part_type_tab[PART_MAX];

/* 分区表项从 446 起，每条 16 字节：标志/CHS/类型/CHS/起始 LBA/扇区数 */
#define MBR_ENT_OFF 446
#define MBR_ENT_LEN 16
#define MBR_SIG_OFF 510

static unsigned char mbr[BLK_SECTOR];

/* 槽名要长住：blkdev_register 只存指针，不复制字符串 */
static const char *const slot_names[BLK_MAX_DEV] = {
    "ata0", "ata1", "part2", "part3"
};

static unsigned int rd32(const unsigned char *p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
           ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

int part_init(void)
{
    unsigned int slot = PART_SLOT_FIRST;
    unsigned int dv;

    for (dv = 0; dv < PART_SLOT_FIRST && slot < BLK_MAX_DEV; dv++) {
        unsigned int sec = blkdev_sectors(dv);
        unsigned int p;

        if (sec == 0)                       /* 这块盘没登记，跳过 */
            continue;
        if (blkdev_read(dv, 0, mbr) != 0)   /* LBA 0 读不出来就当没分区表 */
            continue;
        if (mbr[MBR_SIG_OFF] != PART_MBR_SIG_LO ||
            mbr[MBR_SIG_OFF + 1] != PART_MBR_SIG_HI)
            continue;                       /* 不是 MBR（含空盘） */
        part_mbr_n++;

        for (p = 0; p < PART_MAX; p++) {
            const unsigned char *e = &mbr[MBR_ENT_OFF + p * MBR_ENT_LEN];
            unsigned int start = rd32(e + 8);
            unsigned int cnt   = rd32(e + 12);

            if (cnt == 0)                   /* 空条目 */
                continue;
            if (start == 0 || start + cnt > sec) {
                part_skipped_n++;           /* 起点为 0 或越过盘末 */
                continue;
            }
            if (slot >= BLK_MAX_DEV) {      /* 槽用完了，后面的登记不了 */
                part_skipped_n++;
                continue;
            }
            if (blkdev_register(slot, slot_names[slot],
                                blkdev_drive(dv), start, cnt)
                != (int)slot) {
                part_skipped_n++;
                continue;
            }
            part_type_tab[slot] = e[4];
            slot++;
            part_count_n++;
        }
    }
    return (int)part_count_n;
}
