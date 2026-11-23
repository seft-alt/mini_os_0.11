/* ==============================================================
 * VGA 文本模式输出实现（80x25，显存 0xB8000）
 *
 * 光标的处理是这里最关键的一处细节：显存里的字符位置由软件变量 cursor 决定，
 * 而屏幕上那个闪烁的方块由 CRT 控制器（CRTC）的两个寄存器决定。两者不同步时
 * 就会出现“字写完了、光标还停在 BIOS 输出留下的老地方”的现象。因此每次
 * 写字符/清屏/换行之后都要把 cursor 写回 CRTC 的 0x0E/0x0F（高/低字节）。
 * ============================================================== */
#include "linux/vga.h"
#include "asm/io.h"
#include "linux/printk.h"
#include "linux/stat.h"

#define ATTR_WHITE   VGA_ATTR_WHITE
#define SCREEN_CELLS (VGA_COLS * VGA_ROWS)

#define CRTC_INDEX   0x3D4   /* 索引寄存器：先写它选定目标寄存器 */
#define CRTC_DATA    0x3D5   /* 数据寄存器：随后读写该寄存器的值 */

#define CRTC_CURSOR_HI   0x0E
#define CRTC_CURSOR_LO   0x0F
#define CRTC_START_HI    0x0C   /* 显示起始地址（单位：字符格），双页切换靠这一对 */
#define CRTC_START_LO    0x0D
#define CRTC_CURSOR_SHAPE 0x0A  /* 高 5 位保留（bit5=关闭光标），低 5 位扫描线 */
#define CRTC_CURSOR_SHAPE2 0x0B

static unsigned char *const video = (unsigned char *)VGA_TEXT_BASE;
static unsigned int cursor;          /* 以字符为单位的光标位置 */

/* 当前行已经写了多少列。只为一件事存在：一行正好写满 80 列时，行尾那个 '\n'
 * 会把光标再顶一行——第 25 行触发上滚，banner 被挤出屏幕。编译期有
 * tools/linewidth.py 把格式串算死，这里做运行期兜底：真溢出了就记一笔，
 * `[vga] ... ovf=` 和 shell 的 `stat` 都能看见。键盘回显直接写显存、
 * 不走 vga_putc，所以它不进这个计数（它写的是第 24 行，另有一套约束）。 */
static unsigned int line_len;

/* ---------------- CRTC 光标寄存器 ---------------- */

static void crtc_write(unsigned char index, unsigned char value)
{
    outb(CRTC_INDEX, index);
    outb(CRTC_DATA, value);
}

static unsigned char crtc_read(unsigned char index)
{
    outb(CRTC_INDEX, index);
    return inb(CRTC_DATA);
}

/* 把软件位置写进硬件：0x0E 高字节、0x0F 低字节（共 16 位，够覆盖 2000 格） */
static void cursor_flush(void)
{
    crtc_write(CRTC_CURSOR_HI, (unsigned char)((cursor >> 8) & 0xFF));
    crtc_write(CRTC_CURSOR_LO, (unsigned char)(cursor & 0xFF));
}

/* ---- 双页：CRTC 起始地址 ----
 * 文本模式一屏占 2000 字符格（4000 字节），显存一共 32KB，够放 8 屏。
 * 两屏之间给 0x1000 字节（2048 格）的间距：比一屏多 48 格余量，不会重叠。
 * 切换只改 CRTC 的 0x0C/0x0D，**不动显存**——另一页的内容原样保留，
 * 这正是“证据页不许被用户输出踩掉”的实现方式：pmemsave 读 0xB8000 永远拿第 0 页。 */
void vga_page_select(unsigned int page)
{
    unsigned int start = page * (VGA_PAGE_BYTES / 2);

    crtc_write(CRTC_START_HI, (unsigned char)((start >> 8) & 0xFF));
    crtc_write(CRTC_START_LO, (unsigned char)(start & 0xFF));
}

unsigned int vga_crtc_start(void)
{
    unsigned int hi = crtc_read(CRTC_START_HI);
    unsigned int lo = crtc_read(CRTC_START_LO);

    return (hi << 8) | lo;
}

/* 从硬件读回光标位置，用于自检（不依赖软件变量） */
unsigned int vga_cursor_hardware(void)
{
    unsigned int hi = crtc_read(CRTC_CURSOR_HI);
    unsigned int lo = crtc_read(CRTC_CURSOR_LO);

    return (hi << 8) | lo;
}

unsigned int vga_cursor_position(void)
{
    return cursor;
}

/* 直接把光标搬到某个字符格（越界则钳到合法范围），显存内容不动 */
void vga_cursor_set(unsigned int pos)
{
    if (pos >= SCREEN_CELLS)
        pos = SCREEN_CELLS - 1;
    cursor = pos;
    line_len = 0;                    /* 行内位置被外力改过，列计数从这一格重算 */
    cursor_flush();
}

/* 只搬硬件光标，**不动**软件变量 cursor。
 * 给“输入行”这类不属于 printk 文本流的地方用：键盘回显把光标放到第 24 行，
 * 但 printk 的下一步输出仍然从它自己记住的位置继续——两边互不抢账。 */
void vga_cursor_hw_set(unsigned int pos)
{
    if (pos >= SCREEN_CELLS)
        pos = SCREEN_CELLS - 1;
    crtc_write(CRTC_CURSOR_HI, (unsigned char)((pos >> 8) & 0xFF));
    crtc_write(CRTC_CURSOR_LO, (unsigned char)(pos & 0xFF));
}

/* 光标形状：start/end 是扫描线号（0..15）。文本模式给“下划线”用 14/15。 */
void vga_cursor_shape(unsigned char start, unsigned char end)
{
    crtc_write(CRTC_CURSOR_SHAPE,  (unsigned char)(start & 0x1F));
    crtc_write(CRTC_CURSOR_SHAPE2, (unsigned char)(end   & 0x1F));
}

/* 读回形状，打包成 0xEEND 形式（start 在高字节），便于一次打印 */
unsigned int vga_cursor_shape_read(void)
{
    unsigned int start = crtc_read(CRTC_CURSOR_SHAPE);
    unsigned int end   = crtc_read(CRTC_CURSOR_SHAPE2);

    return ((start & 0x1F) << 8) | (end & 0x1F);
}

/* 隐藏/恢复光标：bit5 是 CRTC 的“关闭光标”位，不必猜扫描线 */
void vga_cursor_show(int on)
{
    unsigned char start = crtc_read(CRTC_CURSOR_SHAPE) & 0x1F;
    unsigned char end   = crtc_read(CRTC_CURSOR_SHAPE2) & 0x1F;

    if (on)
        crtc_write(CRTC_CURSOR_SHAPE, start);          /* 清掉 bit5 */
    else
        crtc_write(CRTC_CURSOR_SHAPE, start | 0x20);   /* 置 bit5 */
    crtc_write(CRTC_CURSOR_SHAPE2, end);
}

/* ---------------- 输出 ---------------- */

/* 光标越过末行时整屏上滚一行 */
static void scroll_if_needed(void)
{
    unsigned int i;
    unsigned int last;

    if (cursor < SCREEN_CELLS)
        return;

    last = VGA_COLS * (VGA_ROWS - 1);
    for (i = 0; i < last; i++) {
        video[i * 2]     = video[(i + VGA_COLS) * 2];
        video[i * 2 + 1] = video[(i + VGA_COLS) * 2 + 1];
    }
    for (i = last; i < SCREEN_CELLS; i++) {
        video[i * 2]     = ' ';
        video[i * 2 + 1] = ATTR_WHITE;
    }
    cursor = last;
}

void vga_clear(void)
{
    unsigned int i;

    for (i = 0; i < SCREEN_CELLS; i++) {
        video[i * 2]     = ' ';
        video[i * 2 + 1] = ATTR_WHITE;
    }
    cursor = 0;
    line_len = 0;
    cursor_flush();                  /* 清屏后光标必须回到左上角 */
}

void vga_putc(char c)
{
    if (c == '\n') {
        /* 换行时清掉本行剩余部分，避免残留 BIOS 输出的文字。
         * 清完后 cursor 恰好落在下一行行首；空行才需要额外前进一行。 */
        if (line_len >= VGA_COLS)
            stat_inc(STAT_OVF);      /* 这行已满 80 列：'\n' 会白顶一行 */
        line_len = 0;
        if (cursor % VGA_COLS != 0) {
            while (cursor % VGA_COLS != 0) {
                video[cursor * 2]     = ' ';
                video[cursor * 2 + 1] = ATTR_WHITE;
                cursor++;
            }
        } else {
            cursor += VGA_COLS;
        }
        scroll_if_needed();
    } else {
        video[cursor * 2]     = (unsigned char)c;
        video[cursor * 2 + 1] = ATTR_WHITE;
        cursor++;
        line_len++;
        scroll_if_needed();
    }

    cursor_flush();                  /* 每字符结束后同步到硬件，滚动后也保持对齐 */
}

void vga_print_hex(unsigned long value)
{
    int shift;

    for (shift = 28; shift >= 0; shift -= 4)
        vga_putc("0123456789ABCDEF"[(value >> shift) & 0xF]);
}
