; ==============================================================
; Linux 0.11 head.s 32位保护模式入口（NASM 转换版）
; 运行地址：物理地址 0x00000000（由 setup 搬移内核到此）
; 功能：初始化硬件环境 → 开启分页 → 跳转C内核入口
; ==============================================================
[BITS 32]

; ---------- 全局符号导出（供C代码引用） ----------
GLOBAL startup_32
GLOBAL idt
GLOBAL gdt
GLOBAL pg_dir
GLOBAL tmp_floppy_area

; ---------- 外部C符号声明 ----------
EXTERN main
EXTERN stack_start
EXTERN printk

; ==============================================================
; 页目录（物理地址 0x00000）
; 注意：启动完成后该区域会被正式页目录覆盖，属于临时复用空间
; ==============================================================
SECTION .text
pg_dir:

startup_32:
    ; 1. 初始化所有段寄存器为内核数据段（选择子0x10）
    mov eax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; 2. 加载内核栈：lss 同时加载 ss 和 esp
    lss esp, [stack_start]

    ; 3. 初始化默认IDT、加载正式GDT
    call setup_idt
    call setup_gdt

    ; 4. GDT更新后刷新所有段寄存器缓存
    mov eax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    lss esp, [stack_start]

; ==============================================================
; 检测 A20 地址线是否真正开启
; 通过 0 地址与 1MB 地址回绕特性判断，不开启则死循环
; ==============================================================
    xor eax, eax
a20_check:
    inc eax
    mov [0x000000], eax
    cmp [0x100000], eax
    je a20_check

; ==============================================================
; 配置 x87 浮点协处理器
; ==============================================================
    mov eax, cr0
    and eax, 0x80000011  ; 保留 PG、PE、ET 标志位
    or eax, 0x00000002   ; 置 MP 位，开启协处理器监控
    mov cr0, eax

    call check_x87

    ; 跳转到分页初始化，完成后进入C内核
    jmp after_page_tables

; ==============================================================
; 检测协处理器类型（287 / 387）
; ==============================================================
check_x87:
    fninit
    fstsw ax
    cmp al, 0
    je .has_287        ; 状态为0说明是287，需切换模式
    ; 无FPU：开启模拟模式
    mov eax, cr0
    xor eax, 0x00000006  ; 清MP、置EM，用软件模拟浮点
    mov cr0, eax
    ret
align 2
.has_287:
    db 0xDB, 0xE4      ; 287 专用 fsetpm 指令，387 自动忽略
    ret

; ==============================================================
; setup_idt：批量填充256个中断门，默认指向兜底函数
; ==============================================================
setup_idt:
    lea edx, [ignore_int]
    mov eax, 0x00080000
    mov ax, dx            ; 中断处理函数低16位偏移
    mov dx, 0x8E00         ; 中断门属性：P=1, DPL=0, 32位

    lea edi, [idt]
    mov ecx, 256
.rp_sidt:
    mov [edi], eax
    mov [edi+4], edx
    add edi, 8
    dec ecx
    jnz .rp_sidt

    lidt [idt_descr]
    ret

; ==============================================================
; setup_gdt：加载正式全局描述符表
; ==============================================================
setup_gdt:
    lgdt [gdt_descr]
    ret

; ==============================================================
; 页表区域（按4KB页对齐）
; 共4个页表，恒等映射前16MB物理内存
; 注意：必须各占一页实体空间，否则 pg0~pg3 会因层层对齐而地址重合，
;       setup_paging 填出的 4 个 PDE 会指向同一张页表。
; ==============================================================
ALIGN 4096
pg0:    times 4096 db 0
pg1:    times 4096 db 0
pg2:    times 4096 db 0
pg3:    times 4096 db 0

; 软盘DMA临时缓冲区（必须对齐，避免跨64KB边界导致DMA异常）
tmp_floppy_area:
    times 1024 db 0

; ==============================================================
; 分页准备完成，压入参数跳转到C内核main
; ==============================================================
after_page_tables:
    push 0          ; 第三个参数 envp
    push 0          ; 第二个参数 argv
    push 0          ; 第一个参数 argc
    push L6         ; main 函数返回地址（正常不会返回）
    push main       ; main 函数入口地址
    jmp setup_paging

L6:
    jmp L6          ; main 意外返回则死循环

; ==============================================================
; 默认中断兜底：打印未知中断提示
; ==============================================================
int_msg:
    db "Unknown interrupt", 13, 10, 0

align 2
ignore_int:
    push eax
    push ecx
    push edx
    push ds
    push es
    push fs

    mov eax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax

    push int_msg
    call printk
    add esp, 4

    pop fs
    pop es
    pop ds
    pop edx
    pop ecx
    pop eax
    iret

; ==============================================================
; setup_paging：开启分页，恒等映射前16MB
; ==============================================================
align 2
setup_paging:
    ; 清零页目录 + 4个页表，共5页
    mov ecx, 1024 * 5
    xor eax, eax
    xor edi, edi
    cld
    rep stosd

    ; 填写页目录项：4个页表，属性 7（存在、可读写、特权级3）
    mov dword [pg_dir + 0],  pg0 + 7
    mov dword [pg_dir + 4],  pg1 + 7
    mov dword [pg_dir + 8],  pg2 + 7
    mov dword [pg_dir + 12], pg3 + 7

    ; 反向填充页表项（从高地址往低地址写，效率更高）
    mov edi, pg3 + 4092
    mov eax, 0x00FFF007    ; 16MB - 4KB + 属性7
    std
.fill_loop:
    stosd
    sub eax, 0x1000
    jge .fill_loop

    ; 设置 CR3 = 页目录基址
    xor eax, eax
    mov cr3, eax

    ; 置 CR0 的 PG 位，正式开启分页
    mov eax, cr0
    or eax, 0x80000000
    mov cr0, eax

    ret  ; 返回后自动弹出栈顶的 main 地址，跳转到C内核

; ==============================================================
; IDTR / GDTR 操作数结构
; ==============================================================
align 2
idt_descr:
    dw 256 * 8 - 1    ; IDT 界限
    dd idt             ; IDT 基址

align 2
gdt_descr:
    dw 256 * 8 - 1    ; GDT 界限
    dd gdt             ; GDT 基址

; ==============================================================
; 中断描述符表（256项，初始全0）
; ==============================================================
align 8
idt:
    times 256 dq 0

; ==============================================================
; 全局描述符表
; ==============================================================
align 8
gdt:
    dq 0x0000000000000000  ; 0# 空描述符
    dq 0x00C09A0000000FFF  ; 1# 内核代码段 0x08：16MB，执行/读
    dq 0x00CF92000000FFFF  ; 2# 内核数据段 0x10：4GB 平坦（G=1, limit=0xFFFFF）
                           ;    按需分页要访问 16MB 以上的地址，段界限必须放开，
                           ;    否则越过 16MB 先触发 #GP 而不是 #PF
    dq 0x0000000000000000  ; 3# 保留项
    times 252 dq 0          ; 预留 LDT、TSS 等空间
