/* ==============================================================
 * 时钟接口视图
 *   8254 通道 0 已被重编程到 100Hz，1 tick = 10ms。
 * ============================================================== */
#ifndef MINI_OS_TIMER_H
#define MINI_OS_TIMER_H

#define TIMER_HZ      100      /* 目标频率（编译期常量，便于静态断言） */
#define TIMER_TICK_MS 10       /* 1 tick 对应的毫秒数 */

/* 重编程 8254 通道 0 到 100Hz，并安装向量 0x20 的中断门 */
void timer_init(void);

/* 已收到的时钟 tick 数（中断里自增，读时 volatile） */
unsigned long timer_ticks(void);

/* 实际频率（tick 数），与 TIMER_HZ 一致 */
unsigned int timer_hz(void);

/* 已编程的分频值（11932），读回校验用 */
unsigned short timer_pit_divisor(void);

/* 锁存并读回通道 0 的当前计数值（必然 < 分频值） */
unsigned short timer_pit_counter(void);

/* tick 数换算成毫秒（100Hz 下就是 ticks*10），32 位分段计算不溢出 */
unsigned long timer_uptime_ms(void);

/* 打开 8259 主片的 IRQ0（其余仍屏蔽） */
void pic_unmask_irq0(void);

/* 由 boot/intr.asm 的 irq0_timer 桩调用：ctx 是压栈现场地址，
 * 返回本次要恢复的栈指针（调度器借此换栈，不切换时原样返回） */
unsigned long irq0_timer_handler(unsigned long ctx);

#endif /* MINI_OS_TIMER_H */
