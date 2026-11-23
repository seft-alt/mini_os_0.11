/* drivers/kbdline.c —— 第 24 行输入行：可编辑、有历史、回车提交
 *
 * 从 keyboard.c 分出来的是“按键怎么变成一行可提交的文本”这件事：
 * 一份真缓冲区 `line_buf`（有光标移动就不能只往屏上追加）、4 条历史、
 * 一圈提交队列，以及**唯一**往第 24 行写字的渲染路径（`echo_render`）。
 *
 * 它不认识扫描码：`kbd_echo()` 收的是已解码的字符，`echo_cmd()` 收的是
 * 已解码的编辑动作（Left/Right/Home/End/Delete/Up/Down）。
 * 第 0 页是证据页：内容只写第 0 页，提示符前缀只画在镜像页（第 1 页）。
 */
#include "linux/kbdline.h"
#include "linux/vga.h"


/* 第 24 行（屏幕最后一行）是空的：把收到的可打印字符回显在那里。
 * 为什么回显在中断里做：它是唯一“有真按键就一定看得见”的位置——
 * 不用等谁来轮询，也不占任何一行已经排满的自检输出；没有按键时
 * 这一行保持空白，所以 verify 的 24 行比对不受影响。
 *
 * 这一行是一条**可编辑的输入行**，不是只增不改的日志：
 *   '\b'（0x0E）删掉光标左边那个字符（后面的字符整体左移一格）；
 *   '\n'（0x1C）整行清空、光标回第 0 列；
 *   写满 79 列还来新字符，就当作“输入行满了”整行清空重来；
 *   `0xE0` 扩展码里的 Left / Right / Home / End / Delete 移动和删除光标处字符。
 * 硬件光标跟着这一行走（vga_cursor_hw_set），所以用户能看见自己打到哪。 */
#define ECHO_ROW     24
#define ECHO_COLS    79
#define ECHO_ADDR    0xB8000UL
#define ECHO_CELLS   (ECHO_ROW * VGA_COLS)

/* 扩展码（0xE0 前缀之后那个码）里我们认的几个编辑键 */
#define ECHO_CMD_NONE   0
#define ECHO_CMD_LEFT   1
#define ECHO_CMD_RIGHT  2
#define ECHO_CMD_HOME   3
#define ECHO_CMD_END    4
#define ECHO_CMD_DEL    5
#define ECHO_CMD_UP     6
#define ECHO_CMD_DOWN   7

/* 输入历史：回车时把非空的行存进来，Up/Down 来回翻。
 * 4 条 × 79 字节 = 316 字节的 BSS，换“能改上一次的命令”，值。
 * 满了就从最旧那条开始整排往下挪——比维护环形下标少一层错位风险。 */
#define ECHO_HIST    4

static volatile unsigned int echo_col;     /* 光标列（不是行尾） */
static volatile unsigned int echo_len;     /* 行内字符数 */
static volatile unsigned int echo_count;   /* 落到屏上的字符总数 */
static volatile unsigned int echo_erase;   /* 退格删掉的字符数 */
static volatile unsigned int echo_newline; /* 回车清行的次数 */
static volatile unsigned int echo_wrap;    /* 撞满 79 列清行重来的次数 */
static volatile unsigned int echo_move;    /* 光标移动次数（左右/Home/End） */

/* 行内容：有了光标移动就不能再“只往屏上追加”，得有一份真缓冲区，
 * 每次都整行重画（79 个字符格，成本可以忽略，逻辑却只有一条路径）。 */
static char line_buf[ECHO_COLS];

/* 历史区：hist_buf[0] 最旧、hist_buf[hist_count-1] 最新；
 * hist_pos = -1 表示“正在编辑一条新行”，0..hist_count-1 表示翻到了第几条。 */
static char hist_buf[ECHO_HIST][ECHO_COLS];
static unsigned char hist_len[ECHO_HIST];
static unsigned int hist_count;
static int hist_pos = -1;
/* 提交队列：回车时把这一行**原样**拷一份进来，等谁来取。
 * 原来只有一格、新的一份直接盖掉还没人取的那份——那等于“谁慢了谁丢行”。
 * 现在是一圈短队列：入队排到队尾、取走从队头出、满了才丢（丢新的那条，
 * 记 `submit_drop`；**旧的一条不会被覆盖**，先来的先被取走，顺序不乱）。
 * 深度同样**运行时可配**（`submit_depth`，默认 4，数组按 `KBD_SUBMIT_MAX` 分配）：
 * “连按几次回车”和“整段多行粘贴”只需要调数字，不用重编。
 * 之所以不做成无限队列：消费者是空闲循环（或以后的一个 shell 任务），
 * 再大的量级应该由消费者自己流控，而不是无限吃内存。 */
#define KBD_SUBMIT_MAX 8             /* 最多排几条（BSS：8 × 79 字节） */
#define KBD_SUBMIT_DEF 4             /* 默认排几条 */

static char submit_q[KBD_SUBMIT_MAX][ECHO_COLS];
static unsigned int submit_qlen[KBD_SUBMIT_MAX];
static unsigned int submit_head;         /* 下一个写入位置 */
static unsigned int submit_tail;         /* 下一个读出位置 */
static unsigned int submit_pending;      /* 队列里还有几条没被取走（0..submit_depth） */
static unsigned int submit_depth = KBD_SUBMIT_DEF;   /* 实际容量（可配） */
static unsigned int submit_count;        /* 回车成功提交了几次 */
static unsigned int submit_drop;         /* 队满导致新提交被丢掉的次数 */

static inline unsigned short *echo_row(void)
{
    return (unsigned short *)ECHO_ADDR + ECHO_CELLS;
}

/* 镜像页：非 0 时把第 24 行**再画一份**到这一页（字节基址）。
 * 为什么要两份：第 0 页是“证据页”（verify 与各类 pmemsave 的基准），
 * 一旦用户开始打字就不再是唯一真相；第 1 页给用户看。输入行两边都得有，
 * 否则切到用户页之后敲键盘什么都看不见。 */
static unsigned long echo_mirror;

static void echo_render(void);          /* 前缀设完要立刻重画一遍 */

/* 提示符前缀（只画在镜像那一页，第 0 页保持“纯输入内容”——所有老测试的断言不受影响）。
 * 长度上限 8：够 `> `、`$ `、`mini> ` 这类，不会吃掉半个输入行。 */
static char echo_prefix[8];
static unsigned int echo_prefix_len;

void kbd_set_echo_mirror(unsigned long base)
{
    echo_mirror = base;
}

unsigned long kbd_echo_mirror(void)
{
    return echo_mirror;
}

void kbd_set_echo_prefix(const char *s, unsigned int len)
{
    unsigned int i;

    if (len > sizeof(echo_prefix))
        len = sizeof(echo_prefix);
    for (i = 0; i < len; i++)
        echo_prefix[i] = s[i];
    echo_prefix_len = len;
    echo_render();
}

unsigned int kbd_echo_prefix_len(void)
{
    return echo_prefix_len;
}

/* 把缓冲区整行画到屏上，并把硬件光标放到 echo_col */
static void echo_render(void)
{
    unsigned int i;
    unsigned short *row = echo_row();

    for (i = 0; i < echo_len; i++)
        row[i] = (unsigned short)((0x0F << 8) | (unsigned char)line_buf[i]);
    for (; i < VGA_COLS; i++)
        row[i] = (unsigned short)((0x0F << 8) | ' ');
    if (echo_mirror) {
        unsigned short *m = (unsigned short *)(echo_mirror) + ECHO_CELLS;
        unsigned int j;

        for (j = 0; j < echo_prefix_len && j < VGA_COLS; j++)
            m[j] = (unsigned short)((0x0F << 8) | (unsigned char)echo_prefix[j]);
        for (j = 0; j < VGA_COLS - echo_prefix_len; j++)
            m[echo_prefix_len + j] = row[j];
        vga_cursor_hw_set(ECHO_CELLS + echo_prefix_len + echo_col);
        return;
    }
    vga_cursor_hw_set(ECHO_CELLS + echo_col);
}

/* 硬件光标：镜像页有前缀时整体右移 prefix_len，否则就是 echo_col */
static void echo_cursor_sync(void)
{
    vga_cursor_hw_set(ECHO_CELLS + (echo_mirror ? echo_prefix_len : 0) + echo_col);
}

/* 对外：把光标交还输入行。输出区打完一条命令之后由 console 调，
 * 这样"输出时光标跟着换行走"和"打字时光标在第 24 行"两件事能接上。 */
void kbd_echo_cursor_to_line(void)
{
    echo_cursor_sync();
}

/* 整行清成空白（属性 0x0F 空格：视觉上是空行，vga-screen.py 会跳过） */
static void echo_clear(void)
{
    echo_len = 0;
    echo_col = 0;
    echo_render();
}

/* 一个可打印字符：写进光标处（覆盖模式），光标右移，行尾跟着长 */
static void echo_put(char c)
{
    hist_pos = -1;                       /* 手动打字：退出历史浏览模式 */
    if (echo_len >= ECHO_COLS) {         /* 行满：整行清空重来，不覆盖也不丢弃 */
        echo_len = 0;
        echo_col = 0;
        echo_wrap++;
    }
    if (echo_col > echo_len)
        echo_col = echo_len;
    line_buf[echo_col] = c;
    echo_col++;
    if (echo_col > echo_len)
        echo_len = echo_col;
    echo_count++;
    echo_render();
}

/* 退格：删掉光标左边那个字符，后面的字符整体左移一格 */
static void echo_back(void)
{
    unsigned int i;

    hist_pos = -1;                       /* 退格也算“回到编辑新行” */
    if (!echo_col) {
        echo_cursor_sync();
        return;
    }
    echo_col--;
    for (i = echo_col; i + 1 < echo_len; i++)
        line_buf[i] = line_buf[i + 1];
    if (echo_len)
        echo_len--;
    line_buf[echo_len] = 0;
    echo_erase++;
    echo_render();
}

/* 回车把当前行存进历史（空行不存），并把“正在编辑新行”重新打开 */
static void hist_push(void)
{
    unsigned int k, i;

    if (!echo_len)
        return;
    if (hist_count >= ECHO_HIST) {            /* 满了：最旧的丢掉，整体前移一格 */
        for (k = 1; k < ECHO_HIST; k++) {
            for (i = 0; i < hist_len[k]; i++)
                hist_buf[k - 1][i] = hist_buf[k][i];
            hist_len[k - 1] = hist_len[k];
        }
        hist_count = ECHO_HIST - 1;
    }
    for (i = 0; i < echo_len; i++)
        hist_buf[hist_count][i] = line_buf[i];
    hist_len[hist_count] = (unsigned char)echo_len;
    hist_count++;
    hist_pos = -1;
}

/* 把历史里的第 nth 条整条搬进编辑行（光标落在行尾） */
static void hist_load(unsigned int nth)
{
    unsigned int i;

    for (i = 0; i < hist_len[nth]; i++)
        line_buf[i] = hist_buf[nth][i];
    echo_len = hist_len[nth];
    line_buf[echo_len] = 0;
    echo_col = echo_len;
}

/* 扩展码里的编辑键：只动光标/删一个字符，不产生字符 */
void echo_cmd(unsigned int cmd)
{
    unsigned int i;

    switch (cmd) {
    case ECHO_CMD_LEFT:
        if (echo_col)
            echo_col--;
        break;
    case ECHO_CMD_RIGHT:
        if (echo_col < echo_len)
            echo_col++;
        break;
    case ECHO_CMD_HOME:
        echo_col = 0;
        break;
    case ECHO_CMD_END:
        echo_col = echo_len;
        break;
    case ECHO_CMD_DEL:                   /* 删光标处那个字符 */
        if (echo_col >= echo_len) {
            echo_cursor_sync();
            return;
        }
        for (i = echo_col; i + 1 < echo_len; i++)
            line_buf[i] = line_buf[i + 1];
        echo_len--;
        line_buf[echo_len] = 0;
        echo_erase++;
        break;
    case ECHO_CMD_UP:                    /* 往上翻历史：先到最新那条，再逐条变旧 */
        if (!hist_count)
            return;
        if (hist_pos < 0)
            hist_pos = (int)hist_count - 1;
        else if (hist_pos > 0)
            hist_pos--;
        hist_load((unsigned int)hist_pos);
        break;
    case ECHO_CMD_DOWN:                  /* 往下翻：翻过最后一条就回到空行 */
        if (hist_pos < 0)
            return;
        hist_pos--;
        if (hist_pos < 0) {
            echo_len = 0;
            echo_col = 0;
        } else {
            hist_load((unsigned int)hist_pos);
        }
        break;
    default:
        return;
    }
    echo_move++;
    echo_render();
}

/* 往提交队列里放一条：队满返回 0（并且记 `submit_drop`），成功返回 1。
 * 回车和“其它生产者”都走这一处，所以排队规则只有一份。 */
static int submit_push(const char *s, unsigned int len)
{
    unsigned int i;

    if (submit_pending >= submit_depth) {
        submit_drop++;                   /* 队满：丢新的这条，队里的旧行保持原样 */
        return 0;
    }
    for (i = 0; i < len; i++)
        submit_q[submit_head][i] = s[i];
    submit_qlen[submit_head] = len;
    submit_head = (submit_head + 1) % KBD_SUBMIT_MAX;
    submit_pending++;
    if (len)
        submit_count++;
    return 1;
}

/* 回车 = 一次提交：入历史、进提交队列、再清行。
 * 为什么“清行”排在最后：清行会把 `echo_len` 归零，而历史与提交都要它。 */
static void line_submit(void)
{
    hist_push();
    (void)submit_push(line_buf, echo_len);
}

/* 一个字符落到第 24 行：可打印就写、'\b' 退格、'\n' 提交 */
void kbd_echo(char c)
{
    if (c == '\b') {
        echo_back();
        return;
    }
    if (c == '\n') {
        line_submit();               /* 先入历史 + 提交槽，再清行 */
        echo_clear();
        echo_newline++;
        return;
    }
    if (c < 0x20 || c > 0x7E) {           /* 制表等其余控制字符：不动屏 */
        echo_cursor_sync();
        return;
    }
    echo_put(c);
}

/* 回显到第 24 行（屏幕最后一行）的字符数；0 = 从来没收到过真按键 */
unsigned int kbd_echo_count(void)
{
    return echo_count;
}

/* 输入行的编辑账：退格擦了几格 / 回车清了几次 / 撞满 79 列重来了几次 */
unsigned int kbd_echo_erase(void)
{
    return echo_erase;
}

unsigned int kbd_echo_newline(void)
{
    return echo_newline;
}

unsigned int kbd_echo_wrap(void)
{
    return echo_wrap;
}

/* 光标被方向键/Home/End 移动了几次（Delete 也算一次“动作”里的删除） */
unsigned int kbd_echo_move(void)
{
    return echo_move;
}

/* 回车提交了几次（空行回车不算） */
unsigned int kbd_line_submit_count(void)
{
    return submit_count;
}

/* 现在有几条提交还没被取走（0..submit_depth） */
unsigned int kbd_line_pending(void)
{
    return submit_pending;
}

/* 提交队列当前的容量（默认 4，可用 `kbd_line_set_capacity()` 改） */
unsigned int kbd_line_capacity(void)
{
    return submit_depth;
}

/* 改提交队列的深度：clamp 到 1..KBD_SUBMIT_MAX，返回**实际生效**的值。
 * 缩小不改动已经排着的那几条（它们还会照顺序被取走），只是从此“再进来就判满”；
 * 所以调深度不会丢行，也不会让消费者读到错位的内容。 */
unsigned int kbd_line_set_capacity(unsigned int depth)
{
    if (depth < 1)
        depth = 1;
    if (depth > KBD_SUBMIT_MAX)
        depth = KBD_SUBMIT_MAX;
    submit_depth = depth;
    return submit_depth;
}

/* 队满导致新提交被丢掉的次数（旧行不会被覆盖，丢的总是新的那条） */
unsigned int kbd_line_drop(void)
{
    return submit_drop;
}

/* 外部生产者入口：把一行交给消费者（回车走的是同一个 `submit_push`）。
 * 返回 1 = 入队成功，0 = 队满被丢（`submit_drop` 已记）。 */
int kbd_line_push(const char *s, unsigned int len)
{
    if (len > ECHO_COLS)
        len = ECHO_COLS;
    return submit_push(s, len);
}

/* 从队头取一条：返回**原始长度**（含被截掉的部分），0 = 队列空。
 * `dst` 为 0 或 `cap` 为 0 时只出队、不拷贝（“丢弃”这条路径）。
 * 取走即出队，队头前移一格——**不会**动到后面排着的那几条。 */
int kbd_line_take(char *dst, unsigned int cap)
{
    unsigned int n, i;

    if (!submit_pending)
        return 0;
    n = submit_qlen[submit_tail];
    submit_pending--;
    if (dst && cap) {
        if (cap > n)
            cap = n;
        for (i = 0; i < cap; i++)
            dst[i] = submit_q[submit_tail][i];
    }
    submit_tail = (submit_tail + 1) % KBD_SUBMIT_MAX;
    return (int)n;
}