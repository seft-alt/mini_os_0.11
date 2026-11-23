/* ==============================================================
 * IDT 门安装
 *
 * IDT 本体（256 项）在 boot/head.asm 的 .text 里，符号 idt 已导出；head 的
 * setup_idt 已把每项填成指向 ignore_int 的 32 位中断门并 lidt 加载过。这里
 * 只原地改写某一项，然后重载一次 IDTR（基址没变，重载保证描述符与内存一致）。
 * ============================================================== */
#include "linux/idt.h"
#include "linux/kernel.h"

/* head.asm 导出：256 个 8 字节中断门 */
extern unsigned char idt[];

struct idtr_s {
    unsigned short limit;
    unsigned int   base;
} __attribute__((packed));

static struct idtr_s idtr;

void idt_set_gate(int vector, void (*handler)(void))
{
    volatile unsigned char *gate;
    unsigned int addr = (unsigned int)handler;

    if (vector < 0 || vector >= IDT_ENTRIES)
        return;

    gate = (volatile unsigned char *)(idt + vector * 8);

    gate[0] = (unsigned char)(addr & 0xFF);         /* 偏移 0..7   */
    gate[1] = (unsigned char)((addr >> 8) & 0xFF);  /* 偏移 8..15  */
    gate[2] = (unsigned char)(KERNEL_CS & 0xFF);    /* 选择子 0..7 */
    gate[3] = (unsigned char)((KERNEL_CS >> 8) & 0xFF);
    gate[4] = 0;                                    /* 保留        */
    gate[5] = 0x8E;                                 /* P=1 DPL=0 32位中断门 */
    gate[6] = (unsigned char)((addr >> 16) & 0xFF); /* 偏移 16..23 */
    gate[7] = (unsigned char)((addr >> 24) & 0xFF); /* 偏移 24..31 */

    idtr.limit = IDT_ENTRIES * 8 - 1;
    idtr.base  = (unsigned int)idt;
    __asm__ volatile ("lidt %0" : : "m"(idtr));
}
