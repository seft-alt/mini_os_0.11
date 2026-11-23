/* ==============================================================
 * 串口（COM1，115200 8N1）
 *
 * 只做一件事：让 printk 的每一个字节也走一条"屏幕之外"的通道。
 * 为什么值这条通道：之前所有调试都靠抓屏——QEMU 里 dump 显存、
 * 用 vga-screen.py 解码，一条线索要绕两步；有了 COM1，
 * `qemu-system-i386 -serial file:/tmp/ser.log` 就能把整条启动日志落到文件里，
 * grep/sed/比对直接可用，还能在崩溃之后照样留下现场（显存会变，串口不会）。
 *
 * 刻意不做中断接收、波特率可配、流控：现在只需要"能看"，不需要"能收"。
 * ============================================================== */
#ifndef MINI_OS_SERIAL_H
#define MINI_OS_SERIAL_H

#define SERIAL_COM1 0x3F8

/* 初始化（关中断、115200 8N1、开 FIFO）。必须早于第一次 printk。 */
void serial_init(void);

/* 是否初始化成功（串口不存在时 printk 不会卡在等待） */
int serial_ready(void);

/* 同步输出一个字符；未初始化或链路忙（等待超过自旋上限）时直接丢弃 */
void serial_putc(char c);

void serial_puts(const char *s);

#endif /* MINI_OS_SERIAL_H */
