/* ==============================================================
 * printk：最小内核打印接口（带一个极简格式化器）
 *
 * 支持的转换（够自检界面用，刻意不做完整 printf）：
 *   %s        字符串（NULL 打成 "(null)"）
 *   %c        单字符（int）
 *   %d        有符号十进制（int）
 *   %u        无符号十进制（unsigned）
 *   %x / %X   十六进制，最少位数，十六进制位不补零
 *   %0Nx/%0NX 定宽十六进制，位数不足补零（N 是 1..9，例如 %08X = vga_print_hex）
 *
 * 为什么只用“显式定宽”的十六进制：屏幕每行最多 79 列，宽度必须能从格式串
 * 一眼算出来。tools/linewidth.py 就是按这些定宽符算行宽的，含 `%X` 的行
 * 会被算成 8 位（最坏情况），所以给界面写的格式串请一律写成 `%0NX`。
 *
 * head.asm 的 ignore_int 也会以 cdecl 方式调用它，因此签名与链接符号名固定。
 * MinGW/GCC 在 COFF 下会加前导下划线，这里用 asm label 钉死。
 * ============================================================== */
#ifndef MINI_OS_PRINTK_H
#define MINI_OS_PRINTK_H

int printk(const char *fmt, ...) __asm__("printk");

/* 只写串口 + 日志环，不碰显存（启动阶段留痕用；24 行自检屏不能被它挤动） */
int printk_dev(const char *fmt, ...) __asm__("printk_dev");

#endif /* MINI_OS_PRINTK_H */
