; boot/uprog4.asm —— 参数传递的探针：证明 argc/argv 真的从内核递到了 ring3
;
; 和其它 uprog 一样，它不是内核的一部分：内核只把它当字节流装进用户页。
; 位置无关：全部用相对 ESP 的偏移。
;
; 与内核的约定（见 include/fs/exec.h 的"参数怎么递到用户程序手里"）：
;   [esp0-44]  argc
;   [esp0-56]  argv[0] .. argv[2] 的绝对指针（步长 4）
;   [esp0-128 + i*24]  argv[i] 的字符串（最多 23 字节 + NUL）
;   [esp0-24]  magic   [esp0-20] 结果值   [esp0-16] {nr,a0,a1,a2}
; 进 ring3 那一刻 esp == esp0，所以"自己的栈顶"就是取参数的唯一锚点。
;
; 它做三件事：
;   1. 有第二个参数就打印 argv[1]，否则打印 argv[0] —— 直接看出"参数到没到"；
;   2. 把 argc 写进结果区（内核打 val=），指纹照旧写 0x45584543；
;   3. exit(argc) —— 退出码也是 argc，两个通道互相印证。
BITS 32

GLOBAL uprog4
GLOBAL uprog4_end

uprog4:
    mov eax, [esp - 44]                 ; argc
    mov dword [esp - 24], 0x45584543    ; 指纹 "EXEC"
    mov [esp - 20], eax                 ; 结果区 = argc

    mov esi, [esp - 56]                 ; argv[0]（恒在）
    cmp eax, 2
    jb .have                            ; argc < 2 → 就打 argv[0]
    mov esi, [esp - 52]                 ; argv[1]：证明第 2 个参数真的到了

.have:
    ; ---- 算长度（最多 23，和内核的槽宽一致）----
    xor ecx, ecx
.scan:
    cmp ecx, 23
    jae .wlen
    cmp byte [esi + ecx], 0
    je .wlen
    inc ecx
    jmp .scan
.wlen:
    ; ---- sys_write(esi, len) ----
    mov dword [esp - 16], 1             ; nr = SYS_NR_WRITE
    mov [esp - 12], esi                 ; a0 = 串首址
    mov [esp - 8], ecx                  ; a1 = 长度
    mov dword [esp - 4], 0
    lea eax, [esp - 16]
    call 0x18:0

    ; ---- 补一个换行（借用布局里空着的 [esp0-40] 那一格）----
    mov byte [esp - 40], 10
    mov dword [esp - 16], 1
    lea eax, [esp - 40]
    mov [esp - 12], eax
    mov dword [esp - 8], 1
    mov dword [esp - 4], 0
    lea eax, [esp - 16]
    call 0x18:0

    ; ---- sys_exit(argc) ----
    mov dword [esp - 16], 2
    mov eax, [esp - 44]
    mov [esp - 12], eax
    mov dword [esp - 8], 0
    mov dword [esp - 4], 0
    lea eax, [esp - 16]
    call 0x18:0
    jmp $                               ; 正常不会走到这里
uprog4_end:
