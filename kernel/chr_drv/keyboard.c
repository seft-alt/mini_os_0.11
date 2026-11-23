/* drivers/keyboard.c —— PS/2 键盘的硬件面：解码 + IRQ1 + PIC 放行
 *
 * 从 860 行里分出来的：
 *   `drivers/kbd_queue.c` —— 每 owner 一条环形队列（谁能取到哪些字符）
 *   `drivers/kbdline.c`   —— 第 24 行可编辑输入行、历史、提交队列
 *   本文件                —— 扫描码集 1 解码、扩展码状态机、IRQ1 处理体、
 *                            PIC bit1 放行、以及“一次中断把 OBF 取空”的循环
 *
 * 三层的关系是单向的：IRQ1 → `kbd_feed()` 解码 → 字符进队列（`kbd_queue_push`）
 * / 交给输入行（`kbd_echo`）。解码器本身不做回显、不碰显存，所以“队列满时
 * 按键仍然看得见”这条约定是靠调用方（中断处理体）保证的，`last_char` 就是
 * 这条通路上的交接变量。
 */
#include "linux/idt.h"
#include "linux/intr.h"
#include "asm/io.h"
#include "linux/vector.h"
#include "linux/keyboard.h"
#include "linux/kbd_queue.h"
#include "linux/kbdline.h"


#define KBD_DATA      0x60     /* 读：扫描码；写：控制器命令参数 */
#define KBD_STATUS    0x64     /* bit0 = OBF（有数据），bit5 = 鼠标数据 */
#define PIC_MASTER_CMD  0x20
#define PIC_MASTER_MASK 0x21
#define PIC_EOI         0x20

#define SC_LSHIFT     0x2A
#define SC_RSHIFT     0x36
#define SC_ENTER      0x1C
#define SC_BACKSPACE  0x0E
#define SC_EXTENDED   0xE0

/* 集 1 的字符表：只覆盖 0x00..0x39（字母、数字、常用符号、空格、回车、退格）。
 * 0 表示“这个码不出字符”（修饰键、方向键的低位等）。 */
static const char map_normal[0x3A] = {
    [0x01] = 27,  [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4',
    [0x06] = '5', [0x07] = '6', [0x08] = '7', [0x09] = '8', [0x0A] = '9',
    [0x0B] = '0', [0x0C] = '-', [0x0D] = '=', [0x0E] = '\b', [0x0F] = '\t',
    [0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r', [0x14] = 't',
    [0x15] = 'y', [0x16] = 'u', [0x17] = 'i', [0x18] = 'o', [0x19] = 'p',
    [0x1A] = '[', [0x1B] = ']', [0x1C] = '\n',
    [0x1E] = 'a', [0x1F] = 's', [0x20] = 'd', [0x21] = 'f', [0x22] = 'g',
    [0x23] = 'h', [0x24] = 'j', [0x25] = 'k', [0x26] = 'l',
    [0x27] = ';', [0x28] = '\'', [0x29] = '`', [0x2B] = '\\',
    [0x2C] = 'z', [0x2D] = 'x', [0x2E] = 'c', [0x2F] = 'v', [0x30] = 'b',
    [0x31] = 'n', [0x32] = 'm', [0x33] = ',', [0x34] = '.', [0x35] = '/',
    [0x39] = ' ',
};

static const char map_shift[0x3A] = {
    [0x02] = '!', [0x03] = '@', [0x04] = '#', [0x05] = '$', [0x06] = '%',
    [0x07] = '^', [0x08] = '&', [0x09] = '*', [0x0A] = '(', [0x0B] = ')',
    [0x0C] = '_', [0x0D] = '+',
    [0x10] = 'Q', [0x11] = 'W', [0x12] = 'E', [0x13] = 'R', [0x14] = 'T',
    [0x15] = 'Y', [0x16] = 'U', [0x17] = 'I', [0x18] = 'O', [0x19] = 'P',
    [0x1A] = '{', [0x1B] = '}', [0x1C] = '\n',
    [0x1E] = 'A', [0x1F] = 'S', [0x20] = 'D', [0x21] = 'F', [0x22] = 'G',
    [0x23] = 'H', [0x24] = 'J', [0x25] = 'K', [0x26] = 'L',
    [0x27] = ':', [0x28] = '"', [0x29] = '~', [0x2B] = '|',
    [0x2C] = 'Z', [0x2D] = 'X', [0x2E] = 'C', [0x2F] = 'V', [0x30] = 'B',
    [0x31] = 'N', [0x32] = 'M', [0x33] = '<', [0x34] = '>', [0x35] = '?',
    [0x39] = ' ',
};

static volatile unsigned int irq_count;
static volatile unsigned int shift_on;
static volatile char last_char;            /* kbd_feed 解出的字符，0 = 这个码没出字符 */
static volatile unsigned int last_cmd;     /* kbd_feed 解出的编辑动作，0 = 不是编辑键 */
static volatile unsigned int ext_state;    /* 1 = 上一个码是 0xE0，下一个码按扩展码解释 */

/* 扩展码（0xE0 之后那个码）→ 编辑动作。表外的一律 ECHO_CMD_NONE。 */
static unsigned int ext_cmd(unsigned char code)
{
    switch (code) {
    case 0x4B: return ECHO_CMD_LEFT;
    case 0x4D: return ECHO_CMD_RIGHT;
    case 0x47: return ECHO_CMD_HOME;
    case 0x4F: return ECHO_CMD_END;
    case 0x53: return ECHO_CMD_DEL;
    case 0x48: return ECHO_CMD_UP;
    case 0x50: return ECHO_CMD_DOWN;
    default:   return ECHO_CMD_NONE;
    }
}

/* 解码 + 入队。ISR 和自检走的是**同一个函数**，所以自检证明的是真路径。
 *
 * 返回值只说“有没有进队列”；**回显不依赖它**——队列满时字符进不去（`drop_count` 记一笔），
 * 但按键仍然是真按键，第 24 行照样得看见它。所以额外把解码结果留在 `last_char`
 * （字符）和 `last_cmd`（编辑动作）里，由中断处理体取走后回显/执行、再清空。
 *
 * 扩展码用**一个字节的状态**（`ext_state`）而不是“拼成两字节码”：状态只有一个 bit，
 * 中断交错时最坏的结果是“丢掉一个方向键”，不会像拼接方案那样错位成错误的字符。 */
int kbd_feed(unsigned char sc)
{
    char c;

    last_char = 0;
    last_cmd = ECHO_CMD_NONE;
    if (sc == SC_EXTENDED) {             /* 0xE0：下一个码按扩展码解释 */
        ext_state = 1;
        return 0;
    }
    if (sc == 0xE1) {                    /* Pause 序列：整段丢弃，顺手清掉前缀 */
        ext_state = 0;
        return 0;
    }
    if (ext_state) {
        ext_state = 0;
        if (sc & 0x80)                   /* 扩展键的释放码：忽略（不需要清 Shift） */
            return 0;
        last_cmd = ext_cmd((unsigned char)sc);
        return 0;                        /* 编辑动作不是字符，不进队列 */
    }
    if (sc & 0x80) {                     /* 释放码 */
        unsigned char code = (unsigned char)(sc & 0x7F);

        if (code == SC_LSHIFT || code == SC_RSHIFT)
            shift_on = 0;
        return 0;                        /* 释放不入队 */
    }
    if (sc == SC_LSHIFT || sc == SC_RSHIFT) {
        shift_on = 1;
        return 0;
    }
    if (sc >= 0x3A)
        return 0;                        /* 表外：功能键、小键盘等，不产生字符 */
    c = shift_on ? map_shift[sc] : map_normal[sc];
    if (!c)
        return 0;
    last_char = c;                       /* 解码成功：先记下，回显不看成败 */
    return kbd_queue_push(c);            /* 归属与容量检查在 kbd_queue.c */
}

/* 向量 0x21：硬件键盘中断。这里只搬数据，没有任何打印和等待。
 *
 * 一次中断里**把控制器攒下的字节全部取走**（上限 16，防“OBF 永远为 1”的假象）：
 * 8042 的输出缓冲只有一格，按键快过中断响应时后面的字节会等在控制器里，
 * 只读一次就会出现“按了 y 却少一个字符”——这正是分段验证第一版踩到的坑。 */
void irq1_keyboard_handler(void)
{
    unsigned int n;

    irq_count++;
    vec_hit(IDT_VEC_KEYBOARD);       /* 向量 0x21 的命中次数 */
    for (n = 0; n < 16; n++) {
        unsigned char status = inb(KBD_STATUS);
        unsigned char sc;

        if (!(status & 0x01))            /* OBF 清空：这一轮没数据了 */
            break;
        sc = inb(KBD_DATA);
        if (!(status & 0x20)) {
            (void)kbd_feed(sc);          /* 进不进队列都先解码 */
            if (last_char) {
                kbd_echo(last_char);     /* 队列满也要回显：按键是真的 */
                last_char = 0;
            } else if (last_cmd != ECHO_CMD_NONE) {
                echo_cmd(last_cmd);      /* 方向键/Home/End/Delete：只动光标 */
                last_cmd = ECHO_CMD_NONE;
            }
        }
    }
    outb(PIC_MASTER_CMD, PIC_EOI);
}

void keyboard_init(void)
{
    idt_set_gate(IDT_VEC_KEYBOARD, irq1_keyboard);
    /* 只把 bit1 清成 0（IRQ1 放行），其余位保持原样——不要去覆盖时钟那一位 */
    outb(PIC_MASTER_MASK, (unsigned char)(inb(PIC_MASTER_MASK) & ~0x02));
    /* 清掉控制器里可能残留的一字节，避免第一次中断读到上电垃圾 */
    if (inb(KBD_STATUS) & 0x01)
        (void)inb(KBD_DATA);
}

/* 读回 PIC 主片屏蔽字（0xFC 表示 IRQ0/IRQ1 都放行，其余仍屏蔽） */
unsigned int kbd_pic_mask(void)
{
    return (unsigned int)inb(PIC_MASTER_MASK) & 0xFF;
}

unsigned int kbd_irq_count(void)
{
    return irq_count;
}