/* ==============================================================
 * 中断入口桩声明（定义在 boot/intr.asm）
 *
 * 符号名不带下划线，与 NASM 侧的 GLOBAL 名一致。
 * ============================================================== */
#ifndef MINI_OS_INTR_H
#define MINI_OS_INTR_H

void isr_divide_error(void);
void isr_general_protection(void);
void isr_page_fault(void);
void irq0_timer(void);
void irq1_keyboard(void);
void isr_yield(void);

#endif /* MINI_OS_INTR_H */
