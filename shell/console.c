/* shell/console.c —— 4 行内存滚动缓冲 + VGA 第 1 页（用户页）控制台。
 *
 * 走 VGA 双页：第 0 页永远是证据页，CRTC 起始地址切到第 1 页后用户看到的
 * 就是这 24 行（第 24 行留给输入行，键盘会镜像过去）。
 * 本文件是 shell 里**唯一**直接写 0xB9000 显存的地方。
 */
#include "linux/console.h"
#include "linux/state.h"
#include "linux/vga.h"
#include "linux/keyboard.h"

#define CON_BASE   ((unsigned long)VGA_TEXT_BASE + VGA_PAGE_BYTES)
#define CON_ROWS   SHELL_CON_ROWS
#define CON_COLS   SHELL_CON_COLS
#define CON_ATTR   VGA_ATTR_WHITE

static char out_buf[SHELL_OUT_LINES][SHELL_OUT_COLS];
static unsigned int out_len[SHELL_OUT_LINES];
static unsigned int out_head;                /* 下一条写哪 */
static unsigned int out_count;               /* 0..SHELL_OUT_LINES，给证据接口用 */
static unsigned int out_total;               /* 累计写过多少行（单调，不封顶）
                                              * 只用来算"这一趟新增了几行"——
                                              * 用 out_count 算会在缓冲写满后恒为 0，
                                              * 表现就是"第 9 行起屏幕再也不动" */

/* ---- 控制台状态（只有 shell_con_open() 之后才动显存第 1 页） ---- */
static unsigned int con_open;
static unsigned int con_row;       /* 0..CON_ROWS-1 */
static unsigned int con_col;
static unsigned int con_chars;     /* 累计写给用户页的字符数（证据） */
static unsigned int con_scrolls;   /* 累计上滚次数（证据） */
static unsigned int con_clear_req; /* `clear`：下一行输出前先抹掉用户页 */
static unsigned int con_cursor_cell; /* 最近一次写进 CRTC 的字符格（用户页内偏移） */

static unsigned short *con_cell(unsigned int row, unsigned int col)
{
    return (unsigned short *)(CON_BASE) + row * VGA_COLS + col;
}

/* 把硬件光标搬到"当前输出位置"。
 * 之前用户页只在输入行（第 24 行）动过 CRTC 光标，输出区写多少字光标都不动，
 * 看上去像卡在输入行上——这里让光标跟着 con_row/con_col 走：
 * 每写一个字符右移一格，换行时落到下一行行首，上滚后停在最后一行。
 * 注意 vga_cursor_hw_set() 只写 CRTC、不动软件 cursor，所以第 0 页的 printk
 * 记账完全不受影响；CRTC 的光标号是相对起始地址（第 1 页）的偏移，正好对齐。
 * 打字时 echo_render() 会把光标抢到第 24 行，两边各管各的一段时间，互不打架。 */
static void con_cursor_sync(void)
{
    if (!con_open)
        return;
    con_cursor_cell = con_row * VGA_COLS + con_col;
    vga_cursor_hw_set(con_cursor_cell);
}

static void con_clear_all(void)
{
    unsigned int r, c;

    for (r = 0; r < CON_ROWS; r++)
        for (c = 0; c < VGA_COLS; c++)
            con_cell(r, c)[0] = (unsigned short)((CON_ATTR << 8) | ' ');
}

/* 上滚一行：1..23 搬到 0..22，最后一行清空 */
static void con_scroll(void)
{
    unsigned int r, c;

    for (r = 0; r < CON_ROWS - 1; r++)
        for (c = 0; c < VGA_COLS; c++)
            con_cell(r, c)[0] = con_cell(r + 1, c)[0];
    for (c = 0; c < VGA_COLS; c++)
        con_cell(CON_ROWS - 1, c)[0] = (unsigned short)((CON_ATTR << 8) | ' ');
    con_scrolls++;
}

void shell_con_newline(void)
{
    con_col = 0;
    con_row++;
    if (con_row >= CON_ROWS) {
        con_scroll();
        con_row = CON_ROWS - 1;
    }
    con_cursor_sync();                   /* 光标跟着落到下一行行首 */
}

/* 写一串字符：'\n' 换行、超 79 列折行、满 24 行上滚。 */
void shell_con_puts(const char *s, unsigned int n)
{
    unsigned int i;

    for (i = 0; i < n; i++) {
        char c = s[i];

        if (c == '\n') {
            shell_con_newline();
            continue;
        }
        if (con_col >= CON_COLS)
            shell_con_newline();
        con_cell(con_row, con_col)[0] = (unsigned short)((CON_ATTR << 8) | (unsigned char)c);
        con_col++;
        con_chars++;
        con_cursor_sync();               /* 光标跟着字符走（行内右移 / 折行） */
    }
}

void shell_con_clear_next(void)
{
    con_clear_req = 1;
}

void shell_out_puts(const char *s, unsigned int n)
{
    unsigned int i;

    if (n > SHELL_OUT_COLS)
        n = SHELL_OUT_COLS;
    for (i = 0; i < n; i++)
        out_buf[out_head][i] = s[i];
    out_len[out_head] = n;
    out_head = (out_head + 1) % SHELL_OUT_LINES;
    if (out_count < SHELL_OUT_LINES)
        out_count++;
    out_total++;
}

void shell_out_reset(void)
{
    unsigned int i;

    for (i = 0; i < SHELL_OUT_LINES; i++)
        out_len[i] = 0;
    out_head = 0;
    out_count = 0;
    out_total = 0;
}

/* 交出去的 count 是**单调累计游标** out_total，不是缓冲里的行数：
 * 缓冲只有 8 格，写满之后 out_count 恒为 8，拿它做差永远是 0 —— 这正是
 * 「连发命令后第 9 行起屏幕不动/最后一行不见了」的根因。 */
void shell_con_mark(unsigned int *head, unsigned int *count)
{
    *head = out_head;
    *count = out_total;
}

/* 这一行新产生的输出行（head0/total0 是执行前的快照）搬到用户页。
 * 新增超过 SHELL_OUT_LINES 行时只搬**最近** 8 行（前面的已经被环形缓冲覆盖），
 * 用 skip 跳过丢掉的那几行，否则搬到的是 8 格里的旧内容。 */
void shell_con_report(unsigned int head0, unsigned int total0)
{
    unsigned int j, n, skip = 0;

    if (!con_open)
        return;
    if (con_clear_req) {
        con_clear_req = 0;
        con_clear_all();
        con_row = 0;
        con_col = 0;
        con_cursor_sync();               /* 清屏后光标回左上角 */
        return;
    }
    n = out_total - total0;
    if (n > SHELL_OUT_LINES) {
        skip = n - SHELL_OUT_LINES;
        n = SHELL_OUT_LINES;
    }
    for (j = 0; j < n; j++) {
        unsigned int idx = (head0 + skip + j) % SHELL_OUT_LINES;

        shell_con_puts(out_buf[idx], out_len[idx]);
        shell_con_newline();
    }
    if (n == 0)
        shell_con_newline();                 /* 没输出也占一行，别让命令粘在一起 */
    /* 输出结束了：光标从"最后一行末尾"交还给第 24 行的输入位置。
     * 输出期间它是一路跟着文字走的（con_cursor_sync），这里只是收尾。 */
    kbd_echo_cursor_to_line();
}

/* 开用户页：清掉第 1 页、写横幅、把 CRTC 切过去、输入行镜像过去。
 * 只在自检全部打印完之后调用（main 末尾），这样 verify 的第 0 页一个字节都不动。 */
void shell_con_open(void)
{
    static const char *const banner =
        "mini-os shell 0.2  (help echo ver clear hist set unset stat irq tasks)";

    con_clear_all();
    con_row = 0;
    con_col = 0;
    con_chars = 0;
    con_scrolls = 0;
    con_clear_req = 0;
    out_head = 0;
    out_count = 0;                        /* 自检那几行的输出不搬到用户页 */
    out_total = 0;

    shell_con_puts(banner, (unsigned int)shell_str_len(banner, SHELL_OUT_COLS));
    shell_con_newline();

    con_open = 1;
    kbd_set_echo_mirror(CON_BASE);        /* 输入行也画到用户页第 24 行 */
    kbd_set_echo_prefix("> ", 2);         /* 用户页的输入行带提示符 */
    con_cursor_cell = CON_ROWS * VGA_COLS + 2;   /* 开口时先在提示符后面闪 */
    vga_cursor_hw_set(con_cursor_cell);
    vga_page_select(1);
}

/* ---- 证据接口 ---- */
unsigned int shell_console_on(void)     { return con_open; }
unsigned int shell_console_row(void)    { return con_row; }
unsigned int shell_console_col(void)    { return con_col; }
unsigned int shell_console_cursor(void) { return con_cursor_cell; }
unsigned int shell_console_chars(void)  { return con_chars; }
unsigned int shell_console_scrolls(void){ return con_scrolls; }

unsigned int shell_out_count(void) { return out_count; }
unsigned int shell_out_len(unsigned int idx)
{
    if (idx >= out_count)
        return 0;
    return out_len[(out_head + SHELL_OUT_LINES - 1 - idx) % SHELL_OUT_LINES];
}
const char *shell_out_line(unsigned int idx)
{
    if (idx >= out_count)
        return 0;
    return out_buf[(out_head + SHELL_OUT_LINES - 1 - idx) % SHELL_OUT_LINES];
}
