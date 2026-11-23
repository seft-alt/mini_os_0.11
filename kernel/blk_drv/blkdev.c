/* drivers/blkdev.c —— 块设备注册表（契约见 include/drivers/block/blkdev.h）
 *
 * 只有一张 4 格的静态表：槽号就是设备号，表和槽位同寿，不需要
 * 生存期管理。base 字段现在全是 0（整盘就是一个设备），但它是
 * 分区层唯一需要的东西——加分区不改这里，只多登记两格。
 * ============================================================== */
#include "linux/blkdev.h"
#include "linux/ata.h"

static struct {
    unsigned int used;
    int drive;
    unsigned int base;
    unsigned int sectors;
    const char *name;
} devs[BLK_MAX_DEV];

unsigned long blkdev_reads_n;
unsigned long blkdev_writes_n;
unsigned long blkdev_errors_n;
unsigned long blkdev_bound_denied_n;
unsigned long blkdev_count_n;
unsigned long blkdev_sectors_tab[BLK_MAX_DEV];

static int dev_ok(unsigned int dev)
{
    return dev < BLK_MAX_DEV && devs[dev].used;
}

int blkdev_register(unsigned int slot, const char *name,
                    int drive, unsigned int base, unsigned int sectors)
{
    if (slot >= BLK_MAX_DEV || !name || drive < 0 || drive > 1)
        return BLK_E_ARG;
    if (sectors == 0)
        return BLK_E_ARG;               /* 0 块的设备 = 没设备 */
    if (devs[slot].used) {
        if (devs[slot].drive == drive && devs[slot].base == base)
            return (int)slot;           /* 同一块盘同一段：幂等 */
        return BLK_E_ARG;
    }
    devs[slot].used = 1;
    devs[slot].drive = drive;
    devs[slot].base = base;
    devs[slot].sectors = sectors;
    devs[slot].name = name;
    blkdev_sectors_tab[slot] = sectors;
    blkdev_count_n++;
    return (int)slot;
}

int blkdev_init(void)
{
    int mask = ata_present_mask();

    /* 槽号 = 盘号，两边一一对应；盘不在就空着，不把从盘挤到 0 号，
     * 否则「dev 0 到底是主盘还是唯一那块盘」会随机器变。 */
    if (mask & 1)
        blkdev_register(0, "ata0", 0, 0, ata_sectors(0));
    if (mask & 2)
        blkdev_register(1, "ata1", 1, 0, ata_sectors(1));
    return (int)blkdev_count_n;
}

int blkdev_read(unsigned int dev, unsigned int lba, void *buf)
{
    if (!dev_ok(dev) || !buf)
        return BLK_E_ARG;
    if (lba >= devs[dev].sectors) {
        blkdev_bound_denied_n++;
        return BLK_E_ARG;
    }
    if (ata_read(devs[dev].drive, devs[dev].base + lba,
                 buf) != 0) {
        blkdev_errors_n++;
        return BLK_E_IO;
    }
    blkdev_reads_n++;
    return 0;
}

int blkdev_write(unsigned int dev, unsigned int lba, const void *buf)
{
    if (!dev_ok(dev) || !buf)
        return BLK_E_ARG;
    if (lba >= devs[dev].sectors) {
        blkdev_bound_denied_n++;
        return BLK_E_ARG;
    }
    if (ata_write(devs[dev].drive, devs[dev].base + lba,
                  buf) != 0) {
        blkdev_errors_n++;
        return BLK_E_IO;
    }
    blkdev_writes_n++;
    return 0;
}

unsigned int blkdev_count(void)
{
    return (unsigned int)blkdev_count_n;
}

unsigned int blkdev_sectors(unsigned int dev)
{
    return dev_ok(dev) ? devs[dev].sectors : 0;
}

const char *blkdev_name(unsigned int dev)
{
    return dev_ok(dev) ? devs[dev].name : 0;
}

int blkdev_drive(unsigned int dev)
{
    return dev_ok(dev) ? devs[dev].drive : -1;
}
