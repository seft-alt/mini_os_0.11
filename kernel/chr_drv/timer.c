/* ==============================================================
 * 8254 定时器（PIT）与时钟中断
 *
 * BIOS 留下的频率约 18.2Hz，tick 无法换算成时间。这里把通道 0 重编程成
 * 100Hz（方式 2 速率发生器），于是 1 tick = 10ms，tick 数量可以直接当毫秒基准。
 *
 * 用方式 2 而不是方式 3（方波）：方式 2 每计满一次就发一个脉冲，节拍语义最直白，
 * 也是 Linux 等内核的选择；方式 3 是占空比 50% 的方波，中断取决于输出上升沿，
 * 在模拟器里的节拍不如方式 2 稳定。
 *
 * 8254 的输入时钟固定 1.193182MHz，分频值 = 输入频率 / 目标准频率：
 *   1193182 / 100 = 11931.82  ->  取 11932，实际 99.9985Hz，误差 15ppm。
 *
 * 注意时间换算不能用 64 位除法：没有 libgcc 就取不到 __udivdi3，
 * 所以 ms 用 (ticks/hz)*1000 + (ticks%hz)*1000/hz 分段算，全程 32 位。
 * ============================================================== */
#include "linux/idt.h"
#include "linux/intr.h"
#include "asm/io.h"
#include "linux/kernel.h"
#include "linux/vector.h"
#include "linux/printk.h"
#include "linux/task.h"
#include "linux/timer.h"
#include "linux/vga.h"

#define PIT_CH0           0x40
#define PIT_CMD           0x43
#define PIT_INPUT_HZ      1193182UL
#define PIT_TARGET_HZ     100
#define PIT_DIVISOR       ((unsigned short)(PIT_INPUT_HZ / PIT_TARGET_HZ + 1))

#define PIT_CMD_CH0_LOHI_MODE2  0x34   /* 通道0 | 先低后高字节 | 方式2 速率发生器 | 二进制 */
#define PIT_CMD_LATCH           0x00   /* 锁存当前计数值，随后可读两字节 */

#define PIC_MASTER_CMD    0x20
#define PIC_MASTER_MASK   0x21
#define PIC_EOI           0x20

static volatile unsigned long ticks;

/* 编译期校验：分频值乘回目标频率必须还原输入时钟（误差不超过一个目标周期）。
 * 改 PIT_TARGET_HZ 时若把除法写错（例如多加/少加 1 变成了另一个频率），这里直接编译失败。 */
typedef char pit_divisor_selfcheck[
    (PIT_DIVISOR * PIT_TARGET_HZ <= PIT_INPUT_HZ + PIT_TARGET_HZ &&
     PIT_DIVISOR * PIT_TARGET_HZ >= PIT_INPUT_HZ - PIT_TARGET_HZ) ? 1 : -1];

/* 向量 0x20：每次时钟中断进入这里（由 boot/intr.asm 的 irq0_timer 桩调用）。
 * 100Hz 下每秒 100 次，不能在里面打印——会把屏幕刷穿、也拖长中断。
 * 返回值是桩要恢复的栈指针：不切换时原样返回，切换时是另一个任务的现场。 */
unsigned long irq0_timer_handler(unsigned long ctx)
{
    outb(PIC_MASTER_CMD, PIC_EOI);   /* 先回 EOI，允许后续中断 */
    ticks++;
    vec_hit(IDT_VEC_TIMER);          /* 向量 0x20 的命中次数（shell 的 irq 命令读它） */
    return sched_tick(ctx);
}

void timer_init(void)
{
    /* 重编程通道 0：命令字节 -> 分频低字节 -> 分频高字节 */
    outb(PIT_CMD, PIT_CMD_CH0_LOHI_MODE2);
    outb(PIT_CH0, (unsigned char)(PIT_DIVISOR & 0xFF));
    outb(PIT_CH0, (unsigned char)((PIT_DIVISOR >> 8) & 0xFF));

    idt_set_gate(IDT_VEC_TIMER, irq0_timer);
}

unsigned long timer_ticks(void)
{
    return ticks;
}

unsigned int timer_hz(void)
{
    return PIT_TARGET_HZ;
}

unsigned short timer_pit_divisor(void)
{
    return PIT_DIVISOR;
}

/* 锁存并读回通道 0 当前计数值，用于确认重编程真的生效（值必然 < 分频值） */
unsigned short timer_pit_counter(void)
{
    unsigned char lo;
    unsigned char hi;

    outb(PIT_CMD, PIT_CMD_LATCH);
    lo = inb(PIT_CH0);
    hi = inb(PIT_CH0);
    return (unsigned short)((hi << 8) | lo);
}

/* tick -> 毫秒：分段算，避免 ticks*1000 溢出，也避免 64 位除法 */
unsigned long timer_uptime_ms(void)
{
    unsigned long t = ticks;

    return (t / PIT_TARGET_HZ) * 1000UL
         + ((t % PIT_TARGET_HZ) * 1000UL) / PIT_TARGET_HZ;
}

void pic_unmask_irq0(void)
{
    outb(PIC_MASTER_MASK, 0xFE);     /* 只放开 bit0（IRQ0），其余继续屏蔽 */
}
