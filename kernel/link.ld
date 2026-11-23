/* ==============================================================
 * 内核链接脚本
 * 约束：head.o 的 .text 必须落在物理地址 0（head.asm 中 pg_dir 是
 *      .text 的第一个符号，setup_paging 以 0 为页目录基址）。
 * 链路：ld -m elf_i386 产出 ELF（符号来源），objcopy -O binary 得到纯二进制。
 * ============================================================== */
ENTRY(startup_32)

SECTIONS
{
    . = 0;

    .text : {
        *(.text)
        *(.text.*)
    }

    .rodata : {
        *(.rodata)
        *(.rodata.*)
    }

    /* 布局契约（core/layout.c）：只给构建期的 objcopy 单独剥离，不参与运行。
     * 单独给一条规则是为了让 "这个段在哪" 不受孤儿段摆放策略影响。 */
    .layout : {
        *(.layout)
    }

    .data : {
        *(.data)
        *(.data.*)
    }

    /* 栈放在 20KB 之后：setup_paging 会 rep stosd 清掉 0~0x5000 */
    .bss : {
        *(.bss)
        *(COMMON)
    }

    /* 内核映像的“最后一字节之后”：物理页分配器必须从它上面开始。
     * 这个符号是 mm/page.c 的 page_init() 运行时检查、tools/kernel-fit.py
     * 构建期检查共同引用的唯一边界。 */
    __kernel_end = .;

    /DISCARD/ : {
        *(.eh_frame)
        *(.comment)
        *(.note*)
    }
}
