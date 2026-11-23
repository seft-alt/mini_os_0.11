; boot/uprog2.asm —— 一段"跨两页"的 ELF 程序（tools/uprog2.ld 把它链成跨页 ELF）
;
; 和 uprog.asm 的区别只有一处：它的 p_memsz 越过 4KB（.bss 被摆在 0x1800），
; 所以 exec 必须给它**一段连续两页**，而不是往常的一页。它证明三件事：
;
;   1. exec 按 elf_span 真的要到了两页（sh 命令表里 `exec` 会多打一行 `exec pg=2`）；
;   2. 第二页确实被映射、被清 0：程序先读 [base+0x1800] 必须是 0，
;      不是 0 就把结果写成 0xDEAD0000 让内核判失败；
;   3. 第二页真的能写：写 0x2A 到 0x1800、写 0x07 到 0x1FF0（第二页最后一个双字
;      附近），再读回来拼成结果 0x72A = 1834 —— 内核据此知道"跨页那一半能读能写"。
;
; 为什么必须自己算基址：exec 把段搬到 code_base + vaddr，而 ld 给的符号地址是
; vaddr（从 0 起）。所以 eip 先算出来、再减掉链接期的偏移，就得到 code_base。
; 这段代码仍然只能用相对 esp 的偏移（用户栈才是内核给的）。
;
; 与内核的约定与 uprog.asm 完全相同：
;   [esp0-128] 要打印的字符串
;   [esp0-24]  magic 0x45584543
;   [esp0-20]  结果值
;   [esp0-16]  {nr, a0, a1, a2}
BITS 32

GLOBAL uprog2

uprog2:
    ; ---- 1. 算出 code_base（PIC 的标准起手式：call 下一条、pop 回来）----
    call .here
.here:
    pop eax
    sub eax, .here - uprog2             ; eax = code_base（这对符号在同一段，差值=常数）

    ; ---- 2. 第二页必须是清过 0 的（.bss 的 memsz 那段）----
    mov edx, [eax + 0x1800]
    test edx, edx
    jnz .dirty

    ; ---- 3. 第二页能写、能读回 ----
    mov dword [eax + 0x1800], 0x2A
    mov dword [eax + 0x1FF0], 0x07
    mov edx, [eax + 0x1800]
    mov ecx, [eax + 0x1FF0]
    shl ecx, 8
    or  edx, ecx                        ; edx = 0x072A = 1834
    jmp .store

.dirty:
    mov edx, 0xDEAD0000                 ; 第二页不是 0：让内核看见"清零没做对"

.store:
    mov dword [esp - 24], 0x45584543    ; "EXEC"：跑完的指纹
    mov dword [esp - 20], edx           ; 结果值

    ; ---- 4. sys_write("uprog2:pg2 ok\n", 14) ----
    ; 'upro' 'g2:p' 'g2 o' 'k\n\0\0'  -> 14 字节（后两个字节留 0，不进长度）
    mov dword [esp - 128], 'upro'
    mov dword [esp - 124], 'g2:p'
    mov dword [esp - 120], 'g2 o'
    mov dword [esp - 116], 0x000A6B    ; 'k','\n','\0','\0'
    mov dword [esp - 16], 1             ; nr = SYS_NR_WRITE
    lea eax, [esp - 128]
    mov dword [esp - 12], eax
    mov dword [esp - 8], 14
    mov dword [esp - 4], 0
    lea eax, [esp - 16]
    call 0x18:0

    ; ---- 5. sys_exit(0x2A = 42) ----
    mov dword [esp - 16], 2
    mov dword [esp - 12], 0x2A
    mov dword [esp - 8], 0
    mov dword [esp - 4], 0
    lea eax, [esp - 16]
    call 0x18:0
    jmp $

; .bss 段必须存在：tools/uprog2.ld 把它摆到 0x1800，
; 于是 p_memsz 至少是 0x1804，exec 看到的就是"两页"。
SECTION .bss
bss_tail:
    resb 4
