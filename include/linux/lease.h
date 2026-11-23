/* ==============================================================
 * 用户页的“租借”：给放行加一份账
 *
 * page_mark_user / page_mark_kernel 是一对裸开关：谁放行谁必须记得关，
 * 忘了就是永久开着，而且没有任何计数器能发现。这里把“放行”变成一次**租借**：
 *
 *   open(owner, phys, ttl) —— 把这一页标成 U/S=1，refs++。
 *   close(owner, phys)     —— refs--；降到 0 才真的把 U/S 收回去。返回“这一页
 *                             现在是否已经关上”，调用方能直接看出“还有别人在用”。
 *   release_owner(owner)   —— 把这个所有者名下的租约一次收干净（任务死亡时调）。
 *   tick()                 —— 每个时钟 tick 调一次：每笔租约各自 ttl--，到 0 就
 *                             **强制回收**（不管 refs 还剩几）并 forced++。这是
 *                             “忘了关”的兜底。ttl == PAGE_LEASE_NO_TTL 的（已经
 *                             交给任务的那些）跳过，不参与倒计时。
 *   handoff(from,to,phys)  —— 换账：把一笔租约改记到任务槽位上并改成永不到期。
 *
 * 关键一条：**键是 (owner, phys)，不是 phys**。同一个页可以被不同任务各借一份，
 * 各自还各自的；A 的长租期不会替 B 续命，A 死了也不会把 B 的那份一起收走
 * （只有“这一页最后一个租约消失”时才真的清 U/S）。
 *
 * 池子只有 8 个槽，满了再 open 返回 0（不静默成功）；租约也不记“借来干什么”，
 * owner 就是它的身份。
 *
 * ---- 可撤销契约：回调 ----
 * 光把页收回来是不够的——借用者那边只会看到“页突然没了”。所以再加一条：
 * bind(owner, cb) 之后，这一 owner 名下的每笔租约在**被强制收走**（超时或主人
 * 死亡，不是它自己还的）时都会收到一次通知，参数里带 why。规矩有两条：
 *   1. 通知只对“不是自己还的”那两种收法发（close 是调用方自己的动作，它知道）；
 *   2. 通知必须发生在 dtor **之后**（dtor 里还可能合法用到那页）。
 * 回收 = 解约：release_owner 收完租约顺手把这个 owner 的回调一起注销，
 * 免得任务槽位被复用后，旧回调指针还在表里。
 * ============================================================== */
#ifndef MINI_OS_LEASE_H
#define MINI_OS_LEASE_H

#define PAGE_LEASE_SLOTS        8
#define PAGE_LEASE_OWNER_KERNEL 0xFF   /* 内核自己（main），不与任何任务槽位冲突 */

/* “永不到期”的租期。只在 handoff 时出现：页一旦归了任务，回收的唯一触发点
 * 就变成“这个槽位死掉”（page_lease_release_owner），时间不该再来插一脚。 */
#define PAGE_LEASE_NO_TTL       0xFFFFFFFFu

/* 借一页：标记 U/S=1 并 refs++（同一 owner 同一页可重复借）。ttl 单位是时钟 tick。
 * 返回 1 成功；池子满、页没映射、ttl=0 都返回 0。 */
int page_lease_open(unsigned int owner, unsigned int phys, unsigned int ttl);

/* 还一页：refs--。refs 归零且没有别的 owner 还借这一页时才真的收回 U/S。
 * 返回 1 = 这一页已经关上；0 = 还有租约、仍然开着（或这一页没被借过）。 */
int page_lease_close(unsigned int owner, unsigned int phys);

/* 把这个 owner 名下所有租约收掉（任务死了顺手收回，不用它自己记得还）。
 * 返回因此被真正关上的页数。 */
unsigned int page_lease_release_owner(unsigned int owner);

/* ---- 交接：把一笔租约改到另一个 owner 名下 ----
 *
 * 场景就是 exec：内核替新任务把页准备好、放行，任务建起来之后页归任务。
 * 这一步不改 U/S（页本来就开着），只改"这笔账记在谁头上"，并且把租期
 * 从 ttl 改成 PAGE_LEASE_NO_TTL——**页归任务之后，时间不再是回收的理由**，
 * 否则 ring3 一旦放开 IF，用户态跑得久一点，内核就会在别人正跑着的页上
 * 把 U/S 收走（这是把全局单值拆成每任务一份之后新露出来的隐患）。
 *
 * 接收方记不下（借用表满）就返回 0、这笔留在原地不动：内核名下的 ttl 还在，
 * 兜底照旧生效。from 是 KERNEL 时不需要从任何任务的表里划掉。 */
int page_lease_handoff(unsigned int from, unsigned int to, unsigned int phys);

/* 累计交接成功的笔数（"页真的归了任务"的可数证据） */
unsigned int page_lease_handed(void);

/* 这一页现在记在谁名下（0 = 没有这笔租约）；同一个页有多笔时返回第一笔。 */
unsigned int page_lease_owner_of(unsigned int phys);

/* 每个 tick 调一次：每笔租约 ttl--，到 0 的强制收回（refs 清零、forced++）。
 * 返回本次被强制回收的页数。 */
unsigned int page_lease_tick(void);

/* 还活着的租约笔数、被强制回收的累计页数、因任务死亡被收回的累计页数、
 * 被拒的 open 次数 */
unsigned int page_lease_active(void);
unsigned int page_lease_forced(void);
unsigned int page_lease_revoked(void);
unsigned int page_lease_denied(void);

/* 这个 (owner, phys) 的引用计数（0 = 没被借或已还清） */
unsigned int page_lease_refs(unsigned int owner, unsigned int phys);

/* ---- 可撤销契约 ---- */

#define PAGE_LEASE_WHY_TTL   0   /* ttl 到期被强制回收 */
#define PAGE_LEASE_WHY_OWNER 1   /* 主人死了，被 release_owner 收走 */
#define PAGE_LEASE_WHY_CLOSE 2   /* 调用方自己还清（不算“被撤销”） */

typedef void (*page_lease_notify_fn)(unsigned int owner, unsigned int phys,
                                     unsigned int why);

/* 给 owner 登记一个通知回调：之后它名下的租约被 TTL/OWNER 收走时会调一次。
 * 返回 1 成功；0 = owner 非法或回调表满。 */
int page_lease_bind(unsigned int owner, page_lease_notify_fn cb);

/* 注销（release_owner 内部会自动做一次）。返回 1 = 真的删掉了一条。 */
int page_lease_unbind(unsigned int owner);

/* 已经发出去的通知条数（含 TTL 与 OWNER 两种） */
unsigned int page_lease_notified(void);

#endif /* MINI_OS_LEASE_H */
