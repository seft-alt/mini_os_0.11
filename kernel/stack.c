/* ==============================================================
 * 内核初始栈与 SS:ESP 装载结构
 * ============================================================== */
#include "linux/kernel.h"
#include "linux/stack.h"

static unsigned char kernel_stack[KERNEL_STACK_SIZE] __attribute__((aligned(16)));

struct stack_start_s stack_start = {
    (unsigned long)(kernel_stack + KERNEL_STACK_SIZE),
    KERNEL_DS
};
