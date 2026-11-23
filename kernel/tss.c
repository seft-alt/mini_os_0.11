/* ==============================================================
 * 32 位 TSS + ring3 段：让“特权级切换”这件事真的能发生
 *
 * 只有调用门的 DPL=3 是不够的。`lcall` 从 ring3 进 ring0 时，CPU 要换栈——
 * 新栈从哪来？答案只有一个：**当前任务的 TSS**（`ss0:esp0`）。没有 ltr 过
 * 的 TSS，这一步直接 `#GP(0)`，门再合法也进不来。
 *
 * 三件事一起做：
 *   1) GDT 第 6 项摆一个 32 位 TSS 描述符（类型 0x89、DPL=0、界限 0x67）；
 *   2) 填 TSS 本体：ss0 = 内核数据段、esp0 = 一张专用内核栈的栈顶、
 *      iomap_base = 结构大小（等于“没有 I/O 位图”，于是 ring3 的 in/out 全被拦）；
 *   3) `ltr` 装载。之后 `str` 能读回 0x30——这是“TSS 真的挂上了”的证据。
 *
 * 同一张表里再摆两项 ring3 用的段（GDT 第 4/5 项 = 0x23/0x2B），都是 4GB 平坦，
 * 只是 DPL=3。它们和内核段用同一段物理地址空间，区别只在“谁能用”。
 *
 * 注意：本内核**不做硬件任务切换**（不 `ljmp $0x30:$0`），TSS 只被当成
 * “特权级切换时要用的那一对 ss0:esp0”来用，其余字段留 0。
 * ============================================================== */
#include "linux/tss.h"
#include "linux/kernel.h"
#include "asm/io.h"
#include "linux/pgdir.h"

/* i386 32 位 TSS 的精确布局：一共 104 字节，最后两字节是 I/O 位图基址。
 * 用 packed 结构而不是手写偏移，是因为“偏移错一个字段”在 #GP 里根本看不出来。 */
struct tss32 {
    unsigned int prev_tss;
    unsigned int esp0;
    unsigned int ss0;
    unsigned int esp1;
    unsigned int ss1;
    unsigned int esp2;
    unsigned int ss2;
    unsigned int cr3;
    unsigned int eip;
    unsigned int eflags;
    unsigned int eax, ecx, edx, ebx;
    unsigned int esp, ebp, esi, edi;
    unsigned int es, cs, ss, ds, fs, gs;
    unsigned int ldt;
    unsigned short trap;
    unsigned short iomap_base;
} __attribute__((packed));

#define TSS_BYTES   ((unsigned long)sizeof(struct tss32))   /* 104 = 0x68 */

/* 专门的“进内核栈”：从 ring3 进 ring0 时 CPU 就切到这上面。
 * 16 字节对齐，避免 SSE/结构体对齐方面的意外。 */
static unsigned char tss_stack[4096] __attribute__((aligned(16)));

static struct tss32 tss;

/* head.asm 的 GDT 本体（256 项 × 8 字节），运行时往里写描述符 */
extern unsigned char gdt[];

#define GDT_USER_CODE_INDEX 4
#define GDT_USER_DATA_INDEX 5
#define GDT_TSS_INDEX       6

/* 把一个 32 位描述符按“限长/基址/访问位/标志半字节”拼成 8 字节。
 * m32 下 long 只有 32 位，8 字节描述符必须用 unsigned long long。
 *
 * 注意 gran 传的是**四个标志位**（G|D/B|L|AVL）而不是“粒度字节”：早期这里
 * 传过 0xCF（把限长高位 0xF 也带进来了），结果 0xCF<<52 的 0xF 溢出到
 * bits 56-59，被 CPU 当成 base_hi 读出 0x0C —— `cs=0x23` 看着完全正常，
 * 但段基址悄悄变成 0x0C000000，取指就跑到 0x0C013000 上去了。限长高位由
 * 下面的 `limit >> 16` 单独填，别混进 flags。 */
static void gdt_write(unsigned int index, unsigned long base, unsigned long limit,
                      unsigned char access, unsigned char flags)
{
    unsigned long long desc;
    unsigned int *p = (unsigned int *)(gdt + index * 8);

    desc = (unsigned long long)(limit & 0xFFFFUL)
         | ((unsigned long long)(base & 0xFFFFFFUL) << 16)
         | ((unsigned long long)access << 40)
         | ((unsigned long long)((limit >> 16) & 0xFUL) << 48)
         | ((unsigned long long)((base >> 24) & 0xFFUL) << 56)
         | ((unsigned long long)(flags & 0xFUL) << 52);

    p[0] = (unsigned int)(desc & 0xFFFFFFFFULL);
    p[1] = (unsigned int)(desc >> 32);
}

void tss_init(void)
{
    unsigned long esp0 = (unsigned long)&tss_stack[sizeof(tss_stack)];

    /* 1) TSS 本体：只填“特权级切换要用到的那一对”，其余清零 */
    tss.prev_tss = 0;
    tss.esp0 = (unsigned int)esp0;
    tss.ss0 = KERNEL_DS;                /* 0x10：内核数据段 */
    tss.esp1 = tss.ss1 = 0;
    tss.esp2 = tss.ss2 = 0;
    /* 这一格只在硬件任务切换（task gate）时被 CPU 装载。本内核不用 task gate，
     * 所以它没有人读；但"没人读"不等于可以乱写——让它在别处写着"每个任务有
     * 自己的页目录"的时候停在 0，将来补上 task gate 就会用内核表跑用户任务。
     * 初始值 = 0 = 内核页目录（PGDIR_KERNEL），之后由 pgdir_load 每次换表同步。 */
    tss.cr3 = PGDIR_KERNEL;
    tss.eip = 0;
    tss.eflags = 0;
    tss.eax = tss.ecx = tss.edx = tss.ebx = 0;
    tss.esp = tss.ebp = tss.esi = tss.edi = 0;
    tss.es = tss.cs = tss.ss = tss.ds = tss.fs = tss.gs = 0;
    tss.ldt = 0;
    tss.trap = 0;
    /* I/O 位图基址 = 结构大小 → “这张表里没有位图”，ring3 的任何 in/out
     * 都会因为超出 TSS 界限而 #GP。给了位图就等于把端口也给了出去。 */
    tss.iomap_base = (unsigned short)TSS_BYTES;

    /* 2) ring3 的代码/数据段：4GB 平坦，只有 DPL 是 3。
     *    代码 0xFA = P|DPL3|S|可执行|可读；数据 0xF2 = P|DPL3|S|可写。
     *    标志位 0xC = G(页粒度)+D/B(32 位)，限长 0xFFFFF×4KB = 4GB。 */
    gdt_write(GDT_USER_CODE_INDEX, 0, 0xFFFFFUL, 0xFA, 0xC);
    gdt_write(GDT_USER_DATA_INDEX, 0, 0xFFFFFUL, 0xF2, 0xC);

    /* 3) TSS 描述符：类型 0x89（可用 32 位 TSS）、DPL=0、字节粒度、界限 0x67 */
    gdt_write(GDT_TSS_INDEX, (unsigned long)&tss, TSS_BYTES - 1, 0x89, 0x0);

    /* 4) ltr 装载。装载之后 TSS 描述符的 busy 位会被 CPU 置上（类型变 0x8B） */
    __asm__ volatile ("ltr %0" : : "rm"((unsigned short)KERNEL_TSS));
}

/* I/O 位图基址的读数（自检用）：0x68 = 没有位图 = ring3 一律被拒 */
unsigned long tss_iomap_base(void)
{
    return (unsigned long)tss.iomap_base;
}

/* 换“进内核栈”的栈顶：每次从 ring0 进 ring3 之前都要换成一个干净的栈顶，
 * 否则上一次留在栈上的残料会被当成返回帧。 */
void tss_set_esp0(unsigned long esp0)
{
    tss.esp0 = (unsigned int)esp0;
}

unsigned long tss_esp0(void)
{
    return (unsigned long)tss.esp0;
}

/* 内核默认栈的栈顶：只有 main（slot 0）会用它，worker 用的是各自的任务页 */
unsigned long tss_default_esp0(void)
{
    return (unsigned long)&tss_stack[sizeof(tss_stack)];
}

/* TSS.cr3 跟着"CPU 真正在用的那张表"走。写在 pgdir_load（唯一换表处）里，
 * 而不是散在各调用点：少一个调用点，就多一处可漂移的账。 */
void tss_set_cr3(unsigned int cr3)
{
    tss.cr3 = cr3;
}

unsigned int tss_cr3(void)
{
    return tss.cr3;
}

/* TR 寄存器里真的装了什么（ltr 之后应为 0x30） */
unsigned long tss_tr(void)
{
    unsigned short tr;

    __asm__ volatile ("str %0" : "=rm"(tr));
    return (unsigned long)tr;
}

/* 从 GDT 描述符里读回 TSS 的基址/类型/界限，验证“写进去的就是这一份” */
unsigned long tss_gdt_base(void)
{
    unsigned int *p = (unsigned int *)(gdt + GDT_TSS_INDEX * 8);
    unsigned long long desc = (unsigned long long)p[0]
                            | ((unsigned long long)p[1] << 32);

    return (unsigned long)((desc >> 16) & 0xFFFFFFUL)
         | (unsigned long)((desc >> 56) & 0xFFUL) << 24;
}

unsigned long tss_gdt_type(void)
{
    unsigned int *p = (unsigned int *)(gdt + GDT_TSS_INDEX * 8);
    unsigned long long desc = (unsigned long long)p[0]
                            | ((unsigned long long)p[1] << 32);

    return (unsigned long)((desc >> 40) & 0xFFUL);
}
