; ==============================================================
; Linux 0.11 bootsect 引导扇区（NASM 转换版）
; 功能：自搬移 → 加载 setup → 加载内核 → 跳转 setup
; 编译格式：nasm -f bin boot.asm -o boot.bin
; ==============================================================
[BITS 16]
[ORG 0x0000]

; -------------------- 常量配置 --------------------
SYSSIZE     equ 0x3000    ; 内核最大尺寸（单位：16字节），总计 192KB
SETUPLEN    equ 4         ; setup 占 4 个扇区
BOOTSEG     equ 0x07C0    ; BIOS 初始加载位置（段地址）
INITSEG     equ 0x9000    ; bootsect 自搬移目标段地址
SETUPSEG    equ 0x9020    ; setup 加载段地址
SYSSEG      equ 0x1000    ; 内核加载起始段地址
ENDSEG      equ SYSSEG + SYSSIZE  ; 内核加载结束段地址
ROOT_DEV    equ 0x306     ; 默认根设备号

; -------------------- 入口起点 --------------------
start:
    mov ax, BOOTSEG
    mov ds, ax
    mov ax, INITSEG
    mov es, ax
    mov cx, 256           ; 复制 256 字 = 512 字节
    sub si, si
    sub di, di
    rep movsw             ; 把自身从 0x7C00 搬到 0x90000
    jmp INITSEG:go        ; 远跳转到搬移后的位置执行

go:
    mov ax, cs
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0xFF00        ; 栈设置在 0x9FF00

; -------------------- 加载 setup 模块 --------------------
load_setup:
    mov dx, 0x0000        ; 驱动器 0，磁头 0
    mov cx, 0x0002        ; 柱面 0，扇区 2
    mov bx, 0x0200        ; 目标偏移 0x200（物理 0x90200）
    mov ax, 0x0200 + SETUPLEN
    int 0x13              ; BIOS 读扇区
    jnc ok_load_setup     ; 读成功跳转

    ; 读失败：复位磁盘并重试
    mov dx, 0x0000
    mov ax, 0x0000
    int 0x13
    jmp load_setup

ok_load_setup:
    ; 获取磁盘参数（每磁道扇区数）
    mov dl, 0x00
    mov ax, 0x0800
    int 0x13
    mov ch, 0x00
    mov [cs:sectors], cx   ; 保存每磁道扇区数

    mov ax, INITSEG
    mov es, ax

; -------------------- 打印加载提示 --------------------
    mov ah, 0x03           ; 读光标位置
    xor bh, bh
    int 0x10

    mov cx, 24
    mov bx, 0x0007         ; 黑底白字
    mov bp, msg1
    mov ax, 0x1301         ; 写字符串，移动光标
    int 0x10

; -------------------- 加载内核 system --------------------
    mov ax, SYSSEG
    mov es, ax
    call read_it           ; 读取内核到 0x10000
    call kill_motor        ; 关闭软驱电机

; -------------------- 根设备判定 --------------------
    mov ax, [cs:root_dev]
    cmp ax, 0
    jne root_defined

    mov bx, [cs:sectors]
    mov ax, 0x0208         ; 1.2MB 软盘（15扇区/磁道）
    cmp bx, 15
    je root_defined
    mov ax, 0x021C         ; 1.44MB 软盘（18扇区/磁道）
    cmp bx, 18
    je root_defined

undef_root:
    jmp undef_root

root_defined:
    mov [cs:root_dev], ax

; -------------------- 跳转到 setup --------------------
    jmp SETUPSEG:0x0000

; ==============================================================
; 内核加载函数：按磁道批量读取，处理 64KB 段边界
; 输入：es = 内核起始段地址
; ==============================================================
sread:  dw 1 + SETUPLEN    ; 当前磁道已读扇区数
head:   dw 0               ; 当前磁头号
track:  dw 0               ; 当前柱面号

read_it:
    mov ax, es
    test ax, 0x0FFF
die:
    jne die               ; 段地址必须 64KB 对齐，否则死循环
    xor bx, bx            ; 段内偏移从 0 开始

rp_read:
    mov ax, es
    cmp ax, ENDSEG
    jb ok1_read
    ret                   ; 内核全部加载完成，返回
ok1_read:
    mov ax, [cs:sectors]
    sub ax, [cs:sread]
    mov cx, ax
    shl cx, 9
    add cx, bx
    jnc ok2_read
    je ok2_read
    xor ax, ax
    sub ax, bx
    shr ax, 9
ok2_read:
    call read_track
    mov cx, ax
    add ax, [cs:sread]
    cmp ax, [cs:sectors]
    jne ok3_read
    mov ax, 1
    sub ax, [cs:head]
    jne ok4_read
    inc word [cs:track]
ok4_read:
    mov [cs:head], ax
    xor ax, ax
ok3_read:
    mov [cs:sread], ax
    shl cx, 9
    add bx, cx
    jnc rp_read
    mov ax, es
    add ax, 0x1000
    mov es, ax
    xor bx, bx
    jmp rp_read

; ==============================================================
; 读单个磁道
; ==============================================================
read_track:
    push ax
    push bx
    push cx
    push dx

    mov dx, [cs:track]
    mov cx, [cs:sread]
    inc cx
    mov ch, dl
    mov dx, [cs:head]
    mov dh, dl
    mov dl, 0
    and dx, 0x0100
    mov ah, 2
    int 0x13
    jc bad_rt             ; CF=1 表示读失败，跳去复位重试
    pop dx
    pop cx
    pop bx
    pop ax
    ret
bad_rt:
    mov ax, 0
    mov dx, 0
    int 0x13
    pop dx
    pop cx
    pop bx
    pop ax
    jmp read_track

; ==============================================================
; 关闭软驱主轴电机
; ==============================================================
kill_motor:
    push dx
    mov dx, 0x3F2
    mov al, 0
    out dx, al
    pop dx
    ret

; ==============================================================
; 数据区
; ==============================================================
sectors:    dw 0

msg1:
    db 13, 10
    db "Loading system ..."
    db 13, 10, 13, 10

; 填充到 508 字节，预留根设备号和引导魔数
times 508 - ($ - $$) db 0
root_dev:   dw ROOT_DEV
boot_flag:  dw 0xAA55      ; BIOS 引导魔数，必须有

    