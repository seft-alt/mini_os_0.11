/* ==============================================================
 * core/layout.c —— 把结构体布局导出成"数据"，给回归脚本读
 *
 * 为什么要这个文件：tools 下的回归脚本早就学会了「nm 找符号 → 硬编码偏移读字段」
 * （task_t 108 字节、ticks 在 +24、esp0 在 +92）。这些数字散在四个脚本里，
 * 只要结构体动一个字段，四个脚本就会静默读到**错误的地址**——测试仪器坏了，
 * 回归照样打印 PASS。这比代码 bug 更贵：它让"绿"失去意义。
 *
 * 做法：把 offsetof/sizeof 编成一个只读数组，放进独立的 .layout 段。
 * 构建期
 *     objcopy -O binary --only-section=.layout build/kernel.elf build/layout.bin
 * 把它单独剥出来，tools/layout.py 再翻译成 build/layout.env：
 *     TASK_SIZE=108
 *     TASK_OFF_TICKS=24
 * 脚本 source 这个文件。从此这些数字**只有一个来源**（结构体定义本身），
 * 没有算术、没有抄写；改 task_t 只需要重跑 make。
 *
 * 这段代码不参与任何运行逻辑——它只是把编译器的答案存下来。
 * ============================================================== */
#include "linux/taskint.h"

/* 读到的第一个字不是这个值，就说明 .layout 段被链接脚本挪走/合并了，
 * layout.py 会直接报错，而不是拿着错位的数组去生成一堆看似合理的数字。 */
#define LAY_MAGIC 0x4C415941u   /* "LAYA" */

const unsigned int layout_words[] __attribute__((section(".layout"), used)) = {
    LAY_MAGIC,
    sizeof(task_t),
    __builtin_offsetof(task_t, esp),
    __builtin_offsetof(task_t, ticks),
    __builtin_offsetof(task_t, esp0),
    __builtin_offsetof(task_t, esp0_stack),
    __builtin_offsetof(task_t, arg),
    __builtin_offsetof(task_t, dtor),
    __builtin_offsetof(task_t, state),
    __builtin_offsetof(task_t, death),
    __builtin_offsetof(task_t, prio),
    __builtin_offsetof(task_t, base_prio),
    __builtin_offsetof(task_t, slice),
    __builtin_offsetof(task_t, left),
    __builtin_offsetof(task_t, runs),
    __builtin_offsetof(task_t, kbd_tag),
    __builtin_offsetof(task_t, leases),
    __builtin_offsetof(task_t, ucode),
    __builtin_offsetof(task_t, upages),
    __builtin_offsetof(task_t, ustack),
    TASK_SLOTS,
    TASK_LEASES,
    TASK_PRIO_LEVELS,
};
