/* ==============================================================
 * 页表项的写入者：按需分页、恒等映射的撤/恢复、用户页的 U/S 收放
 *
 * 分配器（mm/page.c）只回答“哪一页是空的”；这里回答“这页怎么出现在地址空间”。
 * 全部动作都落在**当前 CR3 指向的那张页目录**上（`pgdir_now()`），
 * 所以改完页表必须紧跟一条 `invlpg`——TLB 里那格旧翻译不清，改页表等于没改。
 *
 * 为什么不再写死物理 0：每任务页目录上线之后（见 mm/pgdir.h），一个任务在
 * ring3 期间的 CR3 是它自己那张表。此时若还去改物理 0 的表，用户页的 U/S
 * 位会被写到“别人的页表”上——现象是环 3 能跑，但隔离完全没生效，而且
 * 所有证据（PTE 读回）都是对的，因为它读的也是那张别人的表。
 * 读 CR3 得到"CPU 正在用的那张"，是这里唯一诚实的来源。
 *
 * 四段：
 *   1. 按需分页窗口 0x02000000（4MB）：补 PDE → 补 PTE → invlpg → iret 重试；
 *   2. guard 页：把恒等映射里的一格清成 0，访问即 #PF，原 PTE 留给调用方放回；
 *   3. 用户页：PDE 早已 0x07，这里只补 PTE 的 U/S 位（开、关成对）；
 *   4. 收口：把 [lo, hi) 里所有 PTE 的 U/S 位清掉，把“进不去”交给 CPU 管。
 * ============================================================== */
#include "linux/page.h"
#include "linux/pageint.h"


/* 当前生效的页目录（物理地址即虚拟地址：恒等映射）。 */
static unsigned int *pgdir_now(void)
{
    unsigned int cr3;

    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    return (unsigned int *)(cr3 & 0xFFFFF000U);
}

void page_demand_window(unsigned int *base, unsigned int *size)
{
    *base = DEMAND_BASE;
    *size = DEMAND_SIZE;
}

unsigned int page_fault_map(unsigned int addr, unsigned int error)
{
    unsigned int *pgdir = pgdir_now();   /* 当前 CR3：恒等映射下可直接写 */
    unsigned int pdi = addr >> 22;
    unsigned int pti = (addr >> 12) & 0x3FF;
    unsigned int *pt;
    unsigned int frame;

    /* 只接管“窗口内 + 页不存在”的缺页，其余交回上层 panic */
    if (addr < DEMAND_BASE || addr >= DEMAND_BASE + DEMAND_SIZE)
        return 0;                                   /* 窗口外：非法访问 */
    if (error & 1)
        return 0;                                   /* 页已存在：权限问题，补页解决不了 */

    /* 第一步：缺页目录项，先给这一片 4MB 建一张页表
     * 权限用 PTE_KERNEL（0x03）而不是 0x07：这张窗口是内核自己要用的
     * 演示区，没有理由对 ring3 开放。PDE 的 U/S 一开，这一片 4MB 里
     * 每个 PTE 都成了“差一位就放行”的状态，不如一开始就不给。 */
    if (!(pgdir[pdi] & 1)) {
        frame = page_alloc();
        if (!frame)
            return 0;                               /* 分配器耗尽 */
        pgdir[pdi] = frame | PTE_KERNEL;
        __asm__ volatile ("invlpg (%0)" : : "r"(addr) : "memory");
    }

    /* 第二步：缺页表项，分配真实物理页兜住这个虚拟页 */
    pt = (unsigned int *)(pgdir[pdi] & 0xFFFFF000);
    if (pt[pti] & 1)
        return 0;

    frame = page_alloc();
    if (!frame)
        return 0;

    pt[pti] = frame | PTE_KERNEL;
    __asm__ volatile ("invlpg (%0)" : : "r"(addr) : "memory");
    return frame;
}

unsigned int page_unmap(unsigned int addr)
{
    unsigned int *pgdir = pgdir_now();
    unsigned int pdi = addr >> 22;
    unsigned int pti = (addr >> 12) & 0x3FF;
    unsigned int *pt;
    unsigned int frame;

    if (!(pgdir[pdi] & 1))
        return 0;

    pt = (unsigned int *)(pgdir[pdi] & 0xFFFFF000);
    if (!(pt[pti] & 1))
        return 0;

    frame = pt[pti] & 0xFFFFF000;
    pt[pti] = 0;                                   /* 撤掉映射 */
    __asm__ volatile ("invlpg (%0)" : : "r"(addr) : "memory");
    page_free(frame);                              /* 同时把物理页还给分配器 */
    return frame;
}

/* ---------------------------------------------------------------
 * guard 页：撤掉/恢复恒等映射里的一格
 *
 * 任务栈是走 head 留下的恒等映射访问的（esp 就是物理地址），所以“让栈下面
 * 那一页访问不到”唯一能做的就是把恒等映射里对应的一格清掉——物理页仍然
 * 归分配器所有（不会被别人拿走），但没有虚拟地址指向它，写进去必然 #PF。
 *
 * 恢复时必须把原来的 PTE 原样写回：那一格本来叫“恒等”，即 PTE = phys|flags，
 * 但不要假设它一定是这个值，记下来再放回去最稳。
 * --------------------------------------------------------------- */

unsigned int page_identity_unmap(unsigned int phys)
{
    unsigned int *pgdir = pgdir_now();
    unsigned int pdi = phys >> 22;
    unsigned int pti = (phys >> 12) & 0x3FF;
    unsigned int *pt;
    unsigned int pte;

    if (!(pgdir[pdi] & 1))
        return 0;

    pt = (unsigned int *)(pgdir[pdi] & 0xFFFFF000);
    pte = pt[pti];
    if (!(pte & 1))
        return 0;                                  /* 本来就没映射：不用撤 */

    pt[pti] = 0;
    __asm__ volatile ("invlpg (%0)" : : "r"(phys) : "memory");
    return pte;
}

void page_identity_restore(unsigned int phys, unsigned int pte)
{
    unsigned int *pgdir = pgdir_now();
    unsigned int pdi = phys >> 22;
    unsigned int pti = (phys >> 12) & 0x3FF;
    unsigned int *pt;

    if (!pte || !(pgdir[pdi] & 1))
        return;

    pt = (unsigned int *)(pgdir[pdi] & 0xFFFFF000);
    pt[pti] = pte;
    __asm__ volatile ("invlpg (%0)" : : "r"(phys) : "memory");
}

/* ---------------------------------------------------------------
 * 用户页：把恒等映射里的一格标成 U/S=1
 *
 * ring3 要能取指、读写，光有 PTE 的 P 位不够——U/S 位是“谁可以访问”的那一位。
 * PDE 已经是 0x07（head 建映射时就连 U/S 一起给了），所以这里只补 PTE。
 * 只改权限，不动页框，也不清内容（用户代码要先拷进来）。
 * --------------------------------------------------------------- */
unsigned int page_mark_user(unsigned int phys)
{
    unsigned int *pgdir = pgdir_now();
    unsigned int pdi = phys >> 22;
    unsigned int pti = (phys >> 12) & 0x3FF;
    unsigned int *pt;
    unsigned int pte;

    if (!(pgdir[pdi] & 1))
        return 0;

    pt = (unsigned int *)(pgdir[pdi] & 0xFFFFF000);
    pte = pt[pti];
    if (!(pte & 1))
        return 0;                       /* 没有映射：先有页再说权限 */

    pt[pti] = pte | 0x04;               /* U/S = 1，其余位原样保留 */
    __asm__ volatile ("invlpg (%0)" : : "r"(phys) : "memory");
    return pt[pti];
}

/* 反向动作：把这一格的 U/S 收回去。“临时放行一页、用完关上”用这一对。
 * 与 page_mark_user 对称：没有映射、或本来就没开 U 都返回 0。 */
unsigned int page_mark_kernel(unsigned int phys)
{
    unsigned int *pgdir = pgdir_now();
    unsigned int pdi = phys >> 22;
    unsigned int pti = (phys >> 12) & 0x3FF;
    unsigned int *pt;
    unsigned int pte;

    if (!(pgdir[pdi] & 1))
        return 0;

    pt = (unsigned int *)(pgdir[pdi] & 0xFFFFF000);
    pte = pt[pti];
    if (!(pte & 1) || !(pte & 0x04))
        return 0;                       /* 没映射，或者本来就已经关着 */

    pt[pti] = pte & ~0x04U;             /* U/S = 0，其余位原样保留 */
    __asm__ volatile ("invlpg (%0)" : : "r"(phys) : "memory");
    return 1;
}

unsigned int page_identity_flags(unsigned int phys)
{
    unsigned int *pgdir = pgdir_now();
    unsigned int pdi = phys >> 22;
    unsigned int pti = (phys >> 12) & 0x3FF;

    if (!(pgdir[pdi] & 1))
        return 0;
    return ((unsigned int *)(pgdir[pdi] & 0xFFFFF000))[pti];
}

/* ---------------------------------------------------------------
 * 收口：把 [lo, hi) 里所有 PTE 的 U/S 位清掉
 *
 * head 建恒等映射时**每一条 PTE 都带 0x07**（P|RW|U/S），于是 ring3 眼里
 * 内核 text、页表、分配器位图、VGA，乃至于 1MB 以上那 15MB 空白内存，
 * 全是普通可读内存——它今天打不到那儿，只是因为手上还没有指针。
 * 这一页一页清掉 U/S，就把“能不能进”从“恰好没去碰”变成“CPU 直接拦”。
 *
 * 为什么不只锁 1MB：1MB 只盖住内核镜像 + 页表 + VGA + 页分配器，而
 * 0x100000–0x1000000 这 15MB 同样整片 0x07。留着它，ring3 拿任意一个
 * 常数指针就能读、能写（RW 也在），前面锁的那 1MB 等于白锁。
 * 所以调用方直接传 (0, KERNEL_LOCK_END=16MB)，一次锁完整张恒等映射。
 *
 * 不动 PDE（PDE 的 U/S 一清，整个 4MB 都没法给用户页用了）。返回值是
 * “原本带 U/S、被清掉”的页数——这个数本身就是“这里原来真的是开着的”证据，
 * 不是推测。
 * --------------------------------------------------------------- */
unsigned int page_lock_kernel(unsigned int lo, unsigned int hi)
{
    unsigned int *pgdir = pgdir_now();
    unsigned int pa;
    unsigned int cleared = 0;

    pa = lo & ~0xFFFU;                  /* 起始向下对齐 */
    for (; pa < hi; pa += PAGE_SIZE) {
        unsigned int pdi = pa >> 22;
        unsigned int pti = (pa >> 12) & 0x3FF;
        unsigned int *pt;
        unsigned int pte;

        if (!(pgdir[pdi] & 1))
            continue;
        pt = (unsigned int *)(pgdir[pdi] & 0xFFFFF000);
        pte = pt[pti];
        if (!(pte & 1) || !(pte & 0x04))
            continue;                       /* 没映射，或本来就是内核页 */

        pt[pti] = pte & ~0x04U;
        __asm__ volatile ("invlpg (%0)" : : "r"((void *)pa) : "memory");
        cleared++;
    }

    return cleared;
}