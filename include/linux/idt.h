/* ==============================================================
 * 中断描述符表（IDT）接口
 *
 * IDT 本体由 head.asm 定义并已由 setup_idt 填充兜底门（ignore_int）。
 * C 侧只做“把某个向量改指向真实入口桩”，基址不变，故不需要重建表。
 * ============================================================== */
#ifndef MINI_OS_IDT_H
#define MINI_OS_IDT_H

#define IDT_ENTRIES       256

#define IDT_VEC_DIVIDE    0      /* CPU 异常：除零（无错误码） */
#define IDT_VEC_GP        13     /* CPU 异常：通用保护（错误码=选择子） */
#define IDT_VEC_PAGE_FAULT 14    /* CPU 异常：缺页（错误码=P/W/U 位，CR2 存地址） */
#define IDT_VEC_TIMER     0x20   /* IRQ0：8254 时钟（setup.asm 已重映射 PIC） */
#define IDT_VEC_KEYBOARD  0x21   /* IRQ1：PS/2 键盘（同一片 PIC，重映射后紧挨时钟） */
#define IDT_VEC_YIELD     0x30   /* 软中断：任务主动让出（PIC 重映射后 0x20-0x2F 之外，安全） */

/* 把向量 vector 的门改指向 handler（32 位中断门，DPL=0，选择子内核代码段） */
void idt_set_gate(int vector, void (*handler)(void));

#endif /* MINI_OS_IDT_H */
