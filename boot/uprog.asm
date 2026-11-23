; boot/uprog.asm —— 一段"从磁盘装进用户页、在 ring3 跑起来"的示例程序
;
; 它不是内核的一部分：内核只把它当字节流（挂到 mosfs 数据盘里当文件），
; 由 fs.c 读出来、拷进用户页、交给 `user_enter` iret 进 ring3。
; 所以整段必须是位置无关的：只能用相对 ESP 的偏移，不能有绝对地址。
;
; 它现在演示三件事，一条 syscall 一件：
;   1. sys_write：把自己的字符串打到用户页控制台（ring3 第一次"开口"）；
;   2. 算 1..8 = 36，连同指纹写进结果区（内核据此知道它真的跑完了）；
;   3. sys_exit：把 36 交回内核，而不是用 0xFF 那条老旁路"悄悄走掉"。
;
; 与内核的约定（内核在 exec 里准备的用户栈顶 esp0，窗口 = [esp0-128, esp0)）：
;   [esp0-128] 要打印的字符串（15 字节）
;   [esp0-24]  magic 0x45584543 ("EXEC")
;   [esp0-20]  结果值 36
;   [esp0-16]  请求结构体 {nr, a0, a1, a2}
;
; syscall 号：0 = tune, 1 = write, 2 = exit, 3 = ticks（见 include/sys/syscall.h）
BITS 32

GLOBAL uprog
GLOBAL uprog_end

uprog:
    ; ---- 1. 先算数（纯寄存器，不需要内核）----
    mov dword [esp - 24], 0x45584543    ; "EXEC"：跑完的指纹
    xor eax, eax                        ; 1+2+...+8 = 36
    mov ecx, 8
.loop:
    add eax, ecx
    loop .loop
    mov dword [esp - 20], eax

    ; ---- 2. sys_write("uprog:ring3 ok\n", 15) ----
    ; 字符串必须放在用户栈上（窗口只覆盖栈顶那一段），所以用四个双字拼：
    ;   'upro' 'g:ri' 'ng3 ' 'ok\n\0'  ->  "uprog:ring3 ok\n"
    mov dword [esp - 128], 'upro'
    mov dword [esp - 124], 'g:ri'
    mov dword [esp - 120], 'ng3 '
    mov dword [esp - 116], 0x0A6B6F    ; 'o','k','\n'  = 0x0A6B6F（第三个字节是 0A）

    mov dword [esp - 16], 1             ; nr = SYS_NR_WRITE
    lea eax, [esp - 128]
    mov dword [esp - 12], eax           ; a0 = 串首址
    mov dword [esp - 8], 15             ; a1 = 长度
    mov dword [esp - 4], 0              ; a2
    lea eax, [esp - 16]
    call 0x18:0                         ; 调用门 → sys_write

    ; ---- 3. sys_exit(36) ----
    mov dword [esp - 16], 2             ; nr = SYS_NR_EXIT
    mov dword [esp - 12], 36            ; a0 = 退出码
    mov dword [esp - 8], 0
    mov dword [esp - 4], 0
    lea eax, [esp - 16]
    call 0x18:0                         ; 门的处理体把返回帧改成回 ring0
    jmp $                               ; 正常不会走到这里
uprog_end:
