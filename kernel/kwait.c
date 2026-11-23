/* kernel/kwait.c —— 等事件（sleep_on / wake_up）的可运行自检，见 linux/kwait.h */
#include "linux/kwait.h"
#include "linux/task.h"
#include "linux/timer.h"
#include "asm/io.h"

#define KW_ROUNDS 16

struct kw_box;

struct kw_side {
    struct kw_box *b;
    unsigned int want;          /* 我这端等到第几轮了 */
};

struct kw_box {
    void *c_data;               /* reader 等这个：writer 摆好数据 */
    void *c_ack;                /* writer 等这个：reader 收下并确认 */
    unsigned int seq;           /* writer 已经写完第几轮 */
    unsigned int ack;           /* reader 已经确认到第几轮 */
    unsigned int data;          /* 本轮的数据 */
    unsigned int ok;            /* 数据顺序没出错 = 1 */
    unsigned int abort;         /* 超时兜底：让两端都退出循环 */
    unsigned int rd_done;
    unsigned int wr_done;
};

static struct kw_box kw;
static struct kw_side kw_rd, kw_wr;

/* 上一趟的证据快照（跑完再读，避免命令层直接摸 box 的中间态） */
static unsigned int ev_rounds, ev_ok, ev_timeout, ev_waits, ev_wakeups;

/* cond 里也要认 abort：否则超时兜底叫醒了任务，它一看条件还不成立就又睡回去 */
static int kw_has_data(void *p)
{
    struct kw_side *s = (struct kw_side *)p;

    return s->b->abort || s->b->seq > s->want;
}

static int kw_has_ack(void *p)
{
    struct kw_side *s = (struct kw_side *)p;

    return s->b->abort || s->b->ack > s->want;
}

static void kw_reader(void *arg)
{
    struct kw_side *s = (struct kw_side *)arg;
    struct kw_box *b = s->b;
    unsigned int i;

    for (i = 0; i < KW_ROUNDS && !b->abort; i++) {
        s->want = i;
        while (!kw_has_data(s))
            if (task_wait_chan(b->c_data, kw_has_data, s) < 0)
                goto out;
        if (b->abort)
            break;
        if (b->data != i * 7u + 3u)     /* 顺序错一个字节就整趟算失败 */
            b->ok = 0;
        b->ack = i + 1;                 /* 先确认再叫对方，顺序不能反 */
        task_wakeup(b->c_ack);
    }
out:
    b->rd_done = 1;
}

static void kw_writer(void *arg)
{
    struct kw_side *s = (struct kw_side *)arg;
    struct kw_box *b = s->b;
    unsigned int i;

    for (i = 0; i < KW_ROUNDS && !b->abort; i++) {
        b->data = i * 7u + 3u;
        b->seq = i + 1;                 /* 先摆数据和序号，再叫 reader */
        task_wakeup(b->c_data);
        s->want = i;
        while (!kw_has_ack(s))
            if (task_wait_chan(b->c_ack, kw_has_ack, s) < 0)
                goto out;
    }
out:
    b->wr_done = 1;
    task_wakeup(b->c_data);             /* 收尾多叫一次：万一 reader 还在等 */
}

int kwait_selftest(void)
{
    unsigned int t0, w0, k0;
    int slot_r, slot_w, guard = 0;

    /* 复位信箱（静态对象，上一趟的残留不能带过来） */
    kw.c_data = &kw.seq;
    kw.c_ack = &kw.ack;
    kw.seq = 0;
    kw.ack = 0;
    kw.data = 0;
    kw.ok = 1;
    kw.abort = 0;
    kw.rd_done = 0;
    kw.wr_done = 0;
    kw_rd.b = &kw;
    kw_wr.b = &kw;
    kw_rd.want = 0;
    kw_wr.want = 0;
    ev_rounds = ev_ok = ev_timeout = 0;

    w0 = task_waits();
    k0 = task_wakeups();

    task_sched_on();                    /* 自检收尾时它是关的，这里显式打开 */

    /* 先起 writer（它一醒就会摆第 1 轮并 wakeup；那时 reader 还没建，
     * 所以 reader 醒来后看到 seq 已经是真，直接通过 cond 不睡——不死锁） */
    slot_w = task_spawn(kw_writer, &kw_wr, 1, 0);
    if (!slot_w) {
        ev_timeout = 1;
        return 0;
    }
    slot_r = task_spawn(kw_reader, &kw_rd, 1, 0);
    if (!slot_r) {
        kw.abort = 1;
        task_wakeup(kw.c_ack);
        while (task_wait_depth() > 0 || guard < 400) {
            task_reclaim();
            cpu_hlt();
            guard++;
        }
        ev_timeout = 1;
        return 0;
    }

    /* 等两端都收工。hlt 一次醒一次（tick 100Hz），顺带把僵尸栈收掉 */
    t0 = timer_ticks();
    guard = 0;
    while (!(kw.rd_done && kw.wr_done)) {
        task_reclaim();
        cpu_hlt();
        if (timer_ticks() - t0 > 200 && !kw.abort) {   /* 2 秒：兜底，不算正常路径 */
            kw.abort = 1;
            kw.ok = 0;
            task_wakeup(kw.c_data);
            task_wakeup(kw.c_ack);
        }
        if (++guard > 800)               /* 再兜一层：abort 之后还不退就不等了 */
            break;
    }
    ev_timeout = kw.abort;

    /* 把残留的等待与僵尸收干净：任务死了要 schedule 走一趟才转 UNUSED */
    guard = 0;
    while ((task_wait_depth() > 0 || task_alive(slot_r) || task_alive(slot_w)) &&
           guard++ < 400) {
        if (task_wait_depth() > 0) {
            task_wakeup(kw.c_data);
            task_wakeup(kw.c_ack);
        }
        task_reclaim();
        cpu_hlt();
    }

    ev_rounds = kw.ack;                 /* reader 确认到第几轮就是真跑完的轮数 */
    ev_ok = (kw.ok && !kw.abort && kw.rd_done && kw.wr_done && ev_rounds == KW_ROUNDS);
    ev_waits = task_waits() - w0;
    ev_wakeups = task_wakeups() - k0;
    return (int)ev_ok;
}

unsigned int kwait_rounds(void)   { return ev_rounds; }
unsigned int kwait_ok(void)       { return ev_ok; }
unsigned int kwait_timeout(void)  { return ev_timeout; }
unsigned int kwait_waits(void)    { return ev_waits; }
unsigned int kwait_wakeups(void)  { return ev_wakeups; }

int kwait_left(void)
{
    return task_wait_depth();
}
