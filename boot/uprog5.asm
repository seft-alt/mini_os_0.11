; boot/uprog5.asm —— fd 层的探针：证明"用户程序能打开/读/写/关"是真的
;
; 和其它 uprog 一样，它不是内核的一部分：内核只把它当字节流装进用户页。
; 位置无关：全部用相对 ESP 的偏移，不出现任何绝对地址（没有任何数据标签）。
; 名字也是用立即数写进栈上的——因为程序被装到 code 页头部（0x26000），
; 汇编期算出的"标签绝对地址"加载后会指向别的地方，那是位置无关代码的死法。
;
; 与内核的约定（见 include/fs/exec.h 的窗口布局）：
;   [esp0-16]   {nr, a0, a1, a2}     调用门请求结构体
;   [esp0-24]   magic                0x45584543
;   [esp0-20]   结果值               这里放"第一次 read 读回来几个字节"
;   [esp0-256]  I/O 缓冲起点（fd 的 read/write 数据段，128 字节）
;
; 它做五件事：
;   1. open("hello.txt", 0)  —— 读模式，文件名摆在 [esp-256]；
;   2. read(fd, [esp-192], 16) —— 把前 16 个字节读到 I/O 缓冲；
;   3. write(1, 缓冲, n)     —— 用**控制台** write 把它打出来（屏幕上是真内容）；
;   4. open("fdout", 1) + fwrite 一行 + close —— 证明写模式真的落了盘；
;   5. exit(n)               —— 退出码就是读回的字节数，和结果区互相印证。
;
; 失败时给出可分辨的退出码：97 = 打不开 hello.txt，98 = 读被拒，96 = 建不出 fdout。
BITS 32

GLOBAL uprog5
GLOBAL uprog5_end

uprog5:
    ; ---- 名字1 "hello.txt" -> [esp-256]（三条 dword 立即数，小端拼字节）----
    mov dword [esp - 256], 0x6C6C6568        ; 'h' 'e' 'l' 'l'
    mov dword [esp - 252], 0x78742E6F        ; 'o' '.' 't' 'x'
    mov dword [esp - 248], 0x00000074        ; 't' 0
    ; ---- 名字2 "fdout" -> [esp-240] ----
    mov dword [esp - 240], 0x756F6466        ; 'f' 'd' 'o' 'u'
    mov dword [esp - 236], 0x00000074        ; 't' 0

    ; ---- open("hello.txt", 0) ----
    mov dword [esp - 16], 4                  ; nr = SYS_NR_OPEN
    lea eax, [esp - 256]
    mov [esp - 12], eax                      ; a0 = 名字首址
    mov dword [esp - 8], 0                   ; a1 = 0（读）
    mov dword [esp - 4], 0
    lea eax, [esp - 16]
    call 0x18:0
    cmp eax, -1
    jne .opened
    mov dword [esp - 12], 97
    jmp .die
.opened:
    mov [esp - 32], eax                      ; fd 存在 [esp-32]（窗口里空着的一格）

    ; ---- read(fd, [esp-192], 16) ----
    mov dword [esp - 16], 5                  ; nr = SYS_NR_READ
    mov eax, [esp - 32]
    mov [esp - 12], eax                      ; a0 = fd
    lea eax, [esp - 192]
    mov [esp - 8], eax                       ; a1 = 缓冲（4 字节对齐，落在窗口里）
    mov dword [esp - 4], 16                  ; a2 = 长度
    lea eax, [esp - 16]
    call 0x18:0
    cmp eax, -1
    jne .readok
    mov dword [esp - 12], 98
    jmp .die
.readok:
    mov [esp - 20], eax                      ; 结果区 = 读回字节数
    mov dword [esp - 24], 0x45584543         ; 指纹 "EXEC"

    ; ---- write(1, 缓冲, n)：把真内容打到屏幕上 ----
    mov dword [esp - 16], 1                  ; nr = SYS_NR_WRITE（控制台）
    lea ecx, [esp - 192]
    mov [esp - 12], ecx
    mov [esp - 8], eax                       ; 长度还是 read 的返回值
    mov dword [esp - 4], 0
    lea eax, [esp - 16]
    call 0x18:0

    ; ---- 补一个换行（借用布局里空着的 [esp-40]）----
    mov byte [esp - 40], 10
    mov dword [esp - 16], 1
    lea eax, [esp - 40]
    mov [esp - 12], eax
    mov dword [esp - 8], 1
    mov dword [esp - 4], 0
    lea eax, [esp - 16]
    call 0x18:0

    ; ---- close(fd) ----
    mov dword [esp - 16], 7
    mov eax, [esp - 32]
    mov [esp - 12], eax
    mov dword [esp - 8], 0
    mov dword [esp - 4], 0
    lea eax, [esp - 16]
    call 0x18:0

    ; ---- open("fdout", 1)：写模式，文件不存在就该被建出来 ----
    mov dword [esp - 16], 4
    lea eax, [esp - 240]
    mov [esp - 12], eax
    mov dword [esp - 8], 1
    mov dword [esp - 4], 0
    lea eax, [esp - 16]
    call 0x18:0
    cmp eax, -1
    jne .opened2
    mov dword [esp - 12], 96
    jmp .die
.opened2:
    mov [esp - 32], eax

    ; ---- fwrite(fd, "fdwr\n", 5)：只写 5 个字节，盘上应当正好多这 5 个 ----
    mov dword [esp - 192], 0x72776466        ; 'f' 'd' 'w' 'r'
    mov byte [esp - 188], 10                 ; '\n'
    mov dword [esp - 16], 6                  ; nr = SYS_NR_FWRITE
    mov eax, [esp - 32]
    mov [esp - 12], eax
    lea eax, [esp - 192]
    mov [esp - 8], eax
    mov dword [esp - 4], 5
    lea eax, [esp - 16]
    call 0x18:0

    ; ---- close(fd2) ----
    mov dword [esp - 16], 7
    mov eax, [esp - 32]
    mov [esp - 12], eax
    mov dword [esp - 8], 0
    mov dword [esp - 4], 0
    lea eax, [esp - 16]
    call 0x18:0

    ; ---- exit(n)：退出码 = 读回的字节数 ----
    mov dword [esp - 16], 2
    mov eax, [esp - 20]
    mov [esp - 12], eax
    mov dword [esp - 8], 0
    mov dword [esp - 4], 0
    lea eax, [esp - 16]
    call 0x18:0
    jmp $

.die:                                        ; a0 已经放好失败码，补 nr = 2 再走
    mov dword [esp - 16], 2
    mov dword [esp - 8], 0
    mov dword [esp - 4], 0
    lea eax, [esp - 16]
    call 0x18:0
    jmp $
uprog5_end:
