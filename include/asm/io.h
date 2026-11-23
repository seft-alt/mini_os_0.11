/* ==============================================================
 * 端口 I/O 与 CPU 控制指令的最小封装（内联汇编，AT&T 语法）
 * ============================================================== */
#ifndef MINI_OS_IO_H
#define MINI_OS_IO_H

static inline void outb(unsigned short port, unsigned char value)
{
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline unsigned char inb(unsigned short port)
{
    unsigned char value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void outw(unsigned short port, unsigned short value)
{
    __asm__ volatile ("outw %0, %1" : : "a"(value), "Nd"(port));
}

static inline unsigned short inw(unsigned short port)
{
    unsigned short value;
    __asm__ volatile ("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void cpu_cli(void) { __asm__ volatile ("cli"); }
static inline void cpu_sti(void) { __asm__ volatile ("sti"); }
static inline void cpu_hlt(void) { __asm__ volatile ("hlt"); }

/* 关键区用这两个成对，别用裸 cli/sti：调用方可能本来就在关中断上下文里
 * （比如时钟中断里回调进来的路径），无条件 sti 会把调用方的关中断状态弄丢。 */
static inline unsigned long cpu_save_flags(void)
{
    unsigned long f;

    __asm__ volatile ("pushfl\n\tpopl %0" : "=r"(f));
    return f;
}

static inline void cpu_restore_flags(unsigned long f)
{
    __asm__ volatile ("pushl %0\n\tpopfl" : : "r"(f) : "memory", "cc");
}

#endif /* MINI_OS_IO_H */
