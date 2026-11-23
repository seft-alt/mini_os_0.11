/* sched/queue.c —— 谁下一个跑：三级就绪队列 + 睡眠小顶堆 + 老化/唤醒
 *
 * 从 sched/task.c 分出来的是“**队列本身**”：rq 的环形数组与三级下标、
 * sq 的小顶堆、老化与到点唤醒。它们只碰 `tasks[]` 里的 prio/state/wake_tick/wait，
 * 不分配栈、不换栈、不认识 dtor 与租约——所以可以把它们当成纯数据结构单练。
 *
 * 就绪队列：**每个优先级一条**环形数组，取任务从最高级的队头取，高优先级一旦
 * 就绪立刻抢走（不等低优先级的时间片用完）。
 * 睡眠队列：以 wake_tick 为键的小顶堆，插入/出堆都 O(log N)，键直接读
 * `tasks[slot].wake_tick`，不另存一份唤醒表。
 *
 * 时间片与历史惩罚（`slice_for` / `note_quantum_burn`）也放在这里：它们读的是
 * 优先级与片上计数，跟队列的“下一片给谁”是同一件事的两面。
 */
#include "linux/taskint.h"
#include "linux/tune.h"
#include "linux/timer.h"


/* 取当前该用的时间片：任务自己给了就用自己的，给 0 就按**当前**优先级取默认值。
 * 注意是“当前”而不是“创建时”——老化把任务升到高优先级后，它的时间片
 * 也必须跟着变短，否则升上去还能霸占 8 个 tick，比不升级更糟。 */
unsigned long slice_for(int slot)
{
    return tasks[slot].slice ? tasks[slot].slice : sched_tune.slice[tasks[slot].prio];
}

/* 历史惩罚（衰减）：一片时间片**真的被跑满**才记一笔，连续
 * sched_tune.decay_quanta 片之后把“落回级别”降一级。
 *
 * 为什么降的是 base_prio 而不是 prio：prio 每轮结束都会被 schedule 按 base 重装，
 * 改 prio 会被下一次重装抹掉；base_prio 才是“安静下来之后会回到哪一级”。
 *
 * 为什么必须在 sched_tick 里记账而不是在 schedule 里：schedule 的 left==0 分支
 * 既可能是“跑满了”，也可能是 task_yield / task_sleep 手动清零进来的——后者是
 * 自愿让出，不该挨罚。tick 里递减到 0 的那一下只可能是真跑满。 */
void note_quantum_burn(int slot)
{
    if (tasks[slot].base_prio <= 0)
        return;                     /* 已经是最低级，罚无可罚 */
    if (++tasks[slot].hogs < sched_tune.decay_quanta)
        return;
    tasks[slot].hogs = 0;
    tasks[slot].base_prio--;
    if (tasks[slot].prio > tasks[slot].base_prio)
        tasks[slot].prio = tasks[slot].base_prio;
}

/* ---- 就绪队列：每个优先级一条环形数组 ---- */
static int rq[TASK_PRIO_LEVELS][TASK_SLOTS];
static int rq_head[TASK_PRIO_LEVELS];
static int rq_count[TASK_PRIO_LEVELS];

/* ---- 睡眠队列：以 wake_tick 为键的小顶堆 ----
 * 之前是“按 wake_tick 升序的数组 + 插入排序”：唤醒 O(1)，但插入要搬 O(N) 个元素。
 * 换成小顶堆之后插入 O(log N)、唤醒每次 O(log N)，而且不再需要 sq_wake 数组——
 * 键直接读 tasks[slot].wake_tick，省一份状态就少一处能对不上的地方。 */
static int sq_heap[TASK_SLOTS];
static int sq_count;

/* ---------------------------------------------------------------
 * 队列基本操作
 * --------------------------------------------------------------- */

int rq_contains(int slot)
{
    int p;
    int i;

    for (p = 0; p < TASK_PRIO_LEVELS; p++)
        for (i = 0; i < rq_count[p]; i++)
            if (rq[p][(rq_head[p] + i) % TASK_SLOTS] == slot)
                return 1;
    return 0;
}

void rq_push(int slot)
{
    int p = tasks[slot].prio;

    if (rq_count[p] >= TASK_SLOTS || rq_contains(slot))
        return;                         /* 队列满或已在队里：不重复入队 */

    rq[p][(rq_head[p] + rq_count[p]) % TASK_SLOTS] = slot;
    rq_count[p]++;
}

/* 把某个槽位从它所在的那一级队里摘掉（优先级被老化改过之后要换队） */
void rq_remove(int slot)
{
    int p;
    int i;
    int j;

    for (p = 0; p < TASK_PRIO_LEVELS; p++) {
        for (i = 0; i < rq_count[p]; i++) {
            if (rq[p][(rq_head[p] + i) % TASK_SLOTS] == slot) {
                for (j = i; j + 1 < rq_count[p]; j++)
                    rq[p][(rq_head[p] + j) % TASK_SLOTS] =
                        rq[p][(rq_head[p] + j + 1) % TASK_SLOTS];
                rq_count[p]--;
                return;
            }
        }
    }
}

/* 最高优先级的队头；peek 不摘，pop 摘下来 */
int rq_top(int pop)
{
    int p;

    for (p = TASK_PRIO_LEVELS - 1; p >= 0; p--) {
        if (rq_count[p] > 0) {
            int slot = rq[p][rq_head[p]];

            if (pop) {
                rq_head[p] = (rq_head[p] + 1) % TASK_SLOTS;
                rq_count[p]--;
            }
            return slot;
        }
    }
    return -1;
}

/* ---------------------------------------------------------------
 * 睡眠队列（小顶堆）
 * --------------------------------------------------------------- */

int sq_less(int a, int b)
{
    return tasks[a].wake_tick < tasks[b].wake_tick;     /* 到期早的在上 */
}

void sq_swap(int i, int j)
{
    int t = sq_heap[i];

    sq_heap[i] = sq_heap[j];
    sq_heap[j] = t;
}

/* 插入：放到末尾再上浮。相等时不交换，保证“同时到期”的先来先醒 */
void sq_insert(int slot)
{
    int i = sq_count++;

    sq_heap[i] = slot;
    while (i > 0) {
        int parent = (i - 1) / 2;

        if (!sq_less(sq_heap[i], sq_heap[parent]))
            break;
        sq_swap(i, parent);
        i = parent;
    }
}

/* 下沉：把末尾元素搬到根上再往下滚，这是 pop 的标准做法 */
void sq_sift_down(int i)
{
    for (;;) {
        int left = 2 * i + 1;
        int right = left + 1;
        int min = i;

        if (left < sq_count && sq_less(sq_heap[left], sq_heap[min]))
            min = left;
        if (right < sq_count && sq_less(sq_heap[right], sq_heap[min]))
            min = right;
        if (min == i)
            return;
        sq_swap(i, min);
        i = min;
    }
}

void sq_pop(void)
{
    sq_count--;
    if (sq_count <= 0)
        return;
    sq_heap[0] = sq_heap[sq_count];
    sq_sift_down(0);
}

/* 老化：就绪但一直排不上的任务，等够 sched_tune.age_ticks 就升一级。
 * 这样“高优先级一直不空”不会让低优先级永远拿不到 CPU——但也不是平均分，
 * 低优先级只是拿到一个短促的时间片，跑完降回原级。 */
void aging_pass(void)
{
    int slot;

    for (slot = 1; slot < TASK_SLOTS; slot++) {
        if (tasks[slot].state != TASK_READY || slot == cur)
            continue;

        if (++tasks[slot].wait < sched_tune.age_ticks)
            continue;

        tasks[slot].wait = 0;
        if (tasks[slot].prio >= TASK_PRIO_LEVELS - 1)
            continue;                   /* 已经在最高级，没得升 */

        rq_remove(slot);                /* 换队：先摘出旧的一级 */
        tasks[slot].prio++;
        rq_push(slot);
    }
}

/* 堆顶到期才唤醒；一次 tick 里可能连着到期好几个，所以是 while 而不是 if */
void wake_sleepers(void)
{
    unsigned long now = timer_ticks();

    while (sq_count > 0 && tasks[sq_heap[0]].wake_tick <= now) {
        int slot = sq_heap[0];

        sq_pop();
        tasks[slot].state = TASK_READY;
        rq_push(slot);
    }
}
/* ---------------------------------------------------------------
 * 门面：task.c 只通过这三个入口看/清队列，不碰下标
 * --------------------------------------------------------------- */

/* 清空两条队列（task_reset 换一组任务时用） */
void queue_reset(void)
{
    int i;

    for (i = 0; i < TASK_PRIO_LEVELS; i++) {
        rq_head[i] = 0;
        rq_count[i] = 0;
    }
    sq_count = 0;
}

/* 三级就绪队列合计长度（自检证据） */
int rq_depth_total(void)
{
    int p;
    int n = 0;

    for (p = 0; p < TASK_PRIO_LEVELS; p++)
        n += rq_count[p];
    return n;
}

/* 睡眠队列当前长度 */
int sq_depth(void)
{
    return sq_count;
}
