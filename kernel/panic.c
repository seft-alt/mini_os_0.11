/* core/panic.c —— 统一出错停机出口（契约见 include/core/panic.h）
 *
 * 这里只做三件事：打一行、关中断、停住。不尝试恢复——"能不能恢复"是调用方
 * 的判断（缺页补页、ring3 越权交回内核都走各自的恢复路径），到了 panic 就
 * 意味着已经判定"继续跑更危险"。
 */
#include "linux/panic.h"
#include "linux/kernel.h"
#include "linux/printk.h"
#include "linux/vga.h"

void panic(const char *why)
{
    /* eip 取调用点（__builtin_return_address 在 -O2 下仍可用）：没有调试符号时，
     * 这个值加上 `objdump -d` 就能定位到是哪一行汇编出的错。 */
    printk("PANIC: %s eip=0x%08X\n",
           why ? why : "(null)",
           (unsigned int)(unsigned long)__builtin_return_address(0));
    /* printk 写的是第 0 页（证据页），而运行期屏幕上显示的是第 1 页（用户页）。
     * 机器要停了，最后一句话必须落在人正看得见的那块地方上：把 CRTC 切回证据页。
     * 不往用户页重复打印，是因为那条路要经过 shell 的滚动缓冲，panic 时不该再
     * 依赖"谁维护 con_row/con_col"这种状态。 */
    vga_page_select(0);
    kernel_halt();                      /* cli + hlt：不再收任何中断 */
    for (;;)                            /* kernel_halt 不返回；这里是编译器的定心丸 */
        ;
}
