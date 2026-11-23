; boot/badprog.asm —— 负向用例：一个"不听话"的 ring3 程序
;
; 它只做两件事，都是为了证明内核那两道关真的在拦：
;   1. sys_write 时故意传一个**窗口外**的指针（esp-4096，落在用户栈页的另一头，
;      不在 exec 注册的 [esp0-128, esp0) 里）→ 门处理体必须**一次也不解引用**、
;      直接返回 0，并且计一次 denied；
;   2. 不写指纹 magic，最后 sys_exit(7) 回内核 → exec 读回的结果区仍是哨兵
;      0xDEAD0000，所以 exec 报 E5（"没跑完"），退出码却是 7。
;
; 期望的观测：`exec bad rc=E5 mag=0xDEAD0000 val=0xDEAD0000 ex=7 sy=2/1 n=?/?`
; 其中 sy=2/1 说明"两次过门、一次被拒"——被拒的那次正是那个越界指针。
BITS 32

GLOBAL badprog
GLOBAL badprog_end

badprog:
    ; ---- sys_write(窗口外的指针, 4) ----
    mov dword [esp - 16], 1             ; nr = SYS_NR_WRITE
    lea eax, [esp - 4096]               ; 故意越界：窗口只有 [esp0-128, esp0)
    mov dword [esp - 12], eax
    mov dword [esp - 8], 4
    mov dword [esp - 4], 0
    lea eax, [esp - 16]
    call 0x18:0                         ; 门 → 指针校验失败 → eax = 0

    ; ---- sys_exit(7)：不写指纹，故意让 exec 的哨兵留在原值 ----
    mov dword [esp - 16], 2
    mov dword [esp - 12], 7
    mov dword [esp - 8], 0
    mov dword [esp - 4], 0
    lea eax, [esp - 16]
    call 0x18:0
    jmp $
badprog_end:
