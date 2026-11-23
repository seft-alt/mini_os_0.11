/* ==============================================================
 * VGA 文本模式（80x25，缓冲区 0xB8000）
 * 直写显存输出，并把逻辑光标同步到 CRTC 光标寄存器（0x3D4/0x3D5）。
 * ============================================================== */
#ifndef MINI_OS_VGA_H
#define MINI_OS_VGA_H

#define VGA_TEXT_BASE   0xB8000
#define VGA_COLS        80
#define VGA_ROWS        25
#define VGA_ATTR_WHITE  0x07
#define VGA_PAGE_BYTES  0x1000   /* 双页间距（字节），> 一屏的 4000 字节 */

/* 让 CRTC 从第 page 页开始显示（0 = 证据页，1 = 用户页）。
 * 只改起始地址寄存器，不动显存：另一页内容原样保留。 */
void vga_page_select(unsigned int page);

/* 读回 CRTC 起始地址（字符格），用于自检双页真的切过去了 */
unsigned int vga_crtc_start(void);

/* 清屏并把光标归零（同时写硬件光标） */
void vga_clear(void);

/* 输出单个字符，支持 '\n' 换行与整屏上滚，结束时同步硬件光标 */
void vga_putc(char c);

/* 以 8 位十六进制打印一个 32 位值（无 libgcc，自己实现） */
void vga_print_hex(unsigned long value);

/* ---- 光标 ---- */

/* 软件记录的当前位置（字符格索引，0..1999） */
unsigned int vga_cursor_position(void);

/* 从 CRTC 寄存器读回硬件光标位置，用于自检 */
unsigned int vga_cursor_hardware(void);

/* 直接把光标搬到指定字符格（越界钳到 1999），不动显存内容 */
void vga_cursor_set(unsigned int pos);

/* 只搬硬件光标（CRTC），不改软件记录的 cursor：
 * 键盘回显把闪烁方块放到第 24 行，printk 的软件位置仍然归它自己管 */
void vga_cursor_hw_set(unsigned int pos);

/* 光标形状：start/end 为扫描线号（0..15），文本模式常用 14/15 做下划线 */
void vga_cursor_shape(unsigned char start, unsigned char end);

/* 读回形状，打包成 0xSSTT（start 在高字节） */
unsigned int vga_cursor_shape_read(void);

/* 通过 CRTC 的 bit5 隐藏/恢复光标 */
void vga_cursor_show(int on);

#endif /* MINI_OS_VGA_H */
