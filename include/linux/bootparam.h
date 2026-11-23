/* ==============================================================
 * BIOS 硬件参数区
 *
 * setup.asm 在进入保护模式前把 BIOS 查询结果写到物理 0x90000，
 * 内核可从这里直接读取，无需再调 BIOS 中断。
 * ============================================================== */
#ifndef MINI_OS_BOOTPARAM_H
#define MINI_OS_BOOTPARAM_H

#define BOOT_PARAM_ADDR  0x90000

/* 字段命名与 setup.asm 的存储方式一致（见 boot/setup.asm 第一阶段） */
struct boot_params {
    unsigned short cursor_pos;    /* 0x00 int 10h/AH=03 的 DX：光标位置 */
    unsigned short ext_mem_k;     /* 0x02 int 15h/AH=88 的 AX：扩展内存 KB */
    unsigned short display_page;  /* 0x04 int 10h/AH=0F 的 BX：bl 模式, bh 页号 */
    unsigned short video_info;    /* 0x06 int 10h/AH=0F 的 AX：al 模式, ah 列数 */
    unsigned short ega_ax;        /* 0x08 int 10h/AH=12 的 AX */
    unsigned short ega_bx;        /* 0x0A int 10h/AH=12 的 BX */
    unsigned short ega_cx;        /* 0x0C int 10h/AH=12 的 CX */
};

#define boot_params (*(volatile struct boot_params *)BOOT_PARAM_ADDR)

#endif /* MINI_OS_BOOTPARAM_H */
