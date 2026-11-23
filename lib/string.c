/* lib/string.c —— 最小 libc 的实现（契约见 include/lib/string.h）
 *
 * 这一份必须用 `-fno-builtin` 编（Makefile 里有单条规则）：
 * 否则 -O2 会把 `memset` 里的字节循环又优化成一次对 `memset` 的调用，
 * 变成自己调自己——栈会一路溢到 guard 页，症状是“清个缓冲区就 triple fault”。
 */
#include "linux/string.h"

void *memcpy(void *dst, const void *src, unsigned int n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;

    while (n--)
        *d++ = *s++;
    return dst;
}

void *memset(void *dst, int value, unsigned int n)
{
    unsigned char *d = (unsigned char *)dst;

    while (n--)
        *d++ = (unsigned char)value;
    return dst;
}

int memcmp(const void *a, const void *b, unsigned int n)
{
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;

    while (n--) {
        if (*x != *y)
            return (int)*x - (int)*y;
        x++;
        y++;
    }
    return 0;
}

unsigned int strlen(const char *s)
{
    unsigned int n = 0;

    while (s[n])
        n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char *a, const char *b, unsigned int n)
{
    while (n && *a && *a == *b) {
        a++;
        b++;
        n--;
    }
    if (!n)
        return 0;
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int atoi(const char *s)
{
    int v = 0, sign = 1;

    while (*s == ' ' || *s == '\t')
        s++;
    if (*s == '-' || *s == '+') {
        if (*s == '-')
            sign = -1;
        s++;
    }
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        s++;
    }
    return v * sign;
}

char *strcpy(char *dst, const char *src)
{
    char *d = dst;

    while ((*d++ = *src++) != '\0')
        ;
    return dst;
}
