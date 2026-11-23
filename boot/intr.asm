; ==============================================================
; 中断/异常入口桩（NASM，ELF32）
;
; head.asm 的 setup_idt 把 256 个中断门全部指向 ignore_int 兜底。这里提供
; 真正的入口桩：CPU 压栈 -> 桩保存寄存器、装载内核数据段 -> 调 C 处理函数
; -> 恢复寄存器 -> iret。
;
; 两类异常要分开处理：
;   不带错误码（向量 0）：栈上是 EIP/CS/EFLAGS
;   带错误码（向量 13/14）：CPU 在它们之上再压一个错误码，返回前必须弹掉
;
; 桩调用 C 的 exception_handler(vec, err)，参数按 cdecl 从右往左压栈。
; ==============================================================
[BITS 32]

GLOBAL isr_divide_error
GLOBAL isr_general_protection
GLOBAL isr_page_fault
GLOBAL irq0_timer
GLOBAL irq1_keyboard
GLOBAL isr_yield
GLOBAL isr_tune

EXTERN exception_handler
EXTERN exception_handler_gp
EXTERN exception_handler_pf
EXTERN irq0_timer_handler
EXTERN irq1_keyboard_handler
EXTERN task_yield_handler
EXTERN tune_syscall

%define PUSHA_BYTES 32              ; pusha 压入 8 个寄存器的字节数

SECTION .text

; --------------------------------------------------------------
; 向量 0：除零异常（CPU 不压错误码）
; --------------------------------------------------------------
isr_divide_error:
    pusha
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    cld
    push dword 0                    ; 错误码：该异常没有
    push dword 0                    ; 向量号
    call exception_handler
    add esp, 8
    popa
    iret

; --------------------------------------------------------------
; 向量 13：通用保护异常（CPU 压错误码 = 出问题的选择子，或 0）
;
; 和 14 号一样把现场地址交给 C，并接受它返回的 esp。为什么要这样：
; ring3 执行 `in`/`out` 时，TSS 里没有 I/O 位图 → #GP(0)。这是**权限位在
; 干活**，不是内核 bug；发生在 ring3 的 #GP 应当“把控制权收回内核”，
; 而不是把整个内核停掉（停机就再也看不到键盘，自检也走不完）。
; 内核自己触发的 #GP 仍然按老路径打印 + 停机。
; --------------------------------------------------------------
isr_general_protection:
    pusha
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    cld
    push esp                        ; 现场地址（错误码在 ctx[8]）
    call exception_handler_gp
    add esp, 4
    mov esp, eax                    ; C 可以返回同一个值（不改栈）
    popa
    add esp, 4                      ; 丢掉 CPU 压的错误码
    iret

; --------------------------------------------------------------
; 向量 14：缺页异常（CPU 压错误码：P/W/U 位 + 保留位）
; 出错的线性地址在 CR2 里，由 C 处理函数读取
;
; 这个桩与 13 号不同：它把现场地址（= 错误码那一格的地址）传给 C，并且接受
; C 返回的 esp。为什么需要：栈溢出撞到 guard 页时，我们不想让整个内核停机，
; 只想让**那个任务**死掉——C 里把保存的 eip 改成 task_exit 的地址，iret 之后
; 任务就自己走进正常退出路径（槽位释放、栈页延后回收）。
; 布局（pusha 之后，esp = S）：
;   [S+0] 错误码  [S+4] eip  [S+8] cs  [S+12] eflags
; --------------------------------------------------------------
isr_page_fault:
    pusha
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    cld
    push esp                        ; 现场地址：参数
    call exception_handler_pf
    add esp, 4
    mov esp, eax                    ; C 可以返回同一个值（不改栈）
    popa
    add esp, 4                      ; 丢掉 CPU 压的错误码
    iret

; --------------------------------------------------------------
; 向量 0x20：8254 时钟中断（IRQ0）
;
; 这里比异常桩多一步：C 处理函数返回“本次要恢复的栈指针”，桩把它装进 esp。
; 于是 popa + iret 恢复的是**被选中任务**的现场，而不是被中断的那个——
; 任务切换就是这么一次 mov esp 完成的。
;
;   push esp            ; 参数 = 刚压好的 pusha 保存区地址（也是 esp 的落点）
;   call handler        ; -> eax = 要恢复的栈指针（不切换时就是同一个值）
;   add esp, 4
;   mov esp, eax
;   popa
;   iret
; --------------------------------------------------------------
irq0_timer:
    pusha
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    cld
    push esp                        ; 现场地址（PUSH ESP 压入的是 push 前的值）
    call irq0_timer_handler
    add esp, 4
    mov esp, eax                    ; 换栈：恢复选中任务的现场
    popa
    iret

; --------------------------------------------------------------
; 向量 0x21：PS/2 键盘（IRQ1）
;
; 和时钟桩的区别：**不换栈**。键盘中断只搬数据（读 0x60、解码、入队、回 EOI），
; 不累 tick、不调度，所以 `popa` + `iret` 回到的是同一个现场。
; 也不传 ctx：处理体没有返回值。
; --------------------------------------------------------------
irq1_keyboard:
    pusha
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    cld
    call irq1_keyboard_handler
    popa
    iret

; --------------------------------------------------------------
; 向量 0x30：软中断让出（任务里 `int 0x30` 主动切走）
;
; 和设备中断的区别：这里不回 EOI、不累加 tick，只是借“中断门压现场”的
; 机制把自己也变成一次可以换栈的调度点。CPU 同样把 eip/cs/eflags 压在
; **当前任务自己的栈**上，所以 rest 的处理方式和 IRQ0 桩完全一样。
; --------------------------------------------------------------
isr_yield:
    pusha
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    cld
    push esp
    call task_yield_handler
    add esp, 4
    mov esp, eax
    popa
    iret

; --------------------------------------------------------------
; 向量 0x31：调度参数系统调用（eax=字段号、ebx=新值，返回 eax=旧值）
;
; 与 0x30 的区别：不换栈、不调度，处理体只是改几个数并把旧值写回现场里的
; eax。现场形状与 0x30 完全一致，所以 `popa` 会把改好的 eax 带回任务。
; --------------------------------------------------------------
isr_tune:
    pusha
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    cld
    push esp                        ; 现场地址（pusha 之后，eax 在 ctx[7]）
    call tune_syscall
    add esp, 4
    popa
    iret

; --------------------------------------------------------------
; GDT 调用门的目标桩（GDT 第 3 项，选择子 0x18，DPL=3，参数个数 0）
;
; 与中断门的三点不同：
;   1) 进来时栈上是“返回的 CS:EIP”这对双字（远返回用），而不是 eflags/CS/EIP，
;      所以收尾是 `retf` 而不是 `iret`；
;   2) 门描述符的 DPL=3 管的是“谁能 call 它”。从 ring0 调进来 CPL 不变、也不换栈；
;      从 ring3 调进来才会换栈，而新栈来自 TSS 的 ss0:esp0（见 arch/tss.c）——
;      没有 ltr 过的 TSS，这一步就是 #GP(0)；
;   3) 参数仍不从栈上复制（描述符里的参数个数是 0），但 ABI 已经不是“两个寄存器”
;      了：eax = 指向 {field, value} 的**用户指针**，由 C 侧按“调用者特权级”校验
;      （ring3 来的指针必须落在用户窗口内、4 字节对齐，否则直接拒、绝不解引用）。
; --------------------------------------------------------------
GLOBAL tune_gate_entry
EXTERN tune_gate_call

tune_gate_entry:
    pusha
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    cld
    push esp                        ; 现场地址（pusha 之后，eax 在 ctx[7]）
    call tune_gate_call
    add esp, 4
    popa                            ; 处理体已把返回值写回 ctx[7]，popa 正好把它弹回 eax
    retf                            ; 弹掉门压进来的 CS:EIP（回 ring3 时 CPU 会再弹 ESP:SS）

; ==============================================================
; ring3：进用户态 / 回内核 / 用户态那段代码本身
;
; 三块东西各解决一个问题：
;   user_enter     —— 从 ring0 iret 到 ring3（帧是 SS、ESP、EFLAGS、CS、EIP）
;   user_stub      —— 用户态代码：连走三次调用门，把证据写在自己的栈页上，
;                    最后一次用“结束标记”让内核把我们接回去
;   user_return_stub —— 内核接管点：门处理体把返回帧改写成“回内核”之后，
;                    retf 就落到这里，再把 esp 换回进 ring3 之前的内核栈
;
; 为什么 eflags 分两种：
;   - main 的 ring3 自检（cur=0）仍然 **IF=0**。那条路上 main 的“内核栈”有两张
;     （自己的栈 + TSS 默认栈），一旦被时钟打断，schedule() 会把 main 的 esp
;     记成这张默认栈上的地址，而它本该是自己的栈——所以这几十条指令里关中断。
;   - 任务上下文里的 ring3（cur != 0）这次 **IF=1**：每个任务已经有自己的
;     esp0 栈（见「每任务一套用户页」），时钟断进来落的是这张栈，schedule()
;     记的也是这个任务自己的现场，于是用户态真的能被抢占（tick 在 ring3 里涨）。
; 判断放在 user_enter 里读一次 `cur`，两条路的 eflags 各自不同。
; ==============================================================
GLOBAL user_enter
GLOBAL user_return_stub
GLOBAL user_stub
GLOBAL user_stub_end

EXTERN cur                          ; 当前任务槽位（sched/task.c）
EXTERN user_kret                    ; 每槽位一份“进 ring3 之前的内核 esp”
EXTERN pgd_tab                      ; 每槽位一份页目录（mm/pgdir.c，非 static）
EXTERN cr3_tab                      ; 每槽位“现在装着哪张表”的账（同上）

SECTION .text

; cdecl: void user_enter(unsigned long entry, unsigned long user_esp)
;
; 只用**调用者保存**寄存器（eax/ecx/edx）：C 调用方在这之后还要接着用
; 自己的 ebx/esi/edi（例如 exec 的 `j` 就在 ebx 里），这里改了它们、
; 而函数边界只按 cdecl 传参，编译器不会为"asm 函数偷改 ebx"买单——
; 一改就是"证据写进低端内存、job 全是 0"这种莫名其妙的下游症状。
user_enter:
    push ebp
    mov ebp, esp
    ; 保存 callee-saved：ring3 那段代码会把 ebx/esi/edi 当自己的寄存器用
    ; （探针就是个例子：uprog4 拿 esi 当字符串指针），而这些寄存器里可能
    ; 正躺着调用者（exec 的 body）的局部变量。历史上这里只约定"本函数不改
    ; 它们"，但**不改不等于能保住**——用户态会改。少这三条 push，回内核
    ; 之后 body 里的 `j` 就变成了 argv 里某个指针，症状是 job 的字段被写进
    ; 用户栈（magic 读回 0）。这三条必须与 user_return_stub 的三条 pop 配对。
    push ebx
    push esi
    push edi
    mov eax, [ebp + 8]              ; 用户入口地址（由内核拷到用户页里的那段代码）
    mov edx, [ebp + 12]             ; 用户栈顶
    mov ecx, [cur]                  ; 现场归**这个槽位**：不再是一张全局格子
    mov [user_kret + ecx * 4], esp  ; 记下内核栈（这里压着 edi/esi/ebx/ebp/返回地址）

    ; 换上这个槽位自己的页目录。slot 0 的表项是 0（表示"用内核那张"），
    ; 所以 main 的老路径一行不动。装的是**物理地址**：CR3 只认页框号。
    ; 下面两条 push/pop 只是为了腾出 eax/edx：Entry 与 user_esp 还得原样用。
    push eax
    push edx
    mov eax, [pgd_tab + ecx * 4]
    test eax, eax
    jz .no_cr3
    mov cr3, eax
.no_cr3:
    pop edx
    pop eax

    mov cx, 0x2B                    ; USER_DS
    mov ds, cx
    mov es, cx
    push dword 0x2B                 ; SS3
    push edx                        ; ESP3（用户栈顶）
    mov edx, 0x00000002             ; 默认：只留 bit1（IF=0），main 的自检沿用老行为
    mov ecx, [cur]
    test ecx, ecx
    jz .push_frame
    mov edx, 0x00000202             ; 任务里的 ring3：IF=1，时钟能打断用户态
.push_frame:
    push edx                        ; EFLAGS
    push dword 0x23                 ; CS3
    push eax                        ; EIP3
    iret                            ; 到这儿开始就是 ring3 在跑

; 内核接管点：门处理体把帧改成 (eip=user_return_stub, cs=0x08)
; 换回“**这个槽位**进 ring3 之前的内核栈”，所以被抢占过再回来也对得上。
user_return_stub:
    mov eax, [cur]
    mov esp, [user_kret + eax * 4]
    ; 回到内核页目录，并把这格账清成 0（下次进 ring3 由 user_enter 重新装）。
    ; 顺序很重要：先换好 esp（这是内核栈，用旧表也访问得到），再换表；
    ; 换表之后紧接着的 pop / ret 用的仍是同一张栈，而两张表对
    ; 0..4MB 的恒等映射是一致的，所以这条链不断。
    mov ecx, eax
    xor eax, eax
    mov [cr3_tab + ecx * 4], eax
    mov cr3, eax
    ; 把 user_enter 保存的 callee-saved 还回去（顺序与 push 相反）。
    ; 少了这三条，调用者的 ebx/esi/edi 就是 ring3 留下的值——见 user_enter 处的注释。
    pop edi
    pop esi
    pop ebx
    pop ebp
    ret                             ; 返回到 user_enter 的调用者（C 代码）

; 用户态代码（会被内核整段拷到用户页去执行，所以必须位置无关）
; 约定：eax = 指向 {nr, a0, a1, a2} 的用户指针（见 include/sys/syscall.h）
;       nr = 0 是 tune（a0 = 字段号、a1 = 新值），门返回 eax = 旧值 / 0（被拒）
user_stub:
    sub esp, 32                     ; 请求体 [esp..esp+12]，结果区 [esp+16..esp+24]
    mov dword [esp], 0              ; nr = SYS_NR_TUNE
    mov dword [esp + 4], 2          ; a0 = TUNE_FIELD_RESTORE
    mov dword [esp + 8], 40         ; a1 = 40（门的上限 256 之内，放行）
    mov dword [esp + 12], 0         ; a2
    mov eax, esp
    call 0x18:0                     ; 走调用门：这里会换栈到 TSS.esp0
    mov [esp + 16], eax             ; 回到 ring3 了：把返回值写进自己的结果区

    mov dword [esp + 8], 5000       ; 超过每字段上限 256：应被拒
    mov eax, esp
    call 0x18:0
    mov [esp + 20], eax

    mov eax, 0x04000000             ; 指向**未映射**的地址（也不在用户窗口）：
                                    ; 没有窗口校验的话，内核下一次 deref 就是 #PF
    call 0x18:0
    mov [esp + 24], eax

    mov dword [esp], 0xFF           ; 老旁路：不回用户了，请内核接回去
    mov dword [esp + 4], 0
    mov eax, esp
    call 0x18:0
    jmp $                           ; 正常不会走到这里
user_stub_end:

; 第二段用户代码：越权探测。目标地址由内核写在用户栈顶下面一格（[esp-4]），
; 所以同一段代码可以拿不同的地址反复用——多探几次不需要多拷几份代码。
;
; 为什么读 [esp-4]：进入 ring3 时 ESP 正好是 user_esp0，而那张用户栈页
; 是内核刚放行的，ring3 读得到；内核在每次 user_enter 之前把要探的地址
; 写进那一格，就不用把地址编进指令里（也就保持了位置无关、可重复使用）。
;
; 若这一句没被拦（说明 U/S 没收口），代码会走到下面的 0xFF 结束标记，
; 内核读回 err=0 —— 那个 0 就是“没拦住”的指纹。
GLOBAL user_probe
GLOBAL user_probe_end
user_probe:
    mov ecx, [esp - 4]              ; 目标地址（内核给的）
    mov eax, [ecx]                  ; 越权读它：应当 #PF（err=0x05 = P|U、读）
    sub esp, 16                     ; 四字请求体（门处理体一次读 16 字节）
    mov dword [esp], 0xFF
    mov dword [esp + 4], 0
    mov dword [esp + 8], 0
    mov dword [esp + 12], 0
    mov eax, esp
    call 0x18:0
    jmp $
user_probe_end:

; 第三段用户代码：端口 I/O 探测。
;
; TSS 的 iomap_base = 结构大小（= “这张 TSS 里没有位图”），于是 CPL3 的
; `in`/`out` 一定在“查位图”那一步越界，得到 #GP(0)。被拦时
; exception_handler_gp 把返回帧改成“回内核”，内核看到 ring3_io_deny=1。
;
; 若没被拦（说明 iomap 摆错成“有表且允许”），会接着走下面的结束标记，
; 内核读回 ring3_io_deny=0 —— 那个 0 就是“口子开着”的指纹。
; 这里不依赖用户栈：真被拦时栈根本用不上；没被拦时这一句 `sub esp` 会先撞上
; 已经关掉的栈页（#PF 也是“回内核”，同样留下 0）。
GLOBAL user_io
GLOBAL user_io_end
user_io:
    in al, 0x60                     ; 读 8042 数据口：ring3 下必须 #GP(0)
    sub esp, 16
    mov dword [esp], 0xFF
    mov dword [esp + 4], 0
    mov dword [esp + 8], 0
    mov dword [esp + 12], 0
    mov eax, esp
    call 0x18:0
    jmp $
user_io_end:


