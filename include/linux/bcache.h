/* ==============================================================
 * fs/bcache.h —— 8 槽块缓存（直接映射，写穿）
 *
 * 为什么先做它：mosfs 只有一个 union 缓冲 + 一个 cache_lba，
 * 「刚读过 LBA 5，接着读 LBA 2，再回头读 LBA 5」就要重读两次盘。
 * 有了 8 槽之后，挂载 + ls + cat 三个文件的读块数会明显下降，
 * 而「读同一块第二次不落盘」是可以精确计数的判据。
 *
 * 为什么直接映射而不是 LRU：槽 = (lba + dev*7) & 7，命中/替换
 * 都能在纸上算出来，测试里不必猜「刚才那次访问把谁挤掉了」。
 * 代价是冲突率高，对现在这点工作集（挂载后 6 到 8 块）够用。
 *
 * 为什么写穿：mosfs 的写顺序是「数据块 → inode → 位图」，本来
 * 就不保证掉电一致；再把写延到某个 sync 点，只会多出一个
 * 「以为写了其实没写」的窗口。写穿 + 写完更新缓存行，语义最简单：
 * **盘上有什么，缓存里就是什么**。
 * ============================================================== */
#ifndef MINI_OS_BCACHE_H
#define MINI_OS_BCACHE_H

#define BCACHE_SLOTS    8
#define BCACHE_SECTOR   512

/* 清空缓存与全部计数（fs_mount 开头调一次，让每次挂载的账从 0 起） */
void bcache_init(void);

/* 读一块：命中就从槽里拷，未命中走 blkdev_read。0 = 成功，<0 = 失败 */
int bcache_read(unsigned int dev, unsigned int lba, void *buf);

/* 写一块：先写穿到设备，成功后再把内容装进槽（随后同块读必然命中）。
 * 写失败槽标无效，避免留一个「比盘新」的副本。0 = 成功，<0 = 失败 */
int bcache_write(unsigned int dev, unsigned int lba, const void *buf);

/* 写穿模式下没有脏块，恒返回 0；接口留着，换回写策略时不用改调用方 */
int bcache_sync(void);

unsigned int bcache_hits(void);
unsigned int bcache_misses(void);
unsigned int bcache_writes(void);

/* 全局符号（不 static）：脚本用 nm + QMP xp 直接读 */
extern unsigned long bcache_hits_n;
extern unsigned long bcache_misses_n;
extern unsigned long bcache_write_n;
extern unsigned long bcache_fail_n;

#endif /* MINI_OS_BCACHE_H */
