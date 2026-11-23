; boot/uprog3.asm —— 一段"在 ring3 赖着不走"的示例程序（userpg 自检的探针）
;
; 它为什么要存在：TSS.esp0 从"全局一张栈"改成"跟着任务走"之后，
; 唯一能证明它真的生效的现场是 —— **程序正跑在 ring3（CS=0x23）时，
; TSS.esp0 指向的是这个任务自己的页，而不是开机装进去的默认栈**。
; 要抓到这个瞬间，程序必须在 ring3 停留一会儿，于是有了这段忙等。
;
; 注意：现在任务里跑 ring3 时 IF=1（`user_enter` 对 cur!=0 用 eflags=0x202），
; 所以时钟是**能**打断这段忙等的，ticks 会涨——这正是 tools/ring3pre.sh
; 的断言依据（采样 CS=0x23 期间看 tasks[1].ticks 有没有往前走）。
; 这里仍然烧 CPU 而不是等 tick，是为了让"被抢占"这件事有足够长的窗口可采。
; 两层 dec/jnz 计数循环，具体烧多久看宿主机速度（QEMU TCG 下约 1～2 秒）。
; 自检脚本从"发出 exec"计时，量出来的那个秒数就是它的停留时长。
;
; 与内核的约定照旧（窗口 = [esp0-128, esp0)）：
;   [esp0-128] 要打印的字符串（"uprog3:slow ok\n"，15 字节）
;   [esp0-24]  magic 0x45584543 ("EXEC")
;   [esp0-20]  结果值 0xC0DE0003（"我真的把循环烧完了"）
;   [esp0-16]  {nr, a0, a1, a2}
BITS 32

GLOBAL uprog3
GLOBAL uprog3_end

uprog3:
    ; ---- 1. 先把指纹写上：哪怕后面被谁打断，"它跑到了这一段"也算数 ----
    mov dword [esp - 24], 0x45584543    ; "EXEC"

    ; ---- 2. 烧 CPU：400 * 1000000 次内层循环 ----
    ; 自检脚本要从宿主机隔着 QMP 采样（一次采样往返几十毫秒），
    ; 这段必须比"采样间隔"长得多。实测 40M 次只有零点几秒，够不着；放到 400M。
    mov ecx, 400                        ; 外层
.outer:
    mov eax, 1000000                    ; 内层
.inner:
    dec eax
    jnz .inner
    dec ecx
    jnz .outer

    mov dword [esp - 20], 0xC0DE0003    ; 循环烧完了（不是哨兵值）

    ; ---- 3. sys_write("uprog3:slow ok\n", 15) ----
    mov dword [esp - 128], 'upro'
    mov dword [esp - 124], 'g3:s'
    mov dword [esp - 120], 'low '
    mov dword [esp - 116], 0x0A6B6F     ; 'o','k','\n'

    mov dword [esp - 16], 1             ; nr = SYS_NR_WRITE
    lea eax, [esp - 128]
    mov dword [esp - 12], eax           ; a0 = 串首址
    mov dword [esp - 8], 15             ; a1 = 长度
    mov dword [esp - 4], 0              ; a2
    lea eax, [esp - 16]
    call 0x18:0                         ; 调用门 → sys_write

    ; ---- 4. sys_exit(9) ----
    mov dword [esp - 16], 2             ; nr = SYS_NR_EXIT
    mov dword [esp - 12], 9             ; a0 = 退出码
    mov dword [esp - 8], 0
    mov dword [esp - 4], 0
    lea eax, [esp - 16]
    call 0x18:0                         ; 门的处理体把返回帧改成回 ring0
    jmp $                               ; 正常不会走到这里
uprog3_end:
