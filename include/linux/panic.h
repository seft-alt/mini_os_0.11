/* ==============================================================
 * panic / ASSERT：到达不了的状态，只走一条出口
 *
 * 以前"出错停机"这件事散在两处：`arch/exception.c` 自己 printk 三行再
 * 调 kernel_halt()；别处出错就只能静默 return（看起来像"什么都没发生"）。
 * 这一层给它们一个共同的名字和共同的现场：
 *
 *   PANIC: <原因> eip=0x<调用点>
 *
 * 三个字符通道（VGA / COM1 / 日志环）都由 printk 负责，所以串口那份日志
 * 里也会有——机器停在 QEMU 里的时候，这是唯一还拿得到的东西。
 *
 * ASSERT 与 panic 的区别只在"谁该负责"：ASSERT 是"这里不可能"，
 * 触发就是代码写错了；panic 也可以由设备自检直接调，表示"继续跑更危险"。
 * ============================================================== */
#ifndef MINI_OS_PANIC_H
#define MINI_OS_PANIC_H

/* 打印一行现场，然后 cli + hlt 死循环（不返回）。why 传 NULL 打成 "(null)"。 */
void panic(const char *why) __attribute__((noreturn));

/* ``*ASSERT(cond, why)``：cond 为假就 panic(why)。
 * 写成宏而不是函数，是为了让 eip 落在**调用点**而不是 panic 里。 */
#define ASSERT(cond, why)                     \
    do {                                      \
        if (!(cond))                          \
            panic(why);                       \
    } while (0)

#endif /* MINI_OS_PANIC_H */
