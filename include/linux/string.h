/* ==============================================================
 * 最小 libc：内核与用户程序共用的几个字节/字符串函数
 *
 * 为什么需要它（而不是每个文件自己写循环）：
 *   1. **gcc 会自己发这些调用**。-O2 下 `for (i=0;i<n;i++) p[i]=0;` 常被优化成
 *      一次 `memset`，结构体整体赋值会被优化成 `memcpy`。我们编的是
 *      `-nostdlib -ffreestanding`，这些符号没人提供，链接就会失败；提供它们
 *      等于把“编译器想用但没人给”的那一块补齐。
 *   2. 内核里现在散着好几处逐字节循环（拷贝、清缓冲、比名字），语义完全一样，
 *      各写一遍就各有一份走样的机会。
 *
 * 约定：不做重叠安全的 memcpy（和 C 标准一致：重叠用 memmove，这里没有）；
 * 长度参数一律 unsigned int（32 位内核里就是 size_t）。
 * ============================================================== */
#ifndef MINI_OS_STRING_H
#define MINI_OS_STRING_H

void *memcpy(void *dst, const void *src, unsigned int n);

/* 清 n 字节。返回 dst，便于链式写法 */
void *memset(void *dst, int value, unsigned int n);

/* 逐字节比：相等返回 0，否则返回第一个不同字节的差（>0 / <0） */
int memcmp(const void *a, const void *b, unsigned int n);

unsigned int strlen(const char *s);

/* 相等返回 0；否则返回第一个不同字符的差 */
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, unsigned int n);

/* 十进制（前导空白跳过、可带一个 '+'/'-'）。解析不到数字返回 0。 */
int atoi(const char *s);

/* 把 src 拷进 dst 并补 NUL（dst 长度 caller 负责，这里不做截断保护） */
char *strcpy(char *dst, const char *src);

#endif /* MINI_OS_STRING_H */
