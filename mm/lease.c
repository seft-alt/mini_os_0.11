/* ==============================================================
 * 用户页租借表（见 include/mm/lease.h 的语义说明）
 *
 * 槽位是静态数组：这个内核没有“动态分配内核对象”的能力，
 * 8 个槽足够覆盖“代码页 + 栈页 + 偶发探测页”的规模，满了就明确失败。
 * 键是 (owner, phys) —— 同一页可以被不同任务各借一份，互不续命、互不连坐。
 * ============================================================== */
#include "linux/lease.h"
#include "linux/page.h"
#include "linux/task.h"

struct page_lease {
    unsigned int owner;     /* PAGE_LEASE_OWNER_KERNEL = 内核；1..N = 任务槽位；0 = 空槽 */
    unsigned int phys;      /* 0 = 空槽 */
    unsigned int refs;      /* 同一 owner 借了几次 */
    unsigned int ttl;       /* 剩余 tick，到 0 强制回收 */
};

static struct page_lease leases[PAGE_LEASE_SLOTS];
static unsigned int forced_total;
static unsigned int revoked_total;
static unsigned int denied_total;
static unsigned int notified_total;
static unsigned int handed_total;

/* owner -> 回调。单独一张小表而不是塞进租约槽：一个 owner 可以借很多页，
 * 回调却只有一个；而且解约时是按 owner 注销，不是按页。 */
#define PAGE_LEASE_BINDS 4
struct lease_bind {
    unsigned int owner;                 /* 0 = 空 */
    page_lease_notify_fn cb;
};

static struct lease_bind binds[PAGE_LEASE_BINDS];

static page_lease_notify_fn lease_notify_of(unsigned int owner)
{
    int i;

    for (i = 0; i < PAGE_LEASE_BINDS; i++) {
        if (binds[i].owner == owner)
            return binds[i].cb;
    }
    return 0;
}

/* 发一次“被撤销”的通知。只为 TTL / OWNER 两种收法发：
 * close 是调用方自己的动作，它本来就知道，不需要再通知一遍。 */
static void lease_fire(unsigned int owner, unsigned int phys, unsigned int why)
{
    page_lease_notify_fn cb;

    if (why == PAGE_LEASE_WHY_CLOSE)
        return;
    cb = lease_notify_of(owner);
    if (!cb)
        return;
    notified_total++;
    cb(owner, phys, why);
}

/* 按 (owner, phys) 找槽位；没找到返回 -1 */
static int lease_find(unsigned int owner, unsigned int phys)
{
    int i;

    for (i = 0; i < PAGE_LEASE_SLOTS; i++) {
        if (leases[i].owner == owner && leases[i].phys == phys)
            return i;
    }
    return -1;
}

/* 这一页还有没有别的租约（决定“关掉”还是“继续开着”） */
static int lease_page_held(unsigned int phys)
{
    int i;

    for (i = 0; i < PAGE_LEASE_SLOTS; i++) {
        if (leases[i].phys == phys)
            return 1;
    }
    return 0;
}

/* 清掉一个槽位；若这一页已经没人借了，才真的收回 U/S。
 * why 说明这次是谁把租约收走的（决定要不要发撤销通知）。
 * 返回 1 = 因此关上了一页。 */
static int lease_drop(int slot, unsigned int why)
{
    unsigned int owner = leases[slot].owner;
    unsigned int phys = leases[slot].phys;
    int closed;

    leases[slot].owner = 0;
    leases[slot].phys = 0;
    leases[slot].refs = 0;
    leases[slot].ttl = 0;

    /* 这一笔没了，任务自己的借用表里也要跟着划掉——表是回收的唯一索引，
     * 留着一条已经被收走的页，回收时会去找一个不存在的槽位。 */
    if (owner && owner != PAGE_LEASE_OWNER_KERNEL)
        task_lease_forget(owner, phys);

    closed = 0;
    if (!lease_page_held(phys)) {           /* 还有别的 owner 在用，别关 */
        page_mark_kernel(phys);
        closed = 1;
    }
    /* 通知放在最后：页已经清账、U/S 已经收回，借用者听到的是既成事实；
     * 反过来（先通知再关）会让回调有机会改写一张马上要关的页。 */
    lease_fire(owner, phys, why);
    return closed;
}

int page_lease_open(unsigned int owner, unsigned int phys, unsigned int ttl)
{
    int slot;

    if (!phys || !ttl || !owner) {
        denied_total++;
        return 0;                           /* ttl=0 等于“借了立刻还”，没意义 */
    }
    slot = lease_find(owner, phys);
    if (slot >= 0) {
        /* 同一 owner 再借一次：加一个引用，租期取较长的那个
         * （同一 owner 的两笔借约不区分先后，续着用最省事） */
        leases[slot].refs++;
        if (ttl > leases[slot].ttl)
            leases[slot].ttl = ttl;
        return 1;
    }
    slot = lease_find(0, 0);                /* 空槽的 owner/phys 恒为 0 */
    if (slot < 0) {
        denied_total++;
        return 0;                           /* 池子满：明确失败，不静默成功 */
    }
    if (!page_mark_user(phys)) {
        denied_total++;
        return 0;                           /* 页没映射：借不出来 */
    }
    /* 任务借的页要记在任务自己身上；记不下就等于借不出来——
     * 先把刚放开的 U/S 收回去，再明确失败。 */
    if (owner != PAGE_LEASE_OWNER_KERNEL && !task_lease_note(owner, phys)) {
        if (!lease_page_held(phys))
            page_mark_kernel(phys);
        denied_total++;
        return 0;
    }
    leases[slot].owner = owner;
    leases[slot].phys = phys;
    leases[slot].refs = 1;
    leases[slot].ttl = ttl;
    return 1;
}

int page_lease_close(unsigned int owner, unsigned int phys)
{
    int slot = lease_find(owner, phys);

    if (slot < 0)
        return 0;                           /* 没借过，或者已经被强制回收了 */

    leases[slot].refs--;
    if (leases[slot].refs > 0)
        return 0;                           /* 同一 owner 还借了不止一次：先别关 */
    return lease_drop(slot, PAGE_LEASE_WHY_CLOSE);
}

unsigned int page_lease_release_owner(unsigned int owner)
{
    unsigned int closed = 0;
    unsigned int phys;
    int i;
    int slot;

    if (!owner || owner == PAGE_LEASE_OWNER_KERNEL)
        return 0;                           /* 内核自己的租约不随任务回收 */

    /* 按任务自己的借用表收：表里有几条就找几个槽位，不再线性扫全局表。
     * 表里没有的页一定不是它借的——所以“收错别人的页”从结构上就不可能。 */
    for (i = 0; i < TASK_LEASES; i++) {
        phys = task_lease_at(owner, i);
        if (!phys)
            continue;
        slot = lease_find(owner, phys);
        if (slot >= 0) {
            if (lease_drop(slot, PAGE_LEASE_WHY_OWNER))
                closed++;
        } else {
            task_lease_forget(owner, phys); /* 表里有、租约表里没有：把脏条目清掉 */
        }
    }
    /* 回收 = 解约：主人没了，回调指针也就不能再留着——
     * 任务槽位是复用的，留着等于让下一个任务继承一个野指针。 */
    page_lease_unbind(owner);
    revoked_total += closed;
    return closed;
}

unsigned int page_lease_tick(void)
{
    unsigned int expired = 0;
    int i;

    for (i = 0; i < PAGE_LEASE_SLOTS; i++) {
        if (!leases[i].phys)
            continue;
        /* 已经交给任务的那几笔不参与倒计时：主人死了才有理由收，
         * 时间不是理由（否则用户态跑得久一点，正跑着的页就被收走）。 */
        if (leases[i].ttl == PAGE_LEASE_NO_TTL)
            continue;
        leases[i].ttl--;
        if (!leases[i].ttl) {
            /* 超时：不管 refs 还剩几，一律收回。这是“忘了关”的兜底 */
            if (lease_drop(i, PAGE_LEASE_WHY_TTL))
                forced_total++;
            expired++;
        }
    }
    return expired;
}

int page_lease_handoff(unsigned int from, unsigned int to, unsigned int phys)
{
    int slot = lease_find(from, phys);

    if (slot < 0 || !to || to == from)
        return 0;                           /* 没这笔账，或者原地打转 */
    /* 接收方得先记下来：借用表是回收的唯一索引，表里没有就等于没人管。
     * 记不下就整笔留在原地（它的旧 ttl 还在，兜底照旧）。 */
    if (to != PAGE_LEASE_OWNER_KERNEL && !task_lease_note(to, phys))
        return 0;
    if (from && from != PAGE_LEASE_OWNER_KERNEL)
        task_lease_forget(from, phys);
    leases[slot].owner = to;
    leases[slot].ttl = PAGE_LEASE_NO_TTL;   /* 页归任务：时间不再是理由 */
    handed_total++;
    return 1;
}

unsigned int page_lease_handed(void)
{
    return handed_total;
}

unsigned int page_lease_owner_of(unsigned int phys)
{
    int i;

    for (i = 0; i < PAGE_LEASE_SLOTS; i++) {
        if (leases[i].phys == phys && phys)
            return leases[i].owner;
    }
    return 0;
}

unsigned int page_lease_active(void)
{
    unsigned int n = 0;
    int i;

    for (i = 0; i < PAGE_LEASE_SLOTS; i++) {
        if (leases[i].phys && leases[i].refs)
            n++;
    }
    return n;
}

unsigned int page_lease_forced(void)
{
    return forced_total;
}

unsigned int page_lease_revoked(void)
{
    return revoked_total;
}

unsigned int page_lease_denied(void)
{
    return denied_total;
}

unsigned int page_lease_refs(unsigned int owner, unsigned int phys)
{
    int slot = lease_find(owner, phys);

    return slot < 0 ? 0 : leases[slot].refs;
}

int page_lease_bind(unsigned int owner, page_lease_notify_fn cb)
{
    int i;

    if (!owner || !cb)
        return 0;
    for (i = 0; i < PAGE_LEASE_BINDS; i++) {
        if (binds[i].owner == owner) {      /* 已经有：覆盖 */
            binds[i].cb = cb;
            return 1;
        }
    }
    for (i = 0; i < PAGE_LEASE_BINDS; i++) {
        if (!binds[i].owner) {
            binds[i].owner = owner;
            binds[i].cb = cb;
            return 1;
        }
    }
    return 0;                               /* 回调表满：明确失败 */
}

int page_lease_unbind(unsigned int owner)
{
    int i;

    for (i = 0; i < PAGE_LEASE_BINDS; i++) {
        if (binds[i].owner == owner) {
            binds[i].owner = 0;
            binds[i].cb = 0;
            return 1;
        }
    }
    return 0;
}

unsigned int page_lease_notified(void)
{
    return notified_total;
}
