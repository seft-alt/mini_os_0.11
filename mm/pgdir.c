/* mm/pgdir.c —— 每任务页目录（契约见 include/mm/pgdir.h）
 *
 * 一张页目录 4KB、一张私有 PT0 4KB，都是 4KB 对齐的物理页，靠恒等映射当
 * 普通内存直接写。不做"页目录对象"的理由和 fd 表一样：槽位是编译期常量，
 * 静态池就够，不需要给"页目录本身"加生存期。
 */
#include "linux/pgdir.h"
#include "linux/page.h"
#include "linux/tss.h"

/* gcc 13+ 会把 “(unsigned int *)常量” 当成大小为 0 的对象，对 `kern[i]`
 * 报 -Warray-bounds；本项目 -Werror，于是直接编译失败（gcc 16.2 实测）。
 * 本文件所有指针都指向页表物理页/恒等映射内存，没有可被推断大小的真实数组，
 * 所以只在这里关掉这一个诊断，而不是全局 -Wno-array-bounds。 */
#if defined(__GNUC__) && (__GNUC__ >= 13)
#pragma GCC diagnostic ignored "-Warray-bounds"
#endif

#define PT_ENTRIES  1024
#define PTE_P       0x001U      /* 存在 */
#define PTE_RW      0x002U      /* 可写 */
#define PTE_US      0x004U      /* 用户态可访问（两级都要有才放行） */

/* PDE 0..3 覆盖 0..16MB（head 建了 4 个页表）；私有 PT0 只管 0..4MB */
#define PT0_SPAN    0x00400000U

/* 这两张表**不是 static**：boot/intr.asm 的 user_enter / user_return_stub 要
 * 直接按下标读它们（进 ring3 时装 CR3、回内核时把账清成 0）。
 * 让汇编调 C 函数去取一个整数，代价是要照顾 cdecl 的寄存器约定；
 * 直接读数组省掉这一层，也省掉"取数函数自己会不会被判非法"的疑问。 */
unsigned int pgd_tab[TASK_SLOTS];           /* 页目录物理地址（0 = 用内核的） */
unsigned int pgt_tab[TASK_SLOTS];           /* 私有 PT0 物理地址 */
unsigned int cr3_tab[TASK_SLOTS];           /* 记账：这一格"现在装着"的 CR3 */

static unsigned int n_switch;
static unsigned int n_maps;
static unsigned int n_fails;

static unsigned int pgd_slot_ok(int slot)
{
    return slot >= 0 && slot < TASK_SLOTS;
}

unsigned int pgdir_current(void)
{
    unsigned int cr3;

    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}

static void pgdir_load(unsigned int cr3)
{
    __asm__ volatile ("mov %0, %%cr3" : : "r"(cr3) : "memory");
    /* TSS.cr3 是"硬件任务切换时会装的表"。没有 task gate 时没人读它，但让它
     * 与 CPU 实际在用的表保持一致，是"这一格没有漂移"的唯一保证。放在这里
     * 而不是各调用点：换表这件事只从这一个出口发生。 */
    tss_set_cr3(cr3);
}

/* 把一页当"页目录"用：清零、PDE0 → 私有 PT0（带 U/S，PTE 才有说话的余地）、
 * PDE1..3 → 抄内核的（共享那 12MB 恒等映射）、其余一律 0（不存在）。
 *
 * 为什么 PDE4.. 是 0 而不是照抄内核：这些位置在内核页目录里是"按需分页窗口"
 * （0x02000000）之类只在 ring0 用到的映射。把它们留给用户任务，等于给了
 * 用户一条"访问 32MB 就会被内核补页"的路（补出来的页是谁的权限，取决于
 * 补页代码，不取决于隔离）。干脆不存在——用户碰它必然 #PF，而 #PF 处理体
 * 已经把"用户态发起的缺页"统一当成越权。
 */
static void pgdir_fill(unsigned int pgd, unsigned int pgt)
{
    unsigned int *dir = (unsigned int *)pgd;
    unsigned int *pt = (unsigned int *)pgt;
    unsigned int *kern = (unsigned int *)PGDIR_KERNEL;
    unsigned int i;

    for (i = 0; i < PT_ENTRIES; i++)
        pt[i] = ((i << 12) & (PT0_SPAN - 1)) | PTE_P | PTE_RW;

    for (i = 0; i < PT_ENTRIES; i++)
        dir[i] = 0;
    dir[0] = pgt | PTE_P | PTE_RW | PTE_US;   /* 私有 PT0：必须带 U/S */
    for (i = 1; i < 4; i++)
        dir[i] = kern[i];                     /* 4..16MB：共享内核的页表 */
}

int pgdir_init(void)
{
    int slot, bad = 0;

    pgd_tab[0] = 0;
    pgt_tab[0] = 0;
    cr3_tab[0] = 0;

    for (slot = 1; slot < TASK_SLOTS; slot++) {
        unsigned int pgd = page_alloc();
        unsigned int pgt = page_alloc();

        if (!pgd || !pgt) {                   /* 分配器不够：这一格就是没有 */
            if (pgd)
                page_free(pgd);
            if (pgt)
                page_free(pgt);
            pgd_tab[slot] = 0;
            pgt_tab[slot] = 0;
            cr3_tab[slot] = 0;
            bad++;
            n_fails++;
            continue;
        }
        pgdir_fill(pgd, pgt);
        pgd_tab[slot] = pgd;
        pgt_tab[slot] = pgt;
        cr3_tab[slot] = 0;                     /* 还没进过 ring3：账上记"内核" */
    }
    return bad ? -bad : 0;
}

unsigned int pgdir_of(int slot)
{
    if (!pgd_slot_ok(slot))
        return 0;
    return pgd_tab[slot];
}

unsigned int pgdir_activate(int slot)
{
    unsigned int cr3;

    if (!pgd_slot_ok(slot) || !pgd_tab[slot]) {
        pgdir_load(PGDIR_KERNEL);
        return PGDIR_KERNEL;
    }
    cr3 = pgd_tab[slot];
    cr3_tab[slot] = cr3;
    pgdir_load(cr3);
    return cr3;
}

void pgdir_kernel(void)
{
    int slot = task_self();

    /* 账也要清：这一格"现在装着内核页目录"，下次切回来才是对的。
     * 只改 CR3 不改账，会让 cr3_tab 一直记着那张已经用过的任务表。 */
    if (slot >= 0 && slot < TASK_SLOTS)
        cr3_tab[slot] = PGDIR_KERNEL;
    if (pgdir_current() != PGDIR_KERNEL)
        pgdir_load(PGDIR_KERNEL);
}

/* 切任务时"存取 CR3"。
 *
 * 存：把**现在真正生效的那张表**记到切出者名下。这是唯一诚实的来源——
 *     读 CR3 而不是查 cr3_tab，因为 cr3_tab 是"我们以为的"，而 CR3 是
 *     "CPU 正在用的"；两者不一致时，按前者记账会把错误固化下来。
 * 取：进入者名下记着什么就装什么（0 = 内核）。
 *
 * 换表之后，紧接着执行的这段内核代码必须在新表里也能跑：私有页目录的
 * PDE0/PT0 是 0..4MB 的恒等映射、PDE1..3 与内核共享，而任务栈/页表/显存
 * 都在这一段里——这就是"换表本身不会打断自己"的全部前提。 */
void pgdir_switch(int from, int to)
{
    unsigned int cr3 = pgdir_current();
    unsigned int next;

    if (pgd_slot_ok(from))
        cr3_tab[from] = cr3;

    if (!pgd_slot_ok(to))
        next = PGDIR_KERNEL;
    else
        next = cr3_tab[to];

    if (next != cr3)
        pgdir_load(next);
    n_switch++;
}

/* 在**指定槽位**的私有 PT0 里放行/收回一格。
 * 不用切 CR3：私有 PT0 的物理地址就在表里，恒等映射下可以直接写。
 * 这样调用点（exec 的装配段、任务死后的清理）不必关心"现在装着谁"。
 *
 * writable = 0 时"放行但不给写"（PTE 的 RW 位清掉，US 保留）：这就是用户页的
 * 写保护，也是 W^X 的第一步——代码页只读，数据/栈可写。判据只有一条：
 * ring3 往这页写会得到 #PF(err=P|W|U)，而 exception.c 现在把"用户态发起的
 * 缺页"一律当越权，于是写代码页 = 任务被杀，不需要额外分支。 */
int pgdir_set_user_page(int slot, unsigned int phys, int on, int writable)
{
    unsigned int *pt;
    unsigned int pti, v;

    if (!pgd_slot_ok(slot) || (phys & 0xFFFU) || phys >= PT0_SPAN) {
        n_fails++;
        return 0;
    }
    if (!pgt_tab[slot]) {
        /* slot 0 没有私有表：退回内核页目录的 PTE（老路径，仍是同一个开关）。
         * 这条路做不到"只读放行"（page_mark_user 只动 U/S 位），所以 writable=0
         * 在这里按"没这个能力"拒绝，而不是悄悄按可写放行——静默降级比失败更难查。
         * 实际上 slot 0 是 main，不会跑 exec 的用户页。 */
        unsigned int r;

        if (slot != 0 || !writable) {
            n_fails++;
            return 0;
        }
        r = on ? page_mark_user(phys) : page_mark_kernel(phys);
        if (!r) {
            n_fails++;
            return 0;
        }
        n_maps++;
        return 1;
    }

    pt = (unsigned int *)pgt_tab[slot];
    pti = (phys >> 12) & (PT_ENTRIES - 1);
    v = pt[pti];
    if (!(v & PTE_P)) {                      /* 没有映射：先有页再说权限 */
        n_fails++;
        return 0;
    }
    if (on) {
        if (writable)
            pt[pti] = (v | PTE_US | PTE_RW);
        else
            pt[pti] = (v | PTE_US) & ~PTE_RW;    /* 只读：US 在、W 不在 */
        n_maps++;
    } else {
        pt[pti] = v & ~PTE_US;               /* 本来就没开也照清，幂等 */
    }
    /* 这张表可能正装着（任务在跑）：清 TLB 那一格，否则"收回"要等到
     * 下一次自然换页才生效，而这一刻用户已经能多访问一会儿。
     * 表没装着时 invlpg 也不是错的——它只是白刷一次。 */
    __asm__ volatile ("invlpg (%0)" : : "r"(phys) : "memory");
    return 1;
}

/* 老入口：默认"放行且可写"，行为与写保护上线之前完全一样。 */
int pgdir_set_user(int slot, unsigned int phys, int on)
{
    return pgdir_set_user_page(slot, phys, on, 1);
}

unsigned int pgdir_clear_user(int slot)
{
    unsigned int *pt;
    unsigned int i, n = 0;

    if (!pgd_slot_ok(slot) || !pgt_tab[slot])
        return 0;
    pt = (unsigned int *)pgt_tab[slot];
    for (i = 0; i < PT_ENTRIES; i++) {
        if (pt[i] & PTE_US) {
            /* 收回 = 回到"内核全开"的恒等映射格：US 清掉、RW 恢复。
             * 不能只清 US：写保护页的 RW 也是清的，留着这个"只读的内核格"，
             * 将来一旦打开 CR0.WP 就是一颗雷——而且语义上这一页已经还给池了，
             * 池里的页不该带着上一任主人的权限。 */
            pt[i] = (pt[i] & ~PTE_US) | PTE_RW;
            n++;
        }
    }
    if (n)
        pgdir_load(pgdir_current());         /* 整表换一次 = 刷全 TLB，最省事 */
    return n;
}

unsigned int pgdir_user_pages(int slot)
{
    unsigned int *pt;
    unsigned int i, n = 0;

    if (!pgd_slot_ok(slot) || !pgt_tab[slot])
        return 0;
    pt = (unsigned int *)pgt_tab[slot];
    for (i = 0; i < PT_ENTRIES; i++)
        if (pt[i] & PTE_US)
            n++;
    return n;
}

unsigned int pgdir_switches(void)   { return n_switch; }
unsigned int pgdir_cr3_of(int slot) { return pgd_slot_ok(slot) ? cr3_tab[slot] : 0; }
unsigned int pgdir_maps(void)       { return n_maps; }
unsigned int pgdir_fails(void)      { return n_fails; }
