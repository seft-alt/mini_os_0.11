/* ==============================================================
 * 内核公共定义：段选择子、栈尺寸等全局常量
 * ============================================================== */
#ifndef MINI_OS_KERNEL_H
#define MINI_OS_KERNEL_H

/* 与 boot/setup.asm、boot/head.asm 的 GDT 严格对应 */
#define KERNEL_CS            0x08    /* 内核代码段 */
#define KERNEL_DS            0x10    /* 内核数据段 */
#define USER_CS              0x23    /* GDT 第 4 项 | RPL3：ring3 代码段 */
#define USER_DS              0x2B    /* GDT 第 5 项 | RPL3：ring3 数据段 */
#define KERNEL_TSS           0x30    /* GDT 第 6 项：32 位 TSS（ltr 用） */

#define KERNEL_STACK_SIZE    4096

/* 停机入口（定义在 init/main.c，单独成符号便于调试器下断点）：
 * kernel_halt = 出错停机（cli + hlt，不再收中断）；
 * kernel_idle = 正常收尾的空闲循环（sti + hlt，IRQ 照样进来，键盘靠它工作） */
void kernel_halt(void) __asm__("kernel_halt");
void kernel_idle(void) __asm__("kernel_idle");

#endif /* MINI_OS_KERNEL_H */
