/* ==============================================================
 * 内核初始栈
 *
 * head.asm 用 `lss esp, [stack_start]` 一次性加载 SS:ESP，所以该结构
 * 必须严格是「4 字节 esp + 2 字节 ss」的 6 字节布局，不能有多余字段。
 * ============================================================== */
#ifndef MINI_OS_STACK_H
#define MINI_OS_STACK_H

struct stack_start_s {
    unsigned long  esp;
    unsigned short ss;
} __attribute__((packed));

extern struct stack_start_s stack_start __asm__("stack_start");

#endif /* MINI_OS_STACK_H */
