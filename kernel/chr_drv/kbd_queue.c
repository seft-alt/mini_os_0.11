/* drivers/kbd_queue.c —— 每 owner 一条环形队列（按键归属 + 隔离）
 *
 * 从 keyboard.c 分出来的是“字符进了哪条环、谁能取走”这件事：
 * `queue[t]` 是槽 t 独占的一圈格子，head/tail/count 各自一套，一个槽写满
 * 不会让别人的下标跟着动。归属（`owner_active`）在入队这一刻决定进哪条环，
 * 所以调度器 `kbd_owner_force()` 一改，后续按键立刻换人，已排着的旧字符
 * 仍留在原主人那条环里。
 *
 * 本文件不认扫描码、不知道屏幕在哪：它只接收“一个已经解出来的字符”。
 */
#include "linux/kbd_queue.h"


/* 一个字符进哪条环由入队那一刻的归属决定；满只丢自己这条，别的一条不受影响。
 * 深度运行时可配：数组按最大格数分配，实际用几格看 `lane_depth[t]`（默认 4）。 */
#define KBD_OWNERS     4             /* 0 = 共享/内核，1..3 = attach 出来的槽 */
#define KBD_LANE_MAX   8             /* 每条环最多几格（BSS：4 条 × 8 字节） */
#define KBD_LANE_DEF   4             /* 默认几格：够挡“打字比消费者快”的抖动 */

/* 四条环：`queue[t]` 是槽 t 独占的那一条，`q_head[t]`/`q_tail[t]`/`q_count[t]`
 * 是它自己的读写位置与长度——彼此没有共享的下标，所以一个槽写满也不会让
 * 另一个槽的下标跟着动（这正是旧方案做不到的事）。
 * 数组按 `KBD_LANE_MAX` 分配（最大格数），实际用几格看 `lane_depth[t]`：
 * 调小等于“更快地拒收”，调大等于“容忍更长的突发”，读写下标都按 depth 取模。
 * `lane_drop[t]` 记“这条环因为满丢掉的字符数”，`lane_peak[t]` 记历史最高水位：
 * 两个数一起看，才知道“丢”是真满还是别处有 bug。 */
static volatile char queue[KBD_OWNERS][KBD_LANE_MAX];
static volatile unsigned int q_head[KBD_OWNERS];    /* 下一个写入位置 */
static volatile unsigned int q_tail[KBD_OWNERS];    /* 下一个读出位置 */
static volatile unsigned int q_count[KBD_OWNERS];
static volatile unsigned int lane_depth[KBD_OWNERS] = {
    KBD_LANE_DEF, KBD_LANE_DEF, KBD_LANE_DEF, KBD_LANE_DEF
};
static volatile unsigned int lane_drop[KBD_OWNERS];
static volatile unsigned int lane_peak[KBD_OWNERS];

/* 队列归属：槽 0 保留给共享/内核（老接口 `kbd_getchar()` 就是读槽 0），
 * 1..KBD_OWNERS-1 由 `kbd_owner_attach()` 发放——谁 attach 到哪个槽，
 * 就只能取到进到那个槽那条环里的字符，别人再调也抢不走。 */
static volatile unsigned int owner_mask;    /* bit n = 槽 n 已被 attach */
static volatile unsigned int owner_active;  /* 当前按键归属（默认 0 = 共享） */

/* 归属切换的账（证据不在屏幕上，`tools/kbd-task.sh` 用 xp 读）：
 *   owner_switches —— 归属真的变了的次数（调度每次换人都会试一次）
 *   owner_seen     —— bit n = 槽 n 曾经真的当过“按键主人”
 *   owner_last     —— 最后一次生效的归属（收工后应回到 0） */
static volatile unsigned int owner_switches;
static volatile unsigned int owner_seen;
static volatile unsigned int owner_last;
static volatile unsigned int drop_count;

unsigned int kbd_queue_count(void)
{
    return q_count[0];                   /* 老接口 = 共享那条环 */
}

/* 四条环加起来的字符数（“队列有没有还干净”要看这个，不是只看共享槽） */
unsigned int kbd_queue_total(void)
{
    return q_count[0] + q_count[1] + q_count[2] + q_count[3];
}

/* 槽 t 那条环因为满丢掉的字符数 / 历史最高水位 */
unsigned int kbd_lane_drop(int tag)
{
    if (tag < 0 || (unsigned int)tag >= KBD_OWNERS)
        return 0;
    return lane_drop[tag];
}

unsigned int kbd_lane_peak(int tag)
{
    if (tag < 0 || (unsigned int)tag >= KBD_OWNERS)
        return 0;
    return lane_peak[tag];
}

/* 槽 t 那条环现在用几格（默认 KBD_LANE_DEF） */
unsigned int kbd_lane_depth(int tag)
{
    if (tag < 0 || (unsigned int)tag >= KBD_OWNERS)
        return 0;
    return lane_depth[tag];
}

/* 改槽 t 那条环的深度：clamp 到 1..KBD_LANE_MAX，返回**实际生效**的值。
 * 调小不动已经排着的字符（它们照原顺序被取走，只是从此“再进来就判满”）；
 * 调小到比 q_count 还小时，下一次取字符按新深度取模——所以某条环要缩容，
 * 应该先让它被取空（这也是“调深度不丢行”的前提，写在这里免得踩）。 */
unsigned int kbd_lane_set_depth(int tag, unsigned int depth)
{
    if (tag < 0 || (unsigned int)tag >= KBD_OWNERS)
        return 0;
    if (depth < 1)
        depth = 1;
    if (depth > KBD_LANE_MAX)
        depth = KBD_LANE_MAX;
    if (depth < q_count[tag])
        q_head[tag] = q_tail[tag];       /* 缩到装不下：读写位置对齐，从空开始算 */
    lane_depth[tag] = depth;
    return lane_depth[tag];
}

unsigned int kbd_dropped(void)
{
    return drop_count;
}

/* 清掉槽 tag 那条环：head/tail/count 一起归零。
 * 环形队列“整条清空”不需要动数据，把读写位置重叠就等价于空了——
 * 旧方案要“扫一遍重建”，是因为一条环里混着好几个人的字符；现在一条环只有一个人。 */
static void lane_clear(unsigned int tag)
{
    q_head[tag] = 0;
    q_tail[tag] = 0;
    q_count[tag] = 0;
}

/* 领一个 owner 槽：返回 1..KBD_OWNERS-1，槽满返回 0（0 也是保留给共享的那个号，
 * 所以“返回 0”天然等于“失败”，调用者不需要第二个旗标）。 */
int kbd_owner_attach(void)
{
    unsigned int t;

    for (t = 1; t < KBD_OWNERS; t++) {
        if (!(owner_mask & (1u << t))) {
            owner_mask |= 1u << t;
            return (int)t;
        }
    }
    return 0;
}

/* 还槽：顺手把这条队列里属于它的字符全部丢掉（不能让死掉的消费者留下幽灵字符），
 * 如果它正好是“当前按键归属”，归属退回共享槽 0。 */
void kbd_owner_detach(int tag)
{
    unsigned int t = (unsigned int)tag;

    if (t == 0 || t >= KBD_OWNERS)
        return;
    owner_mask &= ~(1u << t);
    lane_clear(t);
    if (owner_active == t)
        owner_active = 0;
}

/* 把按键归属切给某个 owner；切到 0 = 回到共享。没 attach 的槽不许切（返回 -1）。 */
int kbd_owner_select(int tag)
{
    unsigned int t = (unsigned int)tag;

    if (t != 0 && !(owner_mask & (1u << t)))
        return -1;
    if (t >= KBD_OWNERS)
        return -1;
    owner_active = t;
    return 0;
}

/* 调度器专用：换人时把归属直接搬到新任务的标签上，不做 attach 校验
 * （标签是任务创建时领到的，调度器只是“替它拿回键盘”）。
 * 过程是“先搬账再改值”，所以 owner_seen 一定包含曾经真的当过主人的槽。 */
void kbd_owner_force(int tag)
{
    unsigned int t = (unsigned int)tag;

    if (t >= KBD_OWNERS)
        t = 0;                       /* 越界标签当作共享，不让调度器把队列写坏 */
    owner_last = t;
    if (owner_active == t)
        return;                      /* 没变就不记账：换人但归属相同是很常见的 */
    owner_active = t;
    owner_seen |= 1u << t;
    owner_switches++;
}

/* 归属切换的账 */
unsigned int kbd_owner_switches(void)
{
    return owner_switches;
}

unsigned int kbd_owner_seen(void)
{
    return owner_seen;
}

unsigned int kbd_owner_last(void)
{
    return owner_last;
}

/* 当前按键归属给谁（0 = 共享） */
int kbd_owner_active(void)
{
    return (int)owner_active;
}

/* 只取“属于 tag”的字符：从它自己那条环的队头取一个，其它环一格都不动。
 * 没有属于它的字符就返回 -1——**不会顺手拿别人的**，这就是隔离。
 * 旧方案是“在一条大环里找第一个标签吻合的再整段前移”，现在是 O(1) 的一步。 */
int kbd_getchar_t(int tag)
{
    char c;

    if (tag < 0 || (unsigned int)tag >= KBD_OWNERS)
        return -1;
    if (!q_count[tag])
        return -1;                       /* 无属于它的字符：不阻塞、不等待 */
    c = queue[tag][q_tail[tag]];
    q_tail[tag] = (q_tail[tag] + 1) % lane_depth[tag];
    q_count[tag]--;
    return (int)c;
}

/* 老接口：读共享槽 0。保留它就是为了“不 attach 的调用者行为完全不变”。 */
int kbd_getchar(void)
{
    return kbd_getchar_t(0);
}

/* 槽 tag 那条环里还有几个字符（调试/自检用） */
unsigned int kbd_queue_count_t(int tag)
{
    if (tag < 0 || (unsigned int)tag >= KBD_OWNERS)
        return 0;
    return q_count[tag];
}

/* 入队入口：把解码出来的字符放进“当前归属”那条环。
 * 返回 1 = 进队，0 = 这条环满（`drop_count` 与 `lane_drop[]` 各记一笔）。
 * 这个函数原来内联在 `kbd_feed()` 里，提出来是为了让“解码”和“归属”分开测。 */
int kbd_queue_push(char c)
{
    unsigned int lane = owner_active;     /* 进哪条环，由入队这一刻的归属决定 */
    unsigned int depth = lane_depth[lane];

    if (q_count[lane] >= depth) {         /* 这条环满：只丢自己这条的，别人的不受影响 */
        drop_count++;
        lane_drop[lane]++;
        return 0;
    }
    queue[lane][q_head[lane]] = c;
    q_head[lane] = (q_head[lane] + 1) % depth;
    q_count[lane]++;
    if (q_count[lane] > lane_peak[lane])
        lane_peak[lane] = q_count[lane];  /* 到过 depth 就说明“真被塞满过” */
    return 1;
}
