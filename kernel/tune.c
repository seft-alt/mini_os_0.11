/* 调度参数（运行时可变）+ 两个外部入口：`int 0x31` 与 GDT 调用门。
 *
 * 参数本体是一份可写全局结构（`sched_tune`），原本是 sched/task.c 里的 #define。
 *
 * 入口 1：`int 0x31`（内核自用）
 *     eax = 字段号（TUNE_FIELD_*）
 *     ebx = 新值
 *     返回 eax = 旧值；被拒绝时返回 0
 *
 * 入口 2：386 调用门（GDT 第 3 项，选择子 0x18，DPL=3）
 *     eax = 指向 {field, value} 的指针（ring3 给的指针必须落在用户窗口内）
 *     只放行 TUNE_MASK_GATE 里的字段（时间片 + 回血阈值），老化/衰减不放行，
 *     并且每个放行的字段还各有自己的“可写上限”（见 gate_caps）。
 *
 * 四层校验，缺一层都会留下一个洞：
 *   1) 白名单（mask）：这个入口有没有资格改这个字段；
 *   2) 可写上限（cap）：这个入口能把这个字段改到多大（ring3 ≤ 256，内核 ≤ 10000）；
 *      —— 与“范围”分开：范围管“值荒不荒唐”，上限管“这个入口配不配”；
 *   3) 范围（range）：值本身是不是荒唐（0 值的 slice = 某级任务永远跑不完一片）；
 *   4) 跨字段一致性（order）：改完之后 `slice[0..2]` 必须仍然“低级长、高级短”，
 *      否则高优先级拿着比低优先级还长的片，优先级就被时间片反过来压住了。
 *      这一层是**写完再检查、不一致就回滚**，因为它的约束跨字段、无法预先判。
 * 外加一道不进这四层的“门禁”：ring3 传进来的指针先要过范围/对齐检查，
 * 不合法就连 deref 都不做（那是 #PF 而不是“拒绝”）。
 *
 * 返回值用 0 表示失败是安全的：所有合法值都 ≥ 1，旧值不可能是 0。
 *
 * 单核无锁：只在中断门/调用门里被调用（IF 已清）、或自检里在关中断临界区里调用。 */

#include "linux/idt.h"
#include "linux/kernel.h"
#include "linux/vector.h"
#include "linux/tune.h"
#include "linux/syscall.h"

#define IDT_VEC_TUNE    0x31        /* 软中断号：调度参数系统调用 */

#define TUNE_AGE_TICKS     3
#define TUNE_DECAY_QUANTA  4
#define TUNE_RESTORE_TICKS 30

/* pusha 之后现场的字段位置：ctx[0..7] = edi,esi,ebp,esp,ebx,edx,ecx,eax
 * 再往上是 CPU 压的返回帧：
 *   同一特权级的远调用：ctx[8]=eip, ctx[9]=cs
 *   跨特权级的远调用（ring3 → ring0）：ctx[8]=eip, ctx[9]=cs, ctx[10]=esp, ctx[11]=ss
 *     —— 注意调用门**没有 eflags** 这一格，别拿异常现场的形状去套。 */
#define FRAME_EBX  4
#define FRAME_EAX  7
#define FRAME_EIP  8
#define FRAME_CS   9

/* GDT 第 3 项 = 选择子 0x18：放调用门 */
#define GDT_GATE_INDEX  3
#define GDT_GATE_SEL    (GDT_GATE_INDEX * 8)

/* 请求结构体的"结束标记"（老 ABI 的旁路）：0xFF 不是任何真实 syscall 号，
 * 用户拿它表示"我干完了，别回用户态了"。现在正路是 nr = SYS_NR_EXIT，
 * 0xFF 留着是因为它已经能被 ring3 用、且不需要带退出码。 */
#define TUNE_REQ_EXIT   0xFFUL

struct sched_tune sched_tune;

/* 调用门放行的字段（自检里会临时把这个 mask 改小，验“白名单真的拦得住”） */
unsigned long tune_gate_mask = TUNE_MASK_GATE;

/* 最近一次过门时调用者的 CPL（自检打出来：0x3 才说明这真是 ring3 打进来的）。
 * 单独留一份是因为门处理体本身没有地方“打印”，只能先记账、事后取证。 */
unsigned long tune_gate_last_cpl;

/* head.asm 里的 GDT 本体（8 字节一项，共 256 项）。运行时改门就是改这里。 */
extern unsigned char gdt[];
extern void tune_gate_entry(void);      /* boot/intr.asm 里的门桩 */
extern void user_return_stub(void);     /* boot/intr.asm：门处理体把帧改成这里 */

void sched_tune_defaults(void)
{
    sched_tune.age_ticks = TUNE_AGE_TICKS;
    sched_tune.decay_quanta = TUNE_DECAY_QUANTA;
    sched_tune.restore_ticks = TUNE_RESTORE_TICKS;
    sched_tune.slice[0] = 8;        /* 低优先级：给长片 */
    sched_tune.slice[1] = 4;
    sched_tune.slice[2] = 2;        /* 高优先级：给短片，快速让位 */
}

/* 每个字段允许的**最大可写值**（0 = 这个入口完全不许写这个字段）。
 * 这是“可写上限”，与 tune_range_ok 的“合法区间”不是一回事：
 *   区间是“这个值本身荒不荒唐”（slice=0 会让某级永远跑不完一片，谁都拒）；
 *   上限是“这个入口配多高的权限”（ring3 能改回血阈值，但最多 256 tick；
 *   内核自己走 0x31 就能改到 10000）。
 * 两套都过才算合法：上传限截断的是“权限”，不是“正确性”。 */
#define TUNE_CAP_NONE   0UL

static const unsigned long gate_caps[TUNE_FIELD_COUNT] = {
    TUNE_CAP_NONE,      /* AGE     —— 门的白名单里没有它，上限也就无从谈起 */
    TUNE_CAP_NONE,      /* DECAY   —— 同上 */
    256UL,              /* RESTORE —— ring3 最多把回血单价抬到 256 tick */
    100UL,              /* SLICE0  */
    100UL,              /* SLICE1  */
    100UL               /* SLICE2  */
};

/* 每个字段的合法区间。上界取“宽到够做实验、又不会被写成荒唐值”的量级：
 * 100 tick（1s）的老化/衰减，10000 tick（100s）的回血，1..100 的片长。 */
static int tune_range_ok(unsigned long field, unsigned long value)
{
    switch (field) {
    case TUNE_FIELD_AGE:
    case TUNE_FIELD_DECAY:
        return value >= 1 && value <= 100;
    case TUNE_FIELD_RESTORE:
        return value >= 1 && value <= 10000;
    case TUNE_FIELD_SLICE0:
    case TUNE_FIELD_SLICE1:
    case TUNE_FIELD_SLICE2:
        return value >= 1 && value <= 100;
    default:
        return 0;                   /* 不认识的字段号一律拒绝 */
    }
}

/* 片长必须“低级长、高级短”：slice[i] 是优先级 i 的默认片，i 越大优先级越高。
 * 倒挂（高优先级拿到更长的片）会让优先级被时间片反压，所以写完全套之后要复查。 */
static int tune_slice_order_ok(void)
{
    return sched_tune.slice[0] >= sched_tune.slice[1] &&
           sched_tune.slice[1] >= sched_tune.slice[2];
}

static unsigned long *tune_slot(unsigned long field)
{
    switch (field) {
    case TUNE_FIELD_AGE:     return &sched_tune.age_ticks;
    case TUNE_FIELD_DECAY:   return &sched_tune.decay_quanta;
    case TUNE_FIELD_RESTORE: return &sched_tune.restore_ticks;
    case TUNE_FIELD_SLICE0:  return &sched_tune.slice[0];
    case TUNE_FIELD_SLICE1:  return &sched_tune.slice[1];
    default:                 return &sched_tune.slice[2];
    }
}

/* 两个入口共用的写路径：白名单 → 上限 → 范围 → 写入 → 跨字段复查（不合法就回滚）。
 * cap 传 0 表示“这个入口对该字段没有额外上限”（0x31 用）。
 * 返回旧值；任何一层拒绝都返回 0 且结构保持不变。 */
static unsigned long tune_store(unsigned long field, unsigned long value,
                                unsigned long mask, unsigned long cap)
{
    unsigned long *slot;
    unsigned long old;

    if (field > TUNE_FIELD_SLICE2 || !((1UL << field) & mask))
        return 0;                   /* 字段号不认识，或这个入口无权改 */
    if (cap && value > cap)
        return 0;                   /* 超了本入口的可写上限：拒绝（不是截断） */
    if (!tune_range_ok(field, value))
        return 0;

    slot = tune_slot(field);
    old = *slot;
    *slot = value;                  /* 下一次判定就按新值走，已装填的 left 不回溯 */

    if (!tune_slice_order_ok()) {   /* 倒挂了：撤回，当作没写 */
        *slot = old;
        return 0;
    }
    return old;
}

/* 内核自用：所有字段都放行，不加额外上限 */
unsigned long sched_tune_apply(unsigned long field, unsigned long value)
{
    return tune_store(field, value, TUNE_MASK_ALL, 0);
}

/* 调用门：白名单 + 每字段可写上限 + 同一套范围/一致性校验 */
unsigned long tune_gate_apply(unsigned long field, unsigned long value)
{
    if (field > TUNE_FIELD_SLICE2)
        return 0;
    return tune_store(field, value, tune_gate_mask, gate_caps[field]);
}

/* 门能读到的字段上限（自检要把它打出来：证明上限是表里的数，不是硬编码的） */
unsigned long tune_gate_cap(unsigned long field)
{
    if (field > TUNE_FIELD_SLICE2)
        return 0;
    return gate_caps[field];
}

/* 注册 ring3 可传指针的窗口：只有 [lo, hi) 里的指针才允许 deref。
 * 内核自己（CPL=0）不受这条限制——它本来就能读内核内存。
 * 窗口本体住在 sys/syscall.c（write 也要按同一套规则校验），这里只是转手。 */
void tune_gate_user_window(unsigned long lo, unsigned long hi)
{
    sys_user_window(lo, hi);
}

static unsigned long tune_dispatch(unsigned long ctx,
                                   unsigned long (*apply)(unsigned long,
                                                              unsigned long))
{
    unsigned long *frame = (unsigned long *)ctx;

    frame[FRAME_EAX] = apply(frame[FRAME_EAX], frame[FRAME_EBX]);
    return ctx;
}

/* int 0x31 的处理体：从 pusha 现场里取 eax/ebx，把旧值写回 eax。
 * 不像 0x30 那样换栈——它不调度，只是改几个数。 */
unsigned long tune_syscall(unsigned long ctx)
{
    vec_hit(IDT_VEC_TUNE);           /* 0x31 被调用过几次（门调用不算在这里） */
    return tune_dispatch(ctx, sched_tune_apply);
}

/* ---------------------------------------------------------------
 * 门处理体：ABI 从"号 + 值"两寄存器改成"一个结构体指针"之后，这里只做三件事
 *
 *   1) 校验指针（ring3 才校验，内核自己 lcall 不校验）；
 *   2) 把 {nr,a0,a1,a2} 一次读完（不做两遍解引用），交给 sys_dispatch；
 *   3) 按 dispatch 的返回值决定"写回 eax"还是"改写返回帧回内核"。
 *
 * 分派本身不在这个文件里：号 → 行为的表住在 sys/syscall.c。这里刻意只剩
 * "现场怎么读、帧怎么改"——两者混在一起，改 syscall 的人就得同时懂中断帧。
 *
 * 0xFF 保留原义：老程序用 field=0xFF 表示"我干完了"，等价于 exit(-1)，
 * 但它不带退出码，所以走一条不做记账的旁路（见下面的 TUNE_REQ_EXIT 分支）。
 * --------------------------------------------------------------- */
unsigned long tune_gate_call(unsigned long ctx)
{
    unsigned long *frame = (unsigned long *)ctx;
    unsigned long caller_cs = frame[FRAME_CS];
    unsigned long ptr = frame[FRAME_EAX];
    unsigned long req[SYS_REQ_WORDS];
    unsigned long r;
    unsigned int i;

    frame[FRAME_EAX] = 0;               /* 默认返回 0 = 被拒 */
    tune_gate_last_cpl = caller_cs & 3UL;

    if ((caller_cs & 3UL) == 3UL) {
        /* ring3：指针必须先过“窗口 + 对齐 + 整个结构体不越界”三关。
         * 这条判据住在 sys_ptr_ok 里，因为 write 的参数要走同一套。 */
        if (!sys_ptr_ok(ptr, SYS_REQ_WORDS * sizeof(unsigned long))) {
            sys_note_denied();
            return ctx;
        }
    } else if (!ptr) {
        return ctx;
    }

    /* 读一次就拷走：四个字进局部数组，后面全部用局部值 */
    for (i = 0; i < SYS_REQ_WORDS; i++)
        req[i] = ((volatile unsigned long *)ptr)[i];

    if (req[0] == TUNE_REQ_EXIT) {
        /* 老 ABI 的旁路：不带退出码的"回内核"，不记 syscall 账 */
        frame[FRAME_EIP] = (unsigned long)user_return_stub;
        frame[FRAME_CS] = KERNEL_CS;
        return ctx;
    }

    r = sys_dispatch(req[0], req[1], req[2], req[3]);
    if (r == SYS_DO_EXIT) {
        /* exit：不写 eax，直接改写返回帧——特权级由内核决定去哪儿 */
        frame[FRAME_EIP] = (unsigned long)user_return_stub;
        frame[FRAME_CS] = KERNEL_CS;
        return ctx;
    }

    frame[FRAME_EAX] = r;
    return ctx;
}

/* 门能读到的字段上限（自检要把它打出来：证明上限是表里的数，不是硬编码的） */

/* 在 GDT 第 3 项里摆一个 386 调用门：类型 0x0C、DPL=3、P=1、参数个数 0，
 * 目标是内核代码段（0x08）里的 tune_gate_entry。 */
void sched_tune_gate_init(void)
{
    unsigned long off = (unsigned long)tune_gate_entry;
    unsigned long long desc;            /* m32 下 long 只有 32 位，8 字节描述符得用 long long */
    unsigned int *p = (unsigned int *)(gdt + GDT_GATE_INDEX * 8);

    desc = (unsigned long long)(off & 0xFFFFUL)              /* 目标偏移 15..0 */
         | ((unsigned long long)KERNEL_CS << 16)             /* 目标代码段选择子 0x08 */
         | (0x0CULL << 40)                                   /* 386 调用门，参数个数 0 */
         | (3ULL << 45)                                      /* DPL = 3 */
         | (1ULL << 47)                                      /* P = 1 */
         | ((unsigned long long)(off >> 16) << 48);          /* 目标偏移 31..16 */

    p[0] = (unsigned int)(desc & 0xFFFFFFFFULL);
    p[1] = (unsigned int)(desc >> 32);
}

/* 读回门上真的写着的 DPL：不只是“我们以为写了 3”，而是描述符里确实是 3。
 * 自检拿它当证据（改错一位就会读到 0/1/2）。 */
unsigned long tune_gate_dpl(void)
{
    unsigned long long desc;
    unsigned int *p = (unsigned int *)(gdt + GDT_GATE_INDEX * 8);

    desc = (unsigned long long)p[0] | ((unsigned long long)p[1] << 32);
    return (unsigned long)((desc >> 45) & 3ULL);
}

/* 装默认值 + 挂 0x31 门。main 进门调一次。 */
void sched_tune_init(void)
{
    extern void isr_tune(void);

    sched_tune_defaults();
    idt_set_gate(IDT_VEC_TUNE, isr_tune);
}
/* ==============================================================
 * 两个“从内核改字段”的薄包装（自检用）
 *
 * 为什么不直接写 sched_tune.age_ticks：自检要证明的是“对外接口能改值”，
 * 直接写全局只证明“内核能改自己的变量”。这两条路都留着，才能对照出
 * deny 那条真的是门在拒。
 * ============================================================== */

/* 走 int 0x31（"+a" 让 eax 既是入参也是出参；旧值由 isr_tune 写回现场） */
unsigned long tune_field_set(unsigned long field, unsigned long value)
{
    __asm__ volatile ("int $0x31" : "+a"(field) : "b"(value) : "memory");
    return field;
}

/* 走 GDT 调用门（选择子 0x18，DPL=3）：ABI 是**指针**，eax = 请求结构体地址。
 * 从 CPL0 打进去时内核不校验指针范围——它本来就能读内核内存；
 * 从 ring3 打进去才校验（见 tune_gate_call）。
 * 结构体现在是四个字（nr, a0, a1, a2），tune 只是其中的 nr = 0。 */
unsigned long tune_gate_set(unsigned long field, unsigned long value)
{
    struct sys_req {
        unsigned long nr;
        unsigned long a0;
        unsigned long a1;
        unsigned long a2;
    } req;
    unsigned long ptr;

    req.nr = SYS_NR_TUNE;
    req.a0 = field;
    req.a1 = value;
    req.a2 = 0;
    ptr = (unsigned long)&req;
    __asm__ volatile ("lcall $0x18, $0" : "+a"(ptr) : : "memory");
    return ptr;
}