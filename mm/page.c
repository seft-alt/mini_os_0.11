/* ==============================================================
 * 物理页分配器（位图）
 *
 * 管理 0x20000–0x90000 的低端空闲内存（内核映像之上、BIOS 参数区之前），
 * 用一张位图记录占用情况，支持单页/连续两页分配、释放与用量统计。
 * 位图本身在内核 .bss 里，启动时由 page_init() 清零（.bss 不会自动清零）。
 *
 * 与内核映像的边界是这条规则的核心：`__kernel_end` 必须落在 PAGES_BASE 之下，
 * 三层守护（tools/kernel-fit.py 编译期、page_init() 启动期、link.ld 导出符号）。
 * 曾经真的炸过：映像长到 0x10780 之后，分配器分配首页先 zero_page(0x10000)，
 * 正好把落在 0x10010 的 vga `cursor` 抹成 0——屏幕从第 0 行开始覆写。
 *
 * 本文件只管“哪一页是空的”；页表怎么写归 mm/paging.c。
 * ============================================================== */
#include "linux/page.h"
#include "linux/pageint.h"
#include "linux/printk.h"
#include "linux/stat.h"

/* arch/link.ld 在 .bss 之后给出的边界；page_init() 用它证明“映像没长进分配区” */
extern char __kernel_end[];
extern void kernel_halt(void);

/* 分配区起点：内核映像尾部之上（见 page_init() 的检查）。
 * 一路从 0x20000 抬到这里：先是块设备/分区/块缓存，再是多级目录那 330 行，
 * 再是 file_dev / char_dev / block_dev 这一层抽象（fops 表 + 设备读中转缓冲），
 * 再是管道那 8 个环（`pipes[]` 一对 536 字节，8 对占 0x10C0），
 * 每次都是"尾巴离下一条红线只剩几百字节"。每抬一页代价是可用页少 1。
 * 现在 110 → 107：掉 3 帧换 0x1C60 的余量，往后加模块不必再逐页抠。 */
#define PAGES_BASE      0x00025000
#define PAGES_END       0x00090000    /* BIOS 参数区 0x90000 之前 */
#define PAGE_COUNT      ((PAGES_END - PAGES_BASE) / PAGE_SIZE)
#define BITMAP_WORDS    ((PAGE_COUNT + 31) / 32)


static unsigned int bitmap[BITMAP_WORDS];
static unsigned int used_frames;

/* 扫描起点：`next_free` 之前的页**全都已占用**（这是它能当下标用的唯一理由）。
 * 不变式靠两处维持：分配成功时把起点推到这一页之后；释放时把起点拉回更小的那页。
 * 于是“连续分配 k 页”总共只扫 k+1 步（摊还 O(1)），而不是每次都从 0 重扫。 */
static unsigned int next_free;

static void zero_page(unsigned int phys)
{
    unsigned int *p = (unsigned int *)phys;
    unsigned int i;

    for (i = 0; i < PAGE_SIZE / 4; i++)
        p[i] = 0;
}

void page_init(void)
{
    unsigned int i;

    /* 这条曾经真的炸过：内核映像（含 .bss）长到 0x10780 之后，`cursor` /
     * `line_len` 这些 vga 静态变量正好落在第一页的 0x10010 上，而
     * zero_page(0x10000) 一清零就把它们抹成 0——屏幕于是从第 0 行开始
     * 覆写，banner 被踩掉、整屏错位，看上去像“上滚了 7 行”。
     * 所以分配器和内核之间的边界必须是“内核最后一字节之上”，并且要在
     * 启动时真的验一遍：符号由 arch/link.ld 的 __kernel_end 给出。 */
    if ((unsigned int)(unsigned long)__kernel_end >= PAGES_BASE) {
        printk("[mem] FATAL: kernel ends at 0x%08X, allocator base 0x%08X\n",
               (unsigned int)(unsigned long)__kernel_end, PAGES_BASE);
        kernel_halt();                 /* 映像装不进约定区：宁可停，不许踩 */
    }

    for (i = 0; i < BITMAP_WORDS; i++)
        bitmap[i] = 0;
    used_frames = 0;
    next_free = 0;                     /* 空表：从第一页开始扫 */
}

static int bit_test(unsigned int index)
{
    return (bitmap[index / 32] >> (index % 32)) & 1;
}

static void bit_set(unsigned int index)
{
    bitmap[index / 32] |= (1u << (index % 32));
}

static void bit_clear(unsigned int index)
{
    bitmap[index / 32] &= ~(1u << (index % 32));
}

/* 分配一页：从 `next_free` 开始扫，找不到就绕回 0 再扫一遍。
 * 绕回是必须的——低地址的页可能早就被释放过（guard 页归还、任务栈回收），
 * 只看“起点之后”会漏掉它们。返回的仍是最低可用页之后的第一个空闲页，
 * 与“每次从 0 扫”的结果相同（`verify` 的 24 行就钉着这一点）。 */
unsigned int page_alloc(void)
{
    unsigned int i = next_free;
    unsigned int n;

    for (n = 0; n < PAGE_COUNT; n++) {
        if (!bit_test(i)) {
            bit_set(i);
            used_frames++;
            stat_inc(STAT_ALLOC);
            zero_page(PAGES_BASE + i * PAGE_SIZE);
            next_free = (i + 1 < PAGE_COUNT) ? i + 1 : 0;
            return PAGES_BASE + i * PAGE_SIZE;
        }
        i = (i + 1 < PAGE_COUNT) ? i + 1 : 0;
    }
    return 0;                          /* 分配器耗尽 */
}

/* 连续两页：任务栈要“栈页 + 紧邻下面一页 guard”才有意义，隔开的页给不出保证。
 * 位图里相邻两位同时为空就算找到；从低地址往上扫，和 page_alloc 的取页顺序一致。
 * 这里**不动 `next_free`**：它只负责“哪些页是空的”，而相邻性要把低地址也看一遍。
 * 它把页标成已用，不会破坏“起点之前的页全都已占用”这个不变式。 */
unsigned int page_alloc_pair(void)
{
    unsigned int i;

    for (i = 0; i + 1 < PAGE_COUNT; i++) {
        if (!bit_test(i) && !bit_test(i + 1)) {
            bit_set(i);
            bit_set(i + 1);
            used_frames += 2;
            stat_inc(STAT_ALLOC);
            zero_page(PAGES_BASE + i * PAGE_SIZE);
            zero_page(PAGES_BASE + (i + 1) * PAGE_SIZE);
            return PAGES_BASE + i * PAGE_SIZE;
        }
    }
    return 0;
}

/* 连续 k 页：多页程序（ELF 的 p_memsz 越过一页）在恒等映射下必须是物理连续的一段。
 * 从低地址往上找第一段 k 个连续空位，和 page_alloc_pair 一样**不动 `next_free`**
 * （相邻性要求把低地址也看一遍；把已用页标上也不会破坏“起点之前全占用”的不变式）。 */
unsigned int page_alloc_run(unsigned int k)
{
    unsigned int i, j;

    if (k == 0 || k > PAGE_RUN_MAX)
        return 0;
    for (i = 0; i + k <= PAGE_COUNT; i++) {
        for (j = 0; j < k; j++)
            if (bit_test(i + j))
                break;
        if (j < k)
            continue;                  /* 这段里有一页占着，往下一格接着找 */
        for (j = 0; j < k; j++) {
            bit_set(i + j);
            zero_page(PAGES_BASE + (i + j) * PAGE_SIZE);
        }
        used_frames += k;
        stat_inc(STAT_ALLOC);
        return PAGES_BASE + i * PAGE_SIZE;
    }
    return 0;
}

/* 归还一整段：逐页交给 page_free，校验（范围/对齐/重复释放）都由它做 */
void page_free_run(unsigned int phys, unsigned int k)
{
    unsigned int j;

    for (j = 0; j < k; j++)
        page_free(phys + j * PAGE_SIZE);
}

void page_free(unsigned int phys)
{
    unsigned int index;

    if (phys < PAGES_BASE || phys >= PAGES_END)
        return;
    if ((phys & (PAGE_SIZE - 1)) != 0)
        return;

    index = (phys - PAGES_BASE) / PAGE_SIZE;
    if (index >= PAGE_COUNT || !bit_test(index))
        return;                        /* 不是本分配器给出的页，或重复释放 */

    bit_clear(index);
    used_frames--;
    if (index < next_free)             /* 更低的页空出来了：起点必须拉回去，否则会漏 */
        next_free = index;
}

unsigned int page_frames_total(void) { return PAGE_COUNT; }
unsigned int page_frames_used(void)  { return used_frames; }
unsigned int page_frames_free(void)  { return PAGE_COUNT - used_frames; }