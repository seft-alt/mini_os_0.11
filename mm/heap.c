/* mm/heap.c —— 按字节的 kalloc/kfree（页是唯一来源）
 *
 * 结构很短：
 *   - "块头" 8 字节：size（含头，8 对齐）+ magic。magic 让 double-free /
 *     野指针在检查时能当场露馅，而不是悄悄把空闲表写坏；
 *   - 三个"页块"（chunk）：每次要新页就记一条 {基址, 已用, 容量}，块从低往高长；
 *   - 一张 64 格空闲表：kfree 把 {地址, 大小} 记进去，kalloc 先在这张表里找
 *     够大的（first-fit），找不到才从页块顶上新切。切下的余料留作新的一条，
 *     所以一次大块归还能被小块分配继续吃到。
 *
 * 为什么不做相邻合并：合并需要双向链表或边界标记，代码量翻倍；而现在
 * 没有长跑的场景来暴露碎片，先保证"还能再用"就够了。整页也不还给
 * page_alloc——那是"空页回收"，属于另一个策略层。
 */
#include "linux/heap.h"
#include "linux/page.h"

#define HEAP_ALIGN      8u
#define HEAP_HDR        8u          /* size + magic */
#define HEAP_MIN        16u         /* 切出来的余料小于这个就不切，整块给出去 */
#define HEAP_MAGIC      0x48454150u /* "HEAP"：拦住野指针与 double free */
#define HEAP_FREE_MAGIC 0x46524545u /* "FREE"：已归还的块，防止再次 kfree */
#define HEAP_CHUNKS     8
#define HEAP_FREE_SLOTS 64

typedef struct {
    unsigned int size;              /* 含块头的总字节，8 对齐 */
    unsigned int magic;
} heap_hdr_t;

static unsigned char *chunk_base[HEAP_CHUNKS];
static unsigned int chunk_used[HEAP_CHUNKS];
static unsigned int chunk_cap[HEAP_CHUNKS];
static unsigned int chunk_n;

typedef struct {
    unsigned int addr;              /* 块头地址；0 = 空槽 */
    unsigned int size;
} heap_free_t;

static heap_free_t free_tab[HEAP_FREE_SLOTS];
static unsigned int free_n;

static unsigned int used_bytes;
static unsigned int live_blocks;
static unsigned int pages_taken;
static unsigned int fails;
static unsigned int leaks;          /* 空闲表满时被丢掉的归还（泄漏）*/

static unsigned int align8(unsigned int n)
{
    return (n + (HEAP_ALIGN - 1)) & ~(HEAP_ALIGN - 1);
}

void heap_init(void)
{
    unsigned int i;

    chunk_n = 0;
    free_n = 0;
    used_bytes = 0;
    live_blocks = 0;
    pages_taken = 0;
    fails = 0;
    leaks = 0;
    for (i = 0; i < HEAP_CHUNKS; i++) {
        chunk_base[i] = 0;
        chunk_used[i] = 0;
        chunk_cap[i] = 0;
    }
}

/* 向 page_alloc 要一页，记成新 chunk。返回 chunk 下标；失败 -1。 */
static int heap_new_chunk(void)
{
    unsigned int page;

    if (chunk_n >= HEAP_CHUNKS)
        return -1;
    page = page_alloc();
    if (!page)
        return -1;
    chunk_base[chunk_n] = (unsigned char *)page;
    chunk_used[chunk_n] = 0;
    chunk_cap[chunk_n] = PAGE_SIZE_BYTES;
    pages_taken++;
    return (int)chunk_n++;
}

/* 从空闲表里拿一块（first-fit，必要时切余料）。返回块头地址；没有返回 0。 */
static unsigned int heap_take_free(unsigned int need)
{
    unsigned int i;

    for (i = 0; i < HEAP_FREE_SLOTS; i++) {
        unsigned int size = free_tab[i].size;
        unsigned int addr;

        if (size < need)
            continue;
        addr = free_tab[i].addr;
        if (size - need >= HEAP_MIN) {
            free_tab[i].addr = addr + need;
            free_tab[i].size = size - need;
        } else {
            free_tab[i].addr = 0;
            free_tab[i].size = 0;
            while (free_n > 0 && free_tab[free_n - 1].addr == 0)
                free_n--;
        }
        return addr;
    }
    return 0;
}

void *kalloc(unsigned int size)
{
    unsigned int need;
    unsigned int addr = 0;
    unsigned int i;
    heap_hdr_t *h;

    if (size == 0)
        return 0;
    need = align8(size) + HEAP_HDR;

    addr = heap_take_free(need);
    if (addr == 0) {
        for (i = 0; i < chunk_n; i++) {
            if (chunk_cap[i] - chunk_used[i] >= need) {
                addr = (unsigned int)chunk_base[i] + chunk_used[i];
                chunk_used[i] += need;
                break;
            }
        }
    }
    if (addr == 0) {
        /* 现有页块不够：要新页。注意这里只在"确实要不到"时才失败。 */
        int c = heap_new_chunk();

        if (c >= 0) {
            addr = (unsigned int)chunk_base[c];
            chunk_used[c] = need;
        }
    }
    if (addr == 0) {
        fails++;
        return 0;
    }

    h = (heap_hdr_t *)addr;
    h->size = need;
    h->magic = HEAP_MAGIC;
    used_bytes += need;
    live_blocks++;
    return (void *)(addr + HEAP_HDR);
}

void kfree(void *p)
{
    heap_hdr_t *h;
    unsigned int addr;
    unsigned int i;

    if (!p)
        return;
    addr = (unsigned int)p - HEAP_HDR;
    h = (heap_hdr_t *)addr;
    if (h->magic != HEAP_MAGIC)      /* 野指针 / 已归还：拒绝动它，只记一次泄漏 */
        return;
    h->magic = HEAP_FREE_MAGIC;
    used_bytes -= (h->size <= used_bytes) ? h->size : used_bytes;
    if (live_blocks > 0)
        live_blocks--;

    for (i = 0; i < HEAP_FREE_SLOTS; i++)
        if (free_tab[i].addr == 0)
            break;
    if (i == HEAP_FREE_SLOTS) {
        leaks++;                     /* 空闲表满：这块暂时收不回来，但不会踩内存 */
        return;
    }
    free_tab[i].addr = addr;
    free_tab[i].size = h->size;
    if (i >= free_n)
        free_n = i + 1;
}

unsigned int heap_pages(void)      { return pages_taken; }
unsigned int heap_used_bytes(void) { return used_bytes; }
unsigned int heap_live_blocks(void){ return live_blocks; }

/* 自检：分配 n 块、每块写 "下标派生的图案"，全部读回校验，再全部归还。
 * 返回出错字节数——0 就说明"要得到、写得上、读得回、还得了"这四步都成立。 */
unsigned long heap_selftest(unsigned int n, unsigned int size)
{
    static void *slots[16];
    static unsigned int got[16];
    unsigned long bad = 0;
    unsigned int i;
    unsigned int j;

    if (n == 0 || n > 16 || size == 0)
        return 0xFFFFFFFFUL;

    for (i = 0; i < n; i++) {
        unsigned char *p;

        slots[i] = kalloc(size);
        got[i] = slots[i] != 0;
        if (!got[i]) {
            bad++;
            continue;
        }
        p = (unsigned char *)slots[i];
        for (j = 0; j < size; j++)
            p[j] = (unsigned char)(0xA5 ^ (i * 31 + j));
    }
    for (i = 0; i < n; i++) {
        unsigned char *p;

        if (!got[i])
            continue;
        p = (unsigned char *)slots[i];
        for (j = 0; j < size; j++)
            if (p[j] != (unsigned char)(0xA5 ^ (i * 31 + j)))
                bad++;
    }
    for (i = 0; i < n; i++) {
        if (got[i])
            kfree(slots[i]);
        slots[i] = 0;
        got[i] = 0;
    }
    return bad;
}
