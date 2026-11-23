/* ==============================================================
 * 抢占式调度器：多级优先级就绪队列 + 时间片 + 睡眠堆 + 槽位复用
 *
 * 切换机制仍然是“换栈”：时钟中断进来时 CPU 已经把 eip/cs/eflags 压在
 * **当前任务自己的栈**上，桩再 pusha 一次，现场就在栈顶。调度器把当前数字
 * 记进任务结构、取出下一个任务的 esp，桩 `mov esp, eax` 之后 popa + iret
 * 恢复的就是另一个任务的现场。
 *
 * 任务状态机：
 *
 *   UNUSED --task_spawn--> READY --被选中--> RUNNING
 *                            ^                  |
 *                            |      task_sleep  v
 *                       到期唤醒            SLEEPING
 *                            |
 *                            +------- task_exit ----> DEAD --> UNUSED（栈延后回收）
 *
 * 两个队列（都只存槽位号，不存任务结构本身）：
 *
 *   就绪队列 rq：**每个优先级一条**环形数组。取任务时从最高优先级的队头取，
 *                低优先级只有在高优先级全空时才轮得到 —— 这就是“优先级”。
 *   睡眠队列 sq：以 wake_tick 为键的**小顶堆**。每个 tick 只看堆顶，堆顶到期
 *                才出堆；插入与出堆都是 O(log N)，不需要再搬数组。
 *
 * 时间片 slice：每个任务带一个以 tick 计的 slice 和一个递减的 left。
 * 一次 tick 中断里 left--；left 归零才把当前任务排回自己那条队尾并换人。
 * 所以“同优先级轮转”的节奏是 slice 个 tick 换一次，而**高优先级一旦就绪，
 * 立刻抢走**（不等 slice 用完）。
 *
 * 优先级有两个方向：
 *   老化（向上借）：就绪却排队超过 sched_tune.age_ticks 个 tick，临时升一级，防饿死；
 *   衰减（向下压）：连续跑满 sched_tune.decay_quanta 片时间片，落回级别降一级，罚贪心。
 * 借来的级别在一轮结束时归还（prio 回 base_prio），压下去的级别是持久的
 * （改的是 base_prio）。task_yield / task_sleep 清零惩罚计数——自愿让出不算贪心。
 * 反方向也有一条：sched_tune.restore_ticks 是“回血单价”——每睡满这么多 tick 就把
 * 罚掉的级别还回一级，睡 k 个单价就还 k 级（封顶最高级）；不足一个单价不还，
 * 免得“睡一小会儿”就把惩罚洗掉。
 *
 * 任务栈上要多摆两个格子给入口函数：cdecl 进入函数时 [esp] 是返回地址、
 * [esp+4] 是第一个参数，于是栈顶依次是 ret 桩和 arg，函数一 return 就掉进
 * task_return_stub 自动 task_exit()。
 *
 * 任务栈布局（frame 指向栈上 popa 保存区，也就是 esp 的落点）：
 *
 *   frame[0] edi   frame[4] ebp   frame[8]  eip     <- 任务入口
 *   frame[1] esi   frame[5] ebx   frame[9]  cs      <- 0x08 内核代码段
 *   frame[2] ebp   frame[6] edx   frame[10] eflags  <- 0x202（IF=1）
 *   frame[3] esp   frame[7] eax   frame[11] ret     <- task_return_stub
 *                                 frame[12] arg     <- 入口函数的唯一参数
 * ============================================================== */
#include "linux/task.h"
#include "linux/idt.h"
#include "linux/intr.h"
#include "asm/io.h"
#include "linux/kernel.h"
#include "linux/page.h"
#include "linux/lease.h"
#include "linux/printk.h"
#include "linux/vector.h"
#include "linux/timer.h"
#include "linux/vga.h"
#include "linux/keyboard.h"
#include "linux/tune.h"
#include "linux/tss.h"
#include "linux/syscall.h"
#include "linux/fd.h"
#include "linux/pgdir.h"

#include "linux/taskint.h"



#define PAGE_BYTES       4096
#define FRAME_WORDS      13         /* 8 个寄存器 + eip/cs/eflags + ret + arg */

/* 老化阈值 / 衰减片数 / 回血阈值 / 各优先级时间片都搬到了 sched/tune.c，
 * 运行时可变（见 include/sched/tune.h）；下面这些常量只保留分级相关的。 */

task_t tasks[TASK_SLOTS];
static unsigned int lease_peak;    /* 借用表历史最高水位 */
static unsigned int n_waits;       /* 累计"挂在通道上让出"的次数（证据用） */
static unsigned int n_wakeups;     /* 累计"被 task_wakeup 叫回就绪"的次数 */

int cur;
int active;
unsigned long switches;
static unsigned int reclaim_stack;  /* 待回收的栈页：换栈之后才还，避免在自己栈上释放 */
static unsigned int reclaim_guard;  /* 与栈页配套的 guard 页，同样延后回收 */
static unsigned int reclaim_guard_pte;
static void (*reclaim_dtor)(void *arg, int reason);  /* 待交接的参数析构（换栈之后才调） */
static void *reclaim_arg;
static int reclaim_reason;                /* 该任务的死因，跟着 dtor 一起延后交接 */
static unsigned int reclaim_owner;        /* 该任务槽位号：租约随它一起收回（0 = 没有） */
static unsigned long dtor_runs;          /* dtor 实际执行次数（自检证据） */

/* 跟着任务一起延后回收的两笔：进内核栈页、以及 exec 交给它的用户页。
 * 都必须在“已经换到别的栈上”之后才动手（release_pending 里做）。 */
static unsigned int reclaim_esp0;
static unsigned int reclaim_ucode;
static unsigned int reclaim_upages;
static unsigned int reclaim_ustack;

/* spawn 之前的“页交接”：exec 装上页之后先寄存在这里，task_create 取走记进槽位。
 * 这样“页归任务”在创建那一刻就成立，不存在“任务已经在跑、页还没记上”的窗口。 */
static unsigned int offer_code, offer_pages, offer_stack;

/* 每槽位一份“进 ring3 之前的内核 esp”：boot/intr.asm 的 user_enter 往里写、
 * user_return_stub 从里读。以前这是一张全局格子，于是两个用户行程会互相覆盖；
 * 现在按下标分槽，谁进谁出都对得上自己那张栈（按 `cur` 索引，见 intr.asm）。 */
unsigned long user_kret[TASK_SLOTS];

/* 调试钩子（定位“谁把 exec 证据清零”用，定位完删） */

/* 入口函数 return 之后的落点：替任务收尾 */
static void task_return_stub(void *arg);

/* 还掉延后回收的栈页与 guard 页（定义在 schedule 之前，task_reclaim 先用到） */
static void release_pending(void);

/* ---------------------------------------------------------------
 * 任务建立 / 调度
 * --------------------------------------------------------------- */

/* 找一个空闲槽位、分配一整页当栈，栈顶摆好 popa + iret 现场和入口参数。
 * 成功后立刻排进就绪队列（调度还没开时入队也无害，开了以后才有人取）。 */
static int task_create(void (*entry)(void *), void *arg, void (*dtor)(void *, int),
                       int prio, unsigned long slice)
{
    unsigned int pair;
    unsigned int base;
    unsigned int pte;
    unsigned int e0;
    unsigned int *frame;
    int slot = 0;
    int i;

    /* 入口为空就直接拒掉：真让它进队，iret 会跳到 eip=0，三页白占还换不来证据 */
    if (!entry)
        return 0;

    for (i = 1; i < TASK_SLOTS; i++) {
        if (tasks[i].state == TASK_UNUSED) {
            slot = i;
            break;
        }
    }
    if (!slot)
        return 0;                       /* 没有空槽 */

    /* 一次要连续两页：低的那页当 guard、高的那页当栈。
     * guard 的恒等映射撤掉之后，栈往下溢出第一件事就是撞上它 → #PF。 */
    pair = page_alloc_pair();
    if (!pair)
        return 0;                       /* 没有空闲页（或找不到连续两页） */

    /* 再要一页给“从 ring3 进 ring0”用：TSS.esp0 会换成这个任务的栈顶，
     * 于是两个任务各自进内核也不会踩同一张栈。它也是本任务自己的页，
     * 任务死时和栈页一起还（见 release_pending）。 */
    e0 = page_alloc();
    if (!e0) {
        page_free(pair);
        page_free(pair + PAGE_BYTES);
        return 0;
    }

    pte = page_identity_unmap(pair);
    if (!pte) {                         /* 恒等映射里没有这一格：说明映射布局变了 */
        page_free(pair);
        page_free(pair + PAGE_BYTES);
        page_free(e0);
        return 0;
    }

    base = pair + PAGE_BYTES;
    tasks[slot].stack = base;
    tasks[slot].guard = pair;
    tasks[slot].guard_pte = pte;
    tasks[slot].esp0_stack = e0;
    tasks[slot].esp0 = (unsigned long)e0 + PAGE_BYTES;
    /* 用户页：由 exec 在 spawn 之前交接（见 task_user_offer）。普通任务这几格是 0 */
    tasks[slot].ucode = offer_code;
    tasks[slot].upages = offer_pages;
    tasks[slot].ustack = offer_stack;
    offer_code = offer_pages = offer_stack = 0;
    tasks[slot].wake_tick = 0;
    tasks[slot].runs = 0;
    tasks[slot].ticks = 0;
    tasks[slot].wait = 0;
    tasks[slot].hogs = 0;
    tasks[slot].chan = 0;               /* 新任务没在等任何通道 */
    tasks[slot].arg = arg;
    tasks[slot].dtor = dtor;
    /* 领一个键盘归属槽：这里失败（槽满）不算错误——退回共享槽 0，
     * 任务照跑，只是它的按键和内核是同一份队列。 */
    tasks[slot].kbd_tag = kbd_owner_attach();
    tasks[slot].death = TASK_DEATH_EXIT;   /* 还没死；真死了由 task_die 覆盖 */
    tasks[slot].prio = (prio >= TASK_PRIO_LEVELS) ? TASK_PRIO_LEVELS - 1
                          : (prio < 0 ? 0 : prio);
    tasks[slot].base_prio = tasks[slot].prio;
    /* slice 传 0 表示“每轮按当前优先级取默认值”：高级短、低级长 */
    tasks[slot].slice = slice;
    tasks[slot].left = slice_for(slot);

    frame = (unsigned int *)(base + PAGE_BYTES) - FRAME_WORDS;
    for (i = 0; i < FRAME_WORDS; i++)
        frame[i] = 0;                   /* 页分配时已清零，这里显式摆明布局 */

    frame[8]  = (unsigned int)entry;             /* eip：iret 之后从这里开始执行 */
    frame[9]  = KERNEL_CS;                       /* cs */
    frame[10] = 0x202;                           /* eflags：IF=1，任务里可以被打断 */
    frame[11] = (unsigned int)task_return_stub;  /* 入口函数 return 之后去哪 */
    frame[12] = (unsigned int)arg;               /* cdecl 的第一个参数 */

    /* esp 指向 popa 保存区（frame[0]）：桩先 popa 弹掉 frame[0..7]，
     * iret 再弹 frame[8..10]，之后 esp 正好落在 frame[11]（ret 槽），
     * 入口函数就能从 [esp+4] 拿到 frame[12] 里的参数。 */
    tasks[slot].esp = (unsigned long)frame;
    tasks[slot].state = TASK_READY;
    rq_push(slot);
    return slot;
}

/* 清空所有槽位与队列，并装好 0x30 让出门。允许多次调用（每次换一组任务） */
void task_reset(void)
{
    int i;

    for (i = 0; i < TASK_SLOTS; i++) {
        tasks[i].esp = 0;
        tasks[i].stack = 0;
        tasks[i].guard = 0;
        tasks[i].guard_pte = 0;
        tasks[i].esp0_stack = 0;
        tasks[i].esp0 = 0;
        tasks[i].ucode = 0;
        tasks[i].upages = 0;
        tasks[i].ustack = 0;
        tasks[i].wake_tick = 0;
        tasks[i].runs = 0;
        tasks[i].ticks = 0;
        tasks[i].slice = 1;
        tasks[i].left = 0;
        tasks[i].wait = 0;
        tasks[i].hogs = 0;
        tasks[i].chan = 0;
        tasks[i].arg = 0;
        tasks[i].dtor = 0;
        tasks[i].death = TASK_DEATH_EXIT;
        tasks[i].prio = 0;
        tasks[i].base_prio = 0;
        tasks[i].leases[0] = 0;
        tasks[i].leases[1] = 0;
        tasks[i].leases[2] = 0;
        tasks[i].leases[3] = 0;
        tasks[i].state = TASK_UNUSED;
        /* ring3 的指针窗口是"这一趟行程的私有约定"，槽位一清就得跟着清：
         * 留着等于让下一个任务继承一个已经死掉的窗口（永久后门）。 */
        sys_user_window_reset(i);
        /* fd 表同理：不清的话下一个任务会继承"别人打开的文件 + 别人读到的位置"，
         * 那不只是一处泄漏，是把上一个任务的读进度当成自己的起点。 */
        fd_close_all(i);
    }
    lease_peak = 0;

    queue_reset();

    /* slot 0 是 main：它的 esp 在第一次被抢占时才有值，且永远不进就绪队列，
     * 于是“所有 worker 都在睡”的时候自然落到这里空转。
     * main 的进内核栈用内核自带的那张（它不在任务页上），所以 esp0 取默认栈顶。 */
    tasks[0].state = TASK_READY;
    tasks[0].esp0 = tss_default_esp0();

    cur = 0;
    switches = 0;
    active = 0;
    kbd_owner_force(0);                 /* 开局键盘归共享槽 0 */
    reclaim_stack = 0;
    reclaim_guard = 0;
    reclaim_guard_pte = 0;
    reclaim_esp0 = 0;
    reclaim_ucode = 0;
    reclaim_upages = 0;
    reclaim_ustack = 0;
    offer_code = offer_pages = offer_stack = 0;
    for (i = 0; i < TASK_SLOTS; i++)
        user_kret[i] = 0;               /* 进 ring3 的交接格子也清干净 */
    tss_set_esp0(tasks[0].esp0);        /* 开局就从默认栈开始 */

    idt_set_gate(IDT_VEC_YIELD, isr_yield);
}

void task_sched_on(void) { active = 1; }
int task_active(void)    { return active; }
unsigned long task_switches(void) { return switches; }
int task_self(void)      { return cur; }

/* 某个槽位的键盘归属槽（0 = 共享/没领到） */
int task_kbd_tag_of(int slot)
{
    if (slot < 0 || slot >= TASK_SLOTS)
        return 0;
    return tasks[slot].kbd_tag;
}
int task_rq_depth(void)  { return rq_depth_total(); }
int task_sq_depth(void)  { return sq_depth(); }

/* 某个任务被记账的 tick 数：配合 slice 用来证明“时间片真的按 tick 递减” */
unsigned long task_ticks_of(int slot)
{
    if (slot < 0 || slot >= TASK_SLOTS)
        return 0;
    return tasks[slot].ticks;
}

/* 当前优先级（老化会临时抬高、衰减会压低）：自检里用来观察“被降级了没有” */
int task_prio_of(int slot)
{
    if (slot < 0 || slot >= TASK_SLOTS)
        return -1;
    return tasks[slot].prio;
}

int task_slots(void)
{
    return TASK_SLOTS;
}

/* 状态名给 shell 用：编码留在调度器里，外面只认字符串。
 * “run”与“ready”必须分开——同一个 READY 槽位，正在跑的那个才吃 CPU，
 * 混成一个名字就没人能靠屏幕判断“到底谁在跑”。 */
const char *task_state_name(int slot)
{
    if (slot < 0 || slot >= TASK_SLOTS)
        return 0;
    if (tasks[slot].state == TASK_READY && slot == cur)
        return "run";
    switch (tasks[slot].state) {
    case TASK_READY:    return "ready";
    case TASK_SLEEPING: return "sleep";
    case TASK_WAIT:     return "wait";
    case TASK_DEAD:     return "dead";
    default:            return "unused";
    }
}

unsigned long task_slice_of(int slot)
{
    return (slot < 0 || slot >= TASK_SLOTS) ? 0 : tasks[slot].slice;
}

unsigned long task_left_of(int slot)
{
    return (slot < 0 || slot >= TASK_SLOTS) ? 0 : tasks[slot].left;
}

unsigned long task_runs_of(int slot)
{
    return (slot < 0 || slot >= TASK_SLOTS) ? 0 : tasks[slot].runs;
}

/* 任务栈页的“底”（最低地址）：恒等映射下它就是物理地址。往这里再往下写
 * 一个字就落进 guard 页——阶段七的栈溢出就是靠它触发的。 */
unsigned long task_stack_bottom(void)
{
    return (unsigned long)tasks[cur].stack;
}

void task_user_offer(unsigned int code, unsigned int pages, unsigned int stack)
{
    offer_code = code;
    offer_pages = pages;
    offer_stack = stack;
}

unsigned int task_user_pages_of(int slot)
{
    if (slot < 0 || slot >= TASK_SLOTS)
        return 0;
    if (!tasks[slot].upages)
        return tasks[slot].ustack ? 1 : 0;
    return tasks[slot].upages + (tasks[slot].ustack ? 1 : 0);
}

unsigned long task_esp0_of(int slot)
{
    if (slot < 0 || slot >= TASK_SLOTS)
        return 0;
    return tasks[slot].esp0;
}

/* 这个地址是不是某个任务的 guard 页？是就返回槽位号，否则 -1。
 * 缺页处理用它区分“越界访问”与“栈溢出”：前者停机，后者只杀那个任务。 */
int task_guard_victim(unsigned long addr)
{
    int i;

    for (i = 1; i < TASK_SLOTS; i++) {
        if (!tasks[i].stack)
            continue;
        if (addr >= (unsigned long)tasks[i].guard && addr < (unsigned long)tasks[i].stack)
            return i;
    }
    return -1;
}

/* 在跑的任务里再建一个任务：进临界区，避免入队瞬间被 tick 打断 */
int task_spawn(void (*entry)(void *), void *arg, int prio, unsigned long slice)
{
    return task_spawn_owned(entry, arg, 0, prio, slice);
}

/* 同上，但把 arg 的**所有权**一起交出去：任务死掉（正常退出或被 guard 杀掉）
 * 之后，调度器会调用 dtor(arg, reason) 恰好一次，然后才还栈页。
 *
 * 约定（这就是“参数归谁管”的答案）：
 *   - dtor 为空：arg 由调用方自己管，调度器不碰；
 *   - dtor 非空：从 spawn 成功那一刻起，arg 归任务所有。任务自己不许在退出后
 *     再用它（它已经死了），调用方也不许再用——只等 dtor 收尾；
 *   - dtor 只被调用一次，且是在**换到别的栈上之后**调用的（任务自己的栈已经
 *     是僵尸页），所以 dtor 里可以放心 free 掉跟任务栈无关的东西，但不能
 *     假设自己跑在任务上下文里（IF 由调用点决定，reclaim 里是关中断的）；
 *   - reason 是死因（TASK_DEATH_*）。三条死亡路径都必须先记死因再进
 *     int 0x30，否则 dtor 拿到的是上一个值的残留。 */
int task_spawn_owned(void (*entry)(void *), void *arg, void (*dtor)(void *, int),
                     int prio, unsigned long slice)
{
    int slot;

    cpu_cli();
    slot = task_create(entry, arg, dtor, prio, slice);
    cpu_sti();
    return slot;
}

/* 收尾时把最后一个“僵尸栈”还掉：此刻已经回到 main 自己的栈上，安全 */
void task_reclaim(void)
{
    release_pending();
}

/* dtor 已经跑过几次（自检要一个可数的证据，光“没崩”说明不了它执行了） */
unsigned long task_dtor_calls(void)
{
    return dtor_runs;
}

/* ---- 每个任务的借用表 ---- */

int task_lease_note(unsigned int slot, unsigned int phys)
{
    int i;

    if (slot >= TASK_SLOTS || !phys)
        return 0;                                   /* slot 0（main）不借任务页 */
    for (i = 0; i < TASK_LEASES; i++)
        if (tasks[slot].leases[i] == phys)
            return 1;                               /* 已经记过：同一页第二次借不算新条目 */
    for (i = 0; i < TASK_LEASES; i++) {
        if (!tasks[slot].leases[i]) {
            tasks[slot].leases[i] = phys;
            if ((unsigned int)(i + 1) > lease_peak)
                lease_peak = (unsigned int)(i + 1);
            return 1;
        }
    }
    return 0;                                       /* 表满：明确失败，不挤掉老条目 */
}

int task_lease_forget(unsigned int slot, unsigned int phys)
{
    int i;

    if (slot >= TASK_SLOTS)
        return 0;
    for (i = 0; i < TASK_LEASES; i++) {
        if (tasks[slot].leases[i] == phys) {
            tasks[slot].leases[i] = 0;              /* 只清这一格，不搬动其它条目：
                                                     * 回收循环正在按下标遍历它 */
            return 1;
        }
    }
    return 0;
}

unsigned int task_lease_at(unsigned int slot, int idx)
{
    if (slot >= TASK_SLOTS || idx < 0 || idx >= TASK_LEASES)
        return 0;
    return tasks[slot].leases[idx];
}

unsigned int task_lease_peak(void)
{
    return lease_peak;
}

/* 还有 worker 没结束吗（READY / SLEEPING / WAIT 都算活着，DEAD 已经转成 UNUSED） */
static int any_worker_alive(void)
{
    int i;

    for (i = 1; i < TASK_SLOTS; i++) {
        if (tasks[i].state == TASK_READY || tasks[i].state == TASK_SLEEPING ||
            tasks[i].state == TASK_WAIT)
            return 1;
    }
    return 0;
}

/* 还掉“僵尸”的栈页与 guard 页。必须在**换栈之后**调用：不能在自己还在用的
 * 栈上 page_free。guard 页要先恢复恒等映射再归还，否则下一次 page_alloc 拿到
 * 这一页时它没有虚拟地址，zero_page 会直接缺页。 */
static void release_pending(void)
{
    unsigned int i;

    if (reclaim_guard) {
        page_identity_restore(reclaim_guard, reclaim_guard_pte);
        page_free(reclaim_guard);
        reclaim_guard = 0;
        reclaim_guard_pte = 0;
    }
    if (reclaim_stack) {
        page_free(reclaim_stack);
        reclaim_stack = 0;
    }
    /* 这个任务自己那张“进内核栈”的页：它只是普通页，没有 guard */
    if (reclaim_esp0) {
        page_free(reclaim_esp0);
        reclaim_esp0 = 0;
    }
    /* 用户页归任务：租约是这个任务活着时开上的，所以在这里逐页关掉再还页。
     * 顺序“先关租约再还页”与 exec 的“先要页、再拷、最后放行”正好相反——
     * 那是装的时候，这是拆的时候。 */
    if (reclaim_ucode) {
        for (i = 0; i < reclaim_upages; i++)
            page_lease_close(PAGE_LEASE_OWNER_KERNEL,
                             reclaim_ucode + i * PAGE_SIZE_BYTES);
        if (reclaim_ustack)
            page_lease_close(PAGE_LEASE_OWNER_KERNEL, reclaim_ustack);
        page_free_run(reclaim_ucode, reclaim_upages);
        reclaim_ucode = 0;
        reclaim_upages = 0;
    }
    if (reclaim_ustack) {
        page_free(reclaim_ustack);
        reclaim_ustack = 0;
    }
    /* 参数交接放在最后：栈和 guard 都已经还回去了，dtor 拿到的 arg 一定不再
     * 指向任何“曾经属于这个任务”的内存（除非调用方自己就这么传了，那是它的错）。
     * 只调一次，调完就把指针（连同死因）清空，重复 reclaim 不会重复析构。 */
    if (reclaim_dtor) {
        reclaim_dtor(reclaim_arg, reclaim_reason);
        reclaim_dtor = 0;
        reclaim_arg = 0;
        reclaim_reason = 0;
        dtor_runs++;
    }

    /* 租约回收放在最后一步：这个任务借出去的用户页必须随着它一起收回，
     * 否则“任务死了、页还对 ring3 开着”就是一个永久后门。放在 dtor 之后
     * 是为了让 dtor 还能合法读到自己借的内存（它可能就在那页上）。
     * **借用表要留到这一刻**：它在 TASK_DEAD 分支没被清，正是为了保证
     * “按表回收”这一步还有表可读；收完再在这里清空，槽位复用才安全。 */
    if (reclaim_owner) {
        int k;

        page_lease_release_owner(reclaim_owner);
        for (k = 0; k < TASK_LEASES; k++)
            task_lease_forget(reclaim_owner, task_lease_at(reclaim_owner, k));
        reclaim_owner = 0;
    }
}

/* 一次调度决策：ctx 是刚压栈的现场地址，返回本次要恢复的栈指针。
 * 中断门里 IF=0，不用加锁。tick 与 slice 的递减由 sched_tick 负责。 */
static unsigned long schedule(unsigned long ctx)
{
    int next;

    if (!active)
        return ctx;

    /* 上一轮标记的“僵尸栈”现在可以安全归还：此刻已经不在这张栈上了 */
    release_pending();

    tasks[cur].esp = ctx;
    wake_sleepers();                    /* 只动睡眠队列队首 */

    /* 时间片没用完、也没有更高优先级的任务在等：继续跑，谁也不换。
     * 这是 slice 生效的唯一证据点——否则每个 tick 都换人。 */
    if (cur != 0 && tasks[cur].state == TASK_READY && tasks[cur].left > 0) {
        next = rq_top(0);
        if (next < 0 || tasks[next].prio <= tasks[cur].prio)
            return ctx;
    }

    if (cur != 0) {
        if (tasks[cur].state == TASK_READY) {
            if (tasks[cur].left == 0) {
                /* 一轮时间片用完：从老化提升的级别降回原始优先级 */
                tasks[cur].prio = tasks[cur].base_prio;
                tasks[cur].wait = 0;
            }
            rq_push(cur);               /* 时间片用尽或被高优先级抢占：排回队尾 */
        } else if (tasks[cur].state == TASK_DEAD) {
            reclaim_stack = tasks[cur].stack;   /* 换栈之后再还，不能在自己栈上释放 */
            reclaim_guard = tasks[cur].guard;
            reclaim_guard_pte = tasks[cur].guard_pte;
            reclaim_dtor = tasks[cur].dtor;     /* 参数所有权跟着一起延后交接 */
            reclaim_arg = tasks[cur].arg;
            reclaim_reason = tasks[cur].death;  /* 死因也一起带过去 */
            reclaim_owner = cur;                /* 这个槽位的租约也要一起收 */
            reclaim_esp0 = tasks[cur].esp0_stack;  /* 它那张进内核栈，一起延后还 */
            reclaim_ucode = tasks[cur].ucode;      /* 它名下的用户页，也在这里交接 */
            reclaim_upages = tasks[cur].upages;
            reclaim_ustack = tasks[cur].ustack;
            tasks[cur].state = TASK_UNUSED;     /* 槽位立刻可复用 */
            tasks[cur].stack = 0;
            tasks[cur].guard = 0;
            tasks[cur].guard_pte = 0;
            tasks[cur].esp = 0;
            tasks[cur].esp0_stack = 0;
            tasks[cur].esp0 = 0;
            tasks[cur].ucode = 0;
            tasks[cur].upages = 0;
            tasks[cur].ustack = 0;
            tasks[cur].dtor = 0;
            tasks[cur].arg = 0;
            /* 还键盘归属槽：它名下的字符一并丢掉，槽位可以给下一个任务用 */
            kbd_owner_detach(tasks[cur].kbd_tag);
            tasks[cur].kbd_tag = 0;
            /* 它的 ring3 指针窗口也一起作废（正常情况下任务自己回来时会关掉，
             * 但"被 guard 杀掉 / 被 exit 带走"这两条路走不到那句，所以在这里兜底） */
            sys_user_window_reset(cur);
            /* 这一格要还回池子了：fd 一起收。正常情况下程序自己 close 过，
             * 但"被 guard 杀掉 / 被 exit 带走"两条路走不到那句——不收就是
             * 永久占用一格，而且下一个任务拿到的 fd 号带着旧偏移。 */
            fd_close_all(cur);
        }
    }

    if (!any_worker_alive()) {          /* 全部收工：停表，回 main 收尾 */
        active = 0;
        pgdir_switch(cur, 0);           /* 页目录也回内核那张（to=0 就是它） */
        cur = 0;
        kbd_owner_force(0);             /* 收工回 main：键盘还给共享槽 */
        tss_set_esp0(tasks[0].esp0);    /* 回 main 就换回默认栈 */
        return tasks[0].esp;
    }

    next = rq_top(1);                   /* 最高优先级的队头；队空则落回 main */
    if (next < 0)
        next = 0;

    if (next != cur) {
        /* 换人同时换页目录：把现在生效的那张记到切出者名下，装上进入者的。
         * 这一句的位置很讲究——必须**在 cur 改掉之前**（from 还是老的），
         * 也必须在返回新 esp 之前（asm 一换栈就开始跑新任务了）。 */
        pgdir_switch(cur, next);
        cur = next;
        if (cur != 0) {
            tasks[cur].left = slice_for(cur);   /* 新上场的任务：按当前级别装填时间片 */
            tasks[cur].runs++;
        }
        /* 换人的同时换“键盘归谁”：任务 0（main/内核）回到共享槽 0，
         * 其余任务搬到它创建时领到的 kbd_tag。归属不再是全局单值，
         * 而是**跟着当前上场的是谁**。 */
        kbd_owner_force(cur ? tasks[cur].kbd_tag : 0);
        switches++;
    } else if (cur != 0 && tasks[cur].left == 0) {
        /* 不换人（队里只剩自己）但这一片已经用完了：也得重新装填。
         * 不装填的话 left 会一直是 0，下一个 tick 直接跳进“跑满了”的记账分支，
         * 于是衰减按 tick 而不是按片算——一个独跑的任务会被罚得莫名其妙地快。 */
        tasks[cur].left = slice_for(cur);
    }
    /* TSS.esp0 跟着任务走：从这一刻起，ring3 一旦进门，落的是**当前任务自己**
     * 的那张栈，而不是开机时装进去的全局默认栈。slot 0 的 esp0 就是默认栈顶，
     * 所以“回到内核 main”时它自然又变回去。 */
    tss_set_esp0(tasks[cur].esp0);
    return tasks[cur].esp;
}

/* 时钟中断入口：记账（ticks++、时间片递减、跑满一片就记一笔惩罚），
 * 再跑一遍老化，最后调度 */
unsigned long sched_tick(unsigned long ctx)
{
    /* tick 计数对 main 也要记：slot 0 的空转循环一样占 CPU，不记就成了
     * "谁在跑都看不到"——`tasks` 里 c= 一列的意义正是"这块 CPU 被谁吃了"。
     * 但时间片只在有任务上下文（cur != 0）时递减：main 没有 slice 可扣。 */
    tasks[cur].ticks++;
    if (cur != 0) {
        if (tasks[cur].left > 0 && --tasks[cur].left == 0)
            note_quantum_burn(cur);         /* 这一片真跑满了：历史惩罚记一笔 */
    }
    /* 租约到期检查放在中断里：这是“忘了关的用户页”唯一的兜底。
     * 代价是一次最多 8 槽的线性扫 + 必要时一次 page_mark_kernel（含 invlpg），
     * 在 100Hz 下可以忽略；但要注意它跑在中断上下文，不能在这里睡眠/等锁。 */
    page_lease_tick();
    aging_pass();
    return schedule(ctx);
}

/* 软中断 0x30 入口：只调度，不累 tick、也不在这里扣时间片
 * （要不要扣由发起者决定：task_sleep 睡醒重装、task_yield 直接把 left 清零）。 */
unsigned long task_yield_handler(unsigned long ctx)
{
    vec_hit(IDT_VEC_YIELD);          /* 0x30 被让出过几次（与 sw 的区别：这是入口次数） */
    return schedule(ctx);
}

/* 主动让出：任务仍在就绪态（下一轮该轮到它就轮得到），但把**本轮剩余时间片
 * 全部算完**。这一点很关键：若只是切一下、left 不动，一个每 tick 都 yield 的
 * 任务就能靠不断让出把 slice 永远保住，等于白拿 CPU（“yield 洗时间片”）。
 * 这里直接把 left 置 0，schedule 走的是“时间片用尽”那条分支：降回原始优先级、
 * 排回自己那一级队尾，然后立刻换人。 */
void task_yield(void)
{
    cpu_cli();
    tasks[cur].left = 0;
    tasks[cur].wait = 0;
    tasks[cur].hogs = 0;                /* 自愿让出不是贪心：不给历史惩罚记账 */
    tasks[cur].prio = tasks[cur].base_prio;
    __asm__ volatile ("sti\n\tint $0x30" : : : "memory");
}

void task_sleep(unsigned long ms)
{
    unsigned long ticks_to_wait = (ms * TIMER_HZ) / 1000UL;

    if (ticks_to_wait == 0)
        ticks_to_wait = 1;

    cpu_cli();
    tasks[cur].state = TASK_SLEEPING;
    tasks[cur].wake_tick = timer_ticks() + ticks_to_wait;
    tasks[cur].left = slice_for(cur);           /* 睡醒之后重新给满时间片 */
    tasks[cur].wait = 0;
    tasks[cur].hogs = 0;                        /* 睡了一觉不算贪心 */

    /* 长睡回血：与衰减对称——衰减罚“连续烧”，回血奖“真的等”。
     * 每睡满一个 sched_tune.restore_ticks 就还一级，睡 k 倍就还 k 级（封顶
     * 最高级）。以前是“够阈值就还一级”，于是睡 300ms 和睡 900ms 的回报一样，
     * 惩罚洗不掉的人只能靠多睡几次——现在按倍数折算，睡多久就还多少。
     * 不足一个阈值（k=0）仍然一级都不还，免得好睡一小会儿就把惩罚洗掉。 */
    if (sched_tune.restore_ticks) {
        unsigned long steps = ticks_to_wait / sched_tune.restore_ticks;

        while (steps-- > 0 && tasks[cur].base_prio < TASK_PRIO_LEVELS - 1)
            tasks[cur].base_prio++;
    }

    tasks[cur].prio = tasks[cur].base_prio;     /* 自愿让出也降回原始优先级（含刚还回来的那级） */
    sq_insert(cur);
    /* sti 之后有“下一条指令才认中断”的阴影，紧跟着的 int 0x30 一定先执行，
     * 所以不会出现“刚开中断就被 tick 抢走、醒来又立刻再让出”的空转。 */
    __asm__ volatile ("sti\n\tint $0x30" : : : "memory");
}

/* ---------------------------------------------------------------
 * 等事件：0.11 的 sleep_on / wake_up
 *
 * 与 task_sleep 的区别只在"谁来叫醒"：睡眠堆靠 tick 到期，这里靠通道。
 * 为什么必须把"再查一次条件"放进关中断里：单核不关中断时有个经典窗口
 *   检查条件(假) → 被 tick 抢走 → 对方跑完、置位、wakeup（此刻我还没睡下，
 *   扫不到我）→ 我回来睡下 → 永远没人叫我。
 * 把"检查 + 入睡"塞进同一个 cli 段里，wakeup 只可能发生在入睡之前或之后：
 *   之前 → cond 已是真，直接返回 0；之后 → 扫到我，搬回就绪队列。
 * --------------------------------------------------------------- */
int task_wait_chan(void *chan, int (*cond)(void *ctx), void *ctx)
{
    unsigned long flags;

    if (cur == 0)                       /* main 是空转上下文：睡下去没人叫得醒它 */
        return -1;
    if (!chan)
        return -1;

    flags = cpu_save_flags();
    cpu_cli();
    if (cond && cond(ctx)) {            /* 条件已经成立：不睡，也不记账 */
        cpu_restore_flags(flags);
        return 0;
    }
    tasks[cur].state = TASK_WAIT;
    tasks[cur].chan = chan;
    tasks[cur].left = slice_for(cur);   /* 醒来按新的一轮计时 */
    tasks[cur].wait = 0;
    tasks[cur].hogs = 0;                /* 主动等不算贪心：不记衰减惩罚 */
    tasks[cur].prio = tasks[cur].base_prio;
    n_waits++;
    /* 这里不能 restore flags：要带着"开中断"去 int 0x30，否则切不出去。
     * sti 后紧跟的下一条指令（int）一定先执行，不存在被 tick 抢先的阴影。 */
    __asm__ volatile ("sti\n\tint $0x30" : : : "memory");
    return 1;                           /* 回来时 chan 已由 wakeup 清掉 */
}

int task_wakeup(void *chan)
{
    unsigned long flags;
    int i, n = 0;

    if (!chan)
        return 0;

    flags = cpu_save_flags();
    cpu_cli();
    for (i = 1; i < TASK_SLOTS; i++) {          /* slot 0 从不等待 */
        if (tasks[i].state != TASK_WAIT || tasks[i].chan != chan)
            continue;
        tasks[i].chan = 0;
        tasks[i].state = TASK_READY;
        tasks[i].left = slice_for(i);           /* 醒了就是新的一轮 */
        rq_push(i);
        n_wakeups++;
        n++;
    }
    cpu_restore_flags(flags);
    return n;
}

unsigned int task_waits(void)   { return n_waits; }
unsigned int task_wakeups(void) { return n_wakeups; }

int task_wait_depth(void)
{
    int i, n = 0;

    for (i = 1; i < TASK_SLOTS; i++)
        if (tasks[i].state == TASK_WAIT)
            n++;
    return n;
}

int task_alive(int slot)
{
    if (slot <= 0 || slot >= TASK_SLOTS)
        return 0;
    return tasks[slot].state != TASK_UNUSED;
}

/* 三条死亡路径的唯一出口：先记死因，再标 DEAD，然后借 0x30 让出。
 * 顺序不能反过来——dtor 是在换栈之后才被调的，死因必须在那之前就写进
 * 任务结构，否则交接时读到的是槽位里的残留值。 */
static void task_die(int reason)
{
    cpu_cli();
    /* 页目录先回内核那张。这一条必须在"让出"之前——因为让出之后是调度器
     * 在换栈、跑 dtor、再挑下一个任务，而下一任的 CR3 由 pgdir_switch 装；
     * 若这一步漏了，从 guard/exit 两条路径死掉的槽位会把自己的私有页表
     * "带"给下一任，现象是下一个任务在自己的代码页上 #PF。
     * pgdir_kernel 是幂等的（已经在内核表上就什么也不做）。 */
    pgdir_kernel();
    tasks[cur].death = reason;
    tasks[cur].state = TASK_DEAD;
    __asm__ volatile ("sti\n\tint $0x30" : : : "memory");

    /* 不会回到这里：DEAD 的任务再也不会进就绪队列 */
    for (;;)
        cpu_hlt();
}

void task_exit(void)
{
    task_die(TASK_DEATH_EXIT);
}

/* guard 命中：缺页处理把现场 eip 改成这里，iret 之后任务从这条路径退出。
 * 与 task_exit 的唯一区别就是 dtor 收到的 reason 是“被栈溢出杀掉”。 */
void task_guard_exit(void)
{
    task_die(TASK_DEATH_GUARD);
}

/* 入口函数 return 的落点：死因记成“正常返回”，与显式 task_exit 区分开 */
static void task_return_stub(void *arg)
{
    (void)arg;
    task_die(TASK_DEATH_RETURN);
}
