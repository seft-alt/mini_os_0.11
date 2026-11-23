; boot/uprog6.asm —— "用户态去碰内核高半区"的负向探针
;
; 它只做一件事：在 ring3 里读 0x01100000（16MB + 1MB）。
; 每任务页目录里 dir[4..1023] = 0，所以这个地址在**用户自己的表里不存在**——
; 访问它必然 #PF，而且 err 的 U/S 位（bit2）是 1，说明发起者是用户态。
;
; 刻意**不写指纹**（[esp-24] 的 0x45584543）：被拦下的一趟必须还能被认出来。
; 期望行为（arch/exception.c 收紧判据之后）：
;   - 用户态缺页一律当越权：把返回帧改成"回内核"，既不补页也不停机；
;   - exec 的哨兵原样留着 → rc=E5、mag=0xDEAD0000、val=0xDEAD0000；
;   - 而且系统还活着：紧接着还能敲下一条 shell 命令。
; 判据收紧之前是另一番景象：cr2 >= 16MB 不满足老判据，请求被放给
; page_fault_map，补不上就打 "[mem] PF ... unhandled" 然后 kernel_idle——
; 整个系统停在空闲里，exec 永远等不到结果。
;
BITS 32

GLOBAL uprog6
GLOBAL uprog6_end

uprog6:
    mov eax, [0x01100000]               ; ← 触发点：用户表里没有 16MB 以上
    mov dword [esp - 20], eax           ; 真读到值才会走到这句（预期走不到）
uprog6_end:
    jmp $
