# mini-os

基于 Linux 0.11 引导阶段（bootsect / setup / head）的 NASM 移植 + 最小 C 内核。
当前状态：**能从软盘启动，进入 32 位保护模式、开启分页，进入 C `main()` 打印信息；
8254 已被重编程到 100Hz（1 tick = 10ms）并用 RTC 秒边界交叉校验频率；
除零 / 通用保护 / 缺页三类异常都有真实入口；
缺页已从“打印现场后停机”升级成按需分页——补页表、补物理页、`iret` 重试原指令；
物理页分配器是位图式的，支持分配/释放/用量统计，并有“解除映射再访问”的自检；
VGA 光标已同步到 CRTC（0x0E/0x0F），位置与形状都可读回校验；
已有带状态机、**3 级优先级就绪队列 + tick 时间片 + 老化保底**、**小顶堆睡眠队列**的抢占
调度——任务各有独立栈，能 `task_sleep(ms)` 睡到指定 tick、能 `task_yield()` 主动
让出（让完仍在就绪态）、能 `task_exit()` 退出并由调度器回收栈页，还能
`task_spawn(entry, arg, prio, slice)` 在运行中带参创建任务、复用已释放的槽位**。
屏幕上的 24 行有两种用法：人看一遍，`make verify` 把每行锁成断言。
尚无用户态、文件系统与更细的公平性策略。

构建与调试的目标环境是 **Linux（x86_64 主机跑 32 位保护区内核）**，工具链为
`nasm` + `gcc -m32` + `ld -m elf_i386` + `qemu-system-i386` + `gdb`。

## 模块划分

源码按**职责**分目录，头文件按同一套模块名分组（`-Iinclude`，所以写
`#include "linux/page.h"` 而不是相对路径）。依赖方向是单向的，不许回指：

```
            init/main.c            ← 只做“把东西按顺序装起来 + 自检”，不实现任何机制
                 │
   ┌─────────────┼──────────────┬─────────────┐
 kernel/        mm/      kernel/chr_drv/    fs/
 （调度/中断）  （内存）   （字符设备）     （文件）
 kernel/blk_drv/（块设备）
   └─────────────┴──────────────┴─────────────┘
                 │
            include/linux           ← 全部内核头（0.11 就是单一 linux/ 目录）
            include/asm/io.h        ← 端口 IO 这类机器指令级的东西
```

目录名照 Linux 0.11：没有 `arch/ core/ sched/ sys/ drivers/` 这些现代分组，
`kernel/` 一个目录装下中断、异常、调度、系统调用、printk/panic、时间基准与链接脚本，
设备按 `kernel/chr_drv/` 与 `kernel/blk_drv/` 分开——0.11 的 `console.c`、`keyboard.S`、
`serial.c` 在 `chr_drv`，`hd.c`、`ll_rw_blk.c` 在 `blk_drv`，我们一一对得上。

| 模块 | 目录 | 头文件 | 职责 | 依赖谁 |
| --- | --- | --- | --- | --- |
| 引导 | `boot/` | — | 三段启动（boot→setup→head）+ 中断入口桩（nasm） | 无 |
| 体系结构 | `kernel/` | `include/linux/` | IDT 门安装、异常分发、内核栈、链接脚本 | kernel |
| 内存 | `mm/` | `include/linux/` | 位图物理页分配器（`PAGES_BASE=0x25000`，与内核映像的边界由 `__kernel_end` + `tools/kernel-fit.py` + `page_init()` 三层守住）、按需分页（补 PDE/PTE）、解除映射、U/S 收口（`page_lock_kernel` / `page_mark_kernel`）、用户页租借表（`lease.c`：`(owner, phys)` 双键 + refs + ttl 老化 + 按 owner 回收 + 撤销通知）、**内核堆**（`heap.c`：`kalloc/kfree`，懒要页 + first-fit 空闲表 + magic 校验） | kernel |
| 调度 | `kernel/` | `include/linux/` | 就绪队列、时间片、老化、衰减、睡眠队列、栈页回收、参数所有权（`dtor`）、**每任务借用表**（`leases[TASK_LEASES]`：记它借了哪几页，回收按表走）；`tune.c` 放运行时可变参数、`int 0x31` 与 GDT 调用门（DPL=3 + 白名单 + 片长次序复查） | kernel、mm |
| 外设（字符） | `kernel/chr_drv/` | `include/linux/` | VGA 文本与光标、8254 PIT（IRQ0）、CMOS RTC（`rtc.c`：秒计数 + 完整日期，含 BCD/12 小时/世纪换算与双读比对）、PS/2 键盘（IRQ1：扫描码解码 + 环形队列 + PIC 放行 + 第 24 行可编辑输入行）、COM1 串口（`serial.c`：115200 8N1，脱屏调试通道） | kernel、lib |
| 外设（块） | `kernel/blk_drv/` | `include/linux/` | **ATA PIO**（`ata.c`：主 IDE 通道两盘 IDENTIFY + LBA28 读扇区）、**块设备层**（`blkdev.c`：4 槽注册表，槽号=设备号=盘号，`base` 为分区偏移预留，越界先拒后解引用）、**MBR 分区**（`part.c`：验 0x55 0xAA、逐条查 start/count 不越盘，把主分区登记成槽 2/3） | kernel、lib |
| 基础件 | `kernel/` | `include/linux/` | `printk.c`（自带 `%s %c %d %u %0NX %t` 的极简格式化器；每个字符经 `emit()` 同时落 VGA/串口/日志环）、`klog.c`（32 行启动日志环，行首带 tick）、**`kv.c`**（机器可读证据通道：`KV <tag> k=v ...` 一行只写串口，回归脚本只认这一行，不再把屏幕排版当接口）、`stat.c`（只增不减的计数器，`stat` 命令的数据源）、`clock.c`（**墙钟**：RTC 定锚 + tick 走时 + 定期校准 + 封顶拒绝）、`panic.c`（统一停机出口）、端口 I/O 与 cli/sti/hlt、段选择子、BIOS 参数 | 无 |
| 文件系统 | `fs/` | `include/linux/` | mosfs v1（读 + 追加写 + 删除 + 多级目录）：按 Linux 0.11 的 `super / block_dev / bitmap / inode / namei` 五分法拆成 `super.c`、`blkio.c`、`bitmap.c`、`inode.c`、`namei.c`，`mosfs.c` 只留 `fs_mkdir/fs_read/fs_create/fs_write/fs_unlink/fs_rmdir` 这六个对外动作的编排；内部共享状态（`sb`/`blk`/`dent[]`/计数）收进 `include/linux/fsint.h`。**`bcache.c`**：块缓存。**`fd.c`**：文件描述符表（每槽位 4 格，`name/ino/size/off/flags/fop`），`open/read/write/seek/close` + `fd_close_all(slot)`；**它自己不再碰盘**——读写全部转手给 `fop`（`struct file_operations`），内核的 `cat` 和 ring3 走**同一条**路径。**`file_dev.c`**：普通文件的 fops（读走 open 时的 size 快照、写走追加 + 刷新 size）。**`char_dev.c`**：`/dev/tty` 的 fops（写 COM1、读键盘共享环、拒绝 seek）。**`block_dev.c`**：`/dev/hd` 的 fops（把 off 翻译成 (dev,lba,扇内偏移)，读任意长度、写只收整扇区）。**`pipe.c`**：管道的 fops（一对 fd 共享一个 512 字节环，满则短写、空则返 0）。**`elf.c`**：极小的 ELF32 装载器（只认 `PT_LOAD`，段可以跨页）。**`exec.c`**：`fs_read` 读到的字节 → 认 ELF 就搬段取 `e_entry`、否则当裸二进制 → 一段**连续**代码页（1..4 页，`page_alloc_run`）+ 一页栈 → 租约放行 → `user_enter` 进 ring3 → 指纹自证 → 页全回收（`exec_run`） | kernel、kernel、mm、kernel |
| 系统调用 | `kernel/` | `include/linux/` | 门的**按号分派**（`syscall.c`）：0 tune / 1 write / 2 exit / 3 ticks / **4 open / 5 read / 6 fwrite / 7 close**；`sys_ptr_ok` 是用户指针的唯一判据（对齐 + 窗口 + 不越界），`sys_str_ok` 是文件名的判据（字节串、不要求对齐、不许控制码）；`write` 只放行可打印字符；账目 `calls/denied/bytes` 给 `sys` 命令读 | kernel、kernel、shell、kernel、fs |
| 启动 | `init/` | `include/linux/` | `main()`：只做“初始化 + 按顺序把七个阶段装起来”；`demo.c`：自检的**演示载荷**（烧 CPU 的、睡着的、让出的、借页就死的、故意写穿栈的）连同它们的证据变量；`report.c`：自检界面的**唯一字符串来源**（一行一次 `printk`，列宽可算） | 全部 |

两条硬约束写在 Makefile 里：`head.o` 必须排在最前（它的 `.text` 起点就是页目录
`pg_dir`，物理 0），以及 C 源文件散在九个源码目录（`kernel/` 按 Linux 0.11 的
`chr_drv / blk_drv` 又分成两层）、靠 `vpath` 一条规则编译，
**目标文件仍然平铺在 `build/`**——链接顺序比目录好看更重要，不能让 make 的
目录遍历顺序决定内核内存布局。

这次拆分**不改一行逻辑**，只改文件位置与 `#include` 前缀；拆分当时前后
`kernel.bin` 都是 34106 字节，屏幕账单逐位相同（`esp` 与符号地址除外，因为
目标文件的链接顺序变了）。此后又加了睡眠堆、衰减与 guard 页、TSS/ring3、
双页控制台与一大堆自检代码，再加上串口、日志环、内核堆、ATA、mosfs、exec（已挂到任务上）与系统调用表，
当前映像是 114408 字节（`__kernel_end=0x218A0`，分配区余量 5984 字节），
屏幕上的数值以本文后面“实测屏幕”那一块为准——也正因为长得这么大，
映像尾部一度跨进了物理页分配器（见「内核映像 vs 页分配器」那一节）。

## 屏幕排版：`printk` 格式化 + 列宽检查

自检那一屏的布局从“几十个 `printk` + 手工 `vga_print_hex` 拼一行”收成了
**一行一个格式串**，并且字符串集中在一个文件里：

| 位置 | 职责 |
| --- | --- |
| `kernel/printk.c` | 只做“字节流怎么落到屏幕”：`%s %c %d %u %x %X %0NX`（`%08X` 即原来的 `vga_print_hex`，`%02X`/`%03X`/`%01X` 对应原来的 `print_hex2`/`print_hex3`/`print_small_hex`） |
| `init/report.c` | 只做“数摆在哪一列”：`rep_*` 每个函数打一整行，取位（`& 0xF` / `& 0xFF`）在这里做 |
| `tools/linewidth.py` | 只做“这一行会不会换行”：按格式串的定宽符算列数，>79 列直接失败；**覆盖所有会 `printk` 出整行的目录**（`init core arch mm sched drivers shell`，28 行） |
| `Makefile` 的 `all` | 先 `width` 再 `image`：超宽在编译前就被拦下，不用等 QEMU 跑完才发现 |

为什么一定要“定宽”：文本模式一行 80 格，正好 80 列时行尾那个 `'\n'` 会把
光标顶到下一行，凭空多一行，第 25 行触发上滚，banner 被挤出屏幕（这坑踩过两次）。
所以界面里的十六进制一律写成 `%08X` / `%03X` / `%02X` / `%01X`：
`linewidth.py` 直接把 `%08X` 记 8 列，`%X`（不给宽度）按 8 列的最坏情况算。
`s` 按 0 列算，只允许用在“二选一短语”这种窄字段上（`(protection)/(not-present)` 那一处）。

```
$ make            # 先跑列宽检查，再编译
列宽检查通过：28 行，全部 ≤ 79 列
...
```

覆盖范围原来只有 `init/` + `kernel/exception.c`（27 行）。后来 `mm/page.c` 的
`[mem] FATAL`、`kernel/chr_drv/` 的告警、`shell/` 的 `printk` 也都可能落满一行，于是
改成“谁的 `printk` 带 `'\n'` 就查谁”——一次从 27 行扩到 28 行，**没有一行超宽**，
说明扩的范围本来就没有藏问题，但以后写超了会在 `make` 阶段就被拦下。

这次重构的两个坑，都记在这里免得再犯：

1. **槽位会被下一阶段复用**。原来阶段一的 `U/V` 是“跑完立刻打”，搬到一行合并打印
   之后变成了“阶段二跑完才读”，读到的其实是阶段二 H/L 的数字（同一批槽位号被
   `task_reset()` 收回再用）。症状很有辨识度：`p1 U=0x09 V=0x02 | p2 H=0x09 L=0x02`
   两组一模一样。修法是跑完就把数抄进局部变量（`u_ticks` / `v_ticks`），别等到打印时再读。
2. **`esp` 会随代码长度漂移**。`row 1` 的 `esp=0x...` 是栈指针，改任何一处代码都会变，
   所以它进了 `tools/verify.py` 的抖动白名单（`esp=0x[0-9A-F]+ → esp=0x<N>`），
   和 `cnt` / `dtick` / `tick` / `ms` 一样按占位符比对。

## `init/main.c` 的第二刀：把演示载荷搬进 `init/demo.c`

`main.c` 一度长到 1119 行，其中 300 行是“自检用的临时演员”——烧 CPU 的、睡着的、
让出的、借页就死的、故意写穿栈的。这些代码有个共同点：**它们存在的理由是给别人
制造证据**，和“把内核装起来”这件正事没有关系。于是分成两块：

| 文件 | 内容 |
| --- | --- |
| `init/main.c`（783 行） | 只留编排：初始化顺序、七阶段之间的先后与等待、最后的“窗口外写” |
| `init/demo.c`（约 380 行） | 十个载荷入口 + 它们各自的证据变量（全 `static`），对外只经 `include/linux/demo.h` 的取数函数说话 |

为什么证据变量也一起搬走，而不是留在 `main.c` 里“顺手读一下”：上一轮踩过的
“槽位复用读后值串”（见上一节第 1 条）根源就是证据散落在编排代码旁边、随手
就能读早读晚。现在 `main.c` 里根本拿不到 `own_seen` 这类东西，只能
`demo_own_seen()`，取数时机被挤到“确实需要打印”的那一刻。

顺带把两个演示专用的薄包装也从 `main.c` 挪进 `kernel/tune.c`：`tune_field_set`
（`int 0x31`）与 `tune_gate_set`（`lcall $0x18,$0`，局部 `struct tune_req`）。
它们本来就是对 `tune` 机制的两个调用入口，和 `tune.c` 里的白名单检查放在一起，
“谁能改参数”这个问题只需要看一个文件。

验收标准只有一条：**屏幕账单一位不变**。拆分后 `kernel.bin` 从 55412 涨到
55732 字节（多了函数边界和调用），`verify.sh -n 2` 两轮结构与区间断言全通过，
`shell-console` / `shell-vars` / `kbd-shell` / `ring3-io` 四个脚本全 PASS，
`row 24` 的键盘输入行、`ovf=0x00` 都没动。

## 可观测性：计数器 + `stat` 命令 + 运行期排版兜底

三条线索合成一套“随时问一下”的观测手段，各自解决一个具体问题：

| 手段 | 位置 | 回答的问题 |
| --- | --- | --- |
| `kernel/stat` 计数器 | `kernel/stat.c` / `include/linux/stat.h` | 这些机制**到底被执行了几次**（不是“代码里写了”） |
| `stat` 命令 | `shell/cmd_info.c` 的 `cmd_stat` | 现在就给我看那张表（不用重启、不用抓屏） |
| `ovf` 终值检查 | `kernel/chr_drv/vga.c` 的 `line_len` → `main` 收尾 | 这一屏的排版有没有在运行期被撑破（正常路径屏幕上根本不出现；一出现就整屏错位） |

**计数器**是“只增不减”的定长数组，六个槽：

```
pf       补上的缺页         kernel/exception.c  page_fault_map 成功
pf_fail  补不上的缺页       kernel/exception.c  判 unhandled 之前
alloc    物理页分配         mm/page.c         page_alloc / page_alloc_pair
kill     被守卫杀掉的栈溢   kernel/exception.c  task_guard_victim 命中
cmd      shell 分派的命令   shell/shell.c     命令表命中（敲错的不算）
ovf      排版溢出           kernel/chr_drv/vga.c     一行写满 80 列才换行
```

打点的时候有个取舍：**这些数分属 mm / arch / drivers / shell 四个模块，
但它们要出现在同一张表上**。所以存储只放在 `kernel/stat.c` 一处，各模块自己
`stat_inc()`，谁的接口都不必为了“多一个计数器”而改签名——加第七个计数器
只要动 `stat.h` 的枚举和 `stat.c` 的短名表。

`stat` 命令一行摆完（`cmd_stat` 用 `hex3()` 自己拼字符，不绕 `printk`）：

```
> stat
stat pf=0x003 pf_fail=0x001 alloc=0x01E kill=0x001 cmd=0x011 ovf=0x000
```

三个“从数字就能看出机制成立”的对照：`pf=3` 正好是两次按需分页 + 一次解除映射后
重新缺页；`pf_fail=1` 是最后那个故意写到窗口外的访问；`alloc` 大于 `pf`
说明补页之外还有任务栈/页表在用同一个分配器。

**为什么屏幕上不再常显 `ovf=`**：24 行预算已经算死（见上一节），而 `ovf` 是个
只增不减的计数器——在 `row 5` 打印时它只记着“前六行的账”，却很容易被读成“整屏的账”，
半截快照当终值用是这个屏幕最容易自欺的地方。于是改成**失败才说话**：

```
layout_ovf = stat_read(STAT_OVF);      /* 整屏打完、切页之前读一次终值 */
if (layout_ovf)
    rep_layout_alarm(layout_ovf);      /* 落在 row 24，上滚一行 */
```

违规时这一行会落在 `row 24`，行尾的 `\n` 让整屏上滚一行，`verify` 的 24 行基准
随即整体错位报红——不需要谁去盯某个字段，页面自己会喊。正常路径上这一行根本不出现，
`row 5` 因此腾出 9 列，把 `irq` 从 1 位放宽到 2 位（时钟跑满一秒就到 100 次，
1 位只会永远显示 `0x0`）。运行期仍确认 `stat` 里的 `ovf=0x000`（`shell-console.sh` 断言）。

运行期兜底和编译期检查是互补的，不重复：

- **编译期**：`tools/linewidth.py` 把格式串按定宽符算成列数，超 79 直接让 `make` 失败。
  它现在扫**整个 `init/` 目录**（`report.c` + 编排代码里临时加的 printk）加 `kernel/exception.c`，
  只认“一次 printk 一行”的写法；`shell/` 与 `kernel/chr_drv/` 不在其中，它们的字符串是运行期拼的，
  静态算不出来，靠 `SHELL_OUT_COLS` 在运行时兜底。
- **运行期**：`vga_putc` 记着“本行已写多少列”，`\n` 时若已 ≥ 80 就 `stat_inc(STAT_OVF)`。
  它挡不住事故，但**保证事故不会无声发生**：任何新增的打印路径超宽了，整屏都会错位。

键盘回显直接写显存、不走 `vga_putc`，所以输入行不进 `ovf` 计数——它写的是第 24 行，
另有自己的宽度约束（`kbd_set_echo_mirror` 那条路径）。

## 内核映像 vs 页分配器：一条被跨过的边界

这次“改一个显示字段”意外挖出一个真 bug，值得单独记一节。

**现象**：把 `row 5` 的 `ovf` 挪走之后，`verify` 的 24 行基准突然错位——屏幕上
只剩 17 行，`banner` / `[cpu]` / `[idt]` … 前 7 行整段消失，`[mem] PF` 那一行跑到
`row 0`。看上去像“上滚了 7 行”，但 `row 5` 的内容没变、`poll` 的计数器也都正常。

**定位**：在 `rep_demand`、`rep_read`、`rep_touch`、`rep_phase12` 四个入口各下一次
断点、各 dump 一次 `0xb8000`，看到的是**第二遍覆写**：`cursor` 在 `rep_demand`
之后变回 0，于是后面的每次 `printk` 都从 `row 0` 开始往下盖，把 banner 那一段
逐行吃掉。读 `nm`：

```
000106e0 b argv_len        ← shell 的静态变量
00010780 b states
```

内核映像（含 `.bss`）已经长到 `0x10780`，而 `mm/page.c` 里分配器的起点还是
`PAGES_BASE = 0x10000`。分配器分配一页的第一件事就是 `zero_page(0x10000)`——
**它把 vga 的 `cursor` / `line_len`（正好在 `0x10010`）抹成 0**。屏幕上那句
“光标回 0”，就是这么来的。

这条边界一直存在，只是之前内核还装得下：`0x10000` 是“内核约 0x8000 结束”时留下的
老数字，映像一点点长大，这次终于跨过去了。**跨过去之前它不报错，跨过去之后它不报错、
只是屏幕乱了**——这正是必须把它变成硬约束的原因。

**修法**（两层，缺一不可）：

| 层 | 位置 | 做法 |
| --- | --- | --- |
| 链接期 | `kernel/link.ld` | `.bss` 之后导出 `__kernel_end`，成为“内核最后一字节”的唯一权威 |
| 构建期 | `tools/kernel-fit.py`（`make` 在 `objcopy` 前调用） | `__kernel_end` ≥ `PAGES_BASE` 直接失败；离边界不足一页也失败（留一页余量，防下次加个变量又踩） |
| 启动期 | `mm/page.c` 的 `page_init()` | 用同一个 `__kernel_end` 再验一遍，越界就打 `[mem] FATAL:` 并 `kernel_halt()`——**宁可停机，不许踩** |

`PAGES_BASE` 随之从 `0x10000` 抬到 `0x20000`：分配区 `0x20000–0x90000`，总页数
从 128 变成 112（`0x70`），于是屏幕上 `frames=…/0x00000070`、demo 分配的物理页
从 `0x11000` 变成 `0x21000`（`row 7/9/10/11/22` 一起变）。基准文件相应更新，
`verify.py` 里写死的 `frames=…/0x80` 与 `irq=0x1` 两处字面量也一并改了——
**基准文件也是代码**，改布局就要改断言。

```
$ make
列宽检查通过：27 行，全部 ≤ 79 列
内核装得下：__kernel_end=0x000110F0，分配区从 0x00020000 起（余量 61200 字节）
```

## ring3 的边界：端口 I/O 位图 + 可恢复的 #GP

`ring3` 能跑起来之后，下一个问题是“它能碰什么”。段权限管住了内存（越权访问
→ #PF，见 row 14 的 `knl`），但**端口**是另一套开关：`in`/`out` 由 TSS 的
I/O 位图决定，和段无关。

做法是**不给位图**：`tss.iomap_base` 填成 TSS 结构大小本身（0x68）。CPU 查位图时
第一位就超出段界限，于是 CPL3 的任何 `in`/`out` 直接 `#GP(0)`。给了位图就等于
把 8259/8254/CRTC 全交给用户态——位图是白名单，没有位图才是最严的那种。

只把位图摆对还不够，还得让 `#GP` **可恢复**：13 号向量原来是
`exception_handler` → `kernel_halt`，ring3 一碰端口整个内核就死在半路，自检走不完、
键盘也再看不见。所以 13 号也改成 ctx 风格（与 14 号同一套约定），由
`exception_handler_gp` 分两种：

| 现场 | 处理 |
| --- | --- |
| `cs & 3 == 3` 且错误码 `0` | ring3 的端口访问被拒 → 记 `ring3_io_deny = 1`、`stat_inc(STAT_IO)`，返回帧改成“回内核”（`eip = user_return_stub`、`cs = KERNEL_CS`） |
| 其余（内核自己踩到） | 原样 `exception_handler(13, err)`：打印现场 + 停机 |

判据为什么是“错误码 0”：`#GP` 的错误码在“选择子不合法”时是那个选择子，
在“权限/位图被拒”时是 0。配合 `cs` 低两位，两条一起才既不误杀内核 bug，
也不把真正的越权放过去。

用户态那段代码在 `boot/intr.asm` 的 `user_io`（跟 `user_probe` 同属“连着的一段
用户代码”，一次拷进用户页）。自检里它排在四次越权探测之后、租约超时回收之前——
代码页那时还开着，否则取指就先缺页了。

```
$ bash tools/ring3-io.sh
  已到 kernel_idle（EIP=0x00006b69）
  ring3_io_deny=1（期望 1）
PASS  ring3 端口探测：I/O 位图拦下 in、#GP 收回内核、自检走完
```

三个断言各管一件事：**still 到 kernel_idle**（说明 #GP 被收回而不是停机）、
**`ring3_io_deny == 1`**（说明拦住了、判据成立）、**证据页 row 14 与 row 23 都还在**
（说明这一趟没有把后面的自检打断）。反过来的指纹也留了：如果 iomap 被摆成
“有表且允许”，`user_io` 会接着走结束标记，内核读回 `0` —— 那就是“口子开着”的证据。

`stat` 里能看到这次拦截（`io` 槽）：

```
> stat
stat pf=0x003 pf_fail=0x001 alloc=0x01E kill=0x001 cmd=0x011 io=0x001 ovf=0x000
```

## 目录结构

```
mini-os/
├── boot/                 # 引导三段 + 中断桩（nasm，不在 C 构建里）
│   ├── boot.asm          # 引导扇区：自搬移至 0x90000 -> 加载 setup -> 加载内核 -> 跳 setup
│   ├── setup.asm         # 收集 BIOS 参数到 0x90000、内核搬到 0、开 A20、重映射 PIC、进保护模式
│   ├── head.asm          # 32 位入口：GDT/IDT 初始化、A20 校验、分页（16MB 恒等映射）、跳 main
│   ├── uprog.asm         # `exec` 的示例用户程序（位置无关裸二进制，nasm -f bin → build/uprog.bin）
│   ├── uprog2.asm        # 跨两页的示例程序（.bss 在 0x1800，交给 tools/uprog2.ld 链成跨页 ELF）
│   ├── uprog3.asm        # ring3 忙等探针（TSS.esp0 跟着任务走的现场证据，烧 CPU 不烧 tick）
│   ├── badprog.asm       # 负向用户程序：传窗口外指针、不写指纹（证明门与哨兵都在拦）
│   ├── uprog6.asm        # 高地址探针：ring3 读 16MB 以上，证明"用户态 #PF 一律越权"
│   ├── uprog7.asm        # 只读代码段探针：ring3 写自己的代码页，证明用户页写保护在拦
│   └── intr.asm          # 中断入口桩：除零 / GP / 缺页 / 时钟 / 让出 + 调用门桩 + 进/出 ring3 的跳板与用户态代码
├── init/
│   ├── main.c            # C 入口：自检打印、装异常/时钟门、等 tick、触发缺页、四阶段调度演示
│   ├── demo.c            # 自检的演示载荷（烧 CPU / 睡着 / 让出 / 借页就死 / 故意写穿栈）+ 证据变量
│   └── report.c          # 自检界面的唯一字符串来源（一行一次 printk，列宽可算）
├── kernel/               # 与设备无关的一切（0.11 的 kernel/ 就是这个粒度）
│   ├── idt.c             # 原地改写 IDT 项并重载 IDTR
│   ├── exception.c       # 异常处理：向量名、错误码、缺页补页后重试，其余 panic
│   ├── stack.c           # 内核栈与 stack_start
│   ├── tss.c             # 32 位 TSS + GDT 里摆 ring3 代码/数据段 + ltr（ring3 换栈的前提）
│   ├── link.ld           # 链接脚本：head.o 的 .text 必须位于物理 0
│   ├── task.c            # 任务的一生：建栈/换栈/死亡/延后回收，任务表与借用表，schedule() 的决策
│   ├── kwait.c           # 等事件的运行期自检：两个 worker 用一个信箱 ping-pong（sleep_on/wake_up 证据）
│   ├── queue.c           # 谁下一个跑：三级就绪队列、睡眠小顶堆、老化/唤醒、时间片与历史惩罚
│   ├── tune.c            # 运行时可变参数 + int 0x31 + GDT 调用门（DPL=3、白名单、片长次序复查）
│   ├── syscall.c         # 门的按号分派（tune/write/exit/ticks/open/read/fwrite/close）+ 用户指针判据
│   ├── printk.c          # 极简格式化器（%s %c %d %u %0NX %t）；每字符经 emit() 落 VGA/串口/日志环
│   ├── panic.c           # 统一停机出口（先切回证据页再停）
│   ├── klog.c            # 32 行启动日志环，行首带 tick
│   ├── kv.c              # 机器可读证据通道：KV <tag> k=v 一行只写 COM1
│   ├── stat.c            # 只增不减的计数器，stat 命令的数据源
│   ├── clock.c           # 墙钟：RTC 定锚 + tick 走时 + 定期校准 + 封顶拒绝
│   ├── vector.c          # 真实向量表 + ignore_int 的落地与命中计数
│   └── layout.c          # 布局契约：结构体偏移的唯一定义处
├── kernel/chr_drv/       # 字符设备：按字节、按事件（0.11 的 chr_drv）
│   ├── vga.c             # VGA 文本输出 + printk 落地 + 十六进制打印 + 硬件光标同步
│   ├── timer.c           # 8254 重编程到 100Hz、IRQ0 处理（EOI + tick 计数 + 调度）、PIC 解屏蔽
│   ├── rtc.c             # CMOS/RTC：秒计数 + 完整日期（BCD/12 小时/世纪 + 双读比对）
│   ├── serial.c          # COM1：115200 8N1，脱屏调试通道（printk 每个字符都往这儿抄一份）
│   ├── keyboard.c        # PS/2 键盘的硬件面：扫描码集 1 两张字符表、扩展码状态机、IRQ1 处理体
│   ├── kbd_queue.c       # 每 owner 一条环：归属选择、深度/丢弃/峰值统计
│   └── kbdline.c         # 第 24 行输入行：line_buf + 4 条历史 + 提交环，唯一往第 24 行写字的路径
├── kernel/blk_drv/       # 块设备：按扇区、要缓存（0.11 的 blk_drv）
│   ├── ata.c             # 主 IDE 通道两盘 IDENTIFY + LBA28 读扇区（PIO，轮询不用中断）
│   ├── blkdev.c          # 4 槽块设备注册表：槽号=设备号，base 留给分区偏移，越界先拒
│   └── part.c            # MBR 分区：验 0x55 0xAA、逐条查 start/count 不越盘，登记成槽 2/3
├── mm/
│   ├── page.c            # 位图物理页分配器：单页/连续两页、释放、用量、内核边界检查
│   ├── paging.c          # 页表项的写入者：按需分页补 PDE/PTE、guard 撤/恢复、U/S 收放
│   ├── pgdir.c           # 每任务页目录：slot 1..4 各一张表、放行/收回用户页、切表记账
│   ├── lease.c           # 用户页租借表：(owner,phys) 双键、refs、ttl 老化、按 owner 回收
│   ├── heap.c            # 内核堆：kalloc/kfree，懒要页 + first-fit 空闲表 + magic 校验
│   └── aspace.c          # 每任务地址空间的描述与切换（任务与 pgdir 之间的那层）
├── fs/
│   ├── super.c           # 超级块校验链 + 挂载 + 全局状态（sb / fs_dev / 计数 / dent[]）
│   ├── blkio.c           # 盘上缓冲 blk + 大端 u16/u32 存取 + blk_read/blk_write + blk_lookup/ensure
│   ├── bitmap.c          # 数据块位图：alloc / clear（一个位图位 = 一个 512 字节块）
│   ├── inode.c           # inode 的读写与生命周期：inode_read/write/alloc/zero/free
│   ├── namei.c           # 名字查找：根目录缓存、目录项遍历、path_split、dir_add/del
│   ├── mosfs.c           # 只做编排：fs_mkdir / fs_read / fs_create / fs_write / fs_unlink / fs_rmdir
│   ├── bcache.c          # 块缓存：几个 512 字节缓冲挂在 blkdev 上，命中/未命中都记账
│   ├── fd.c              # 文件描述符表 + 名字分派（/dev/* → 设备表，其余走盘）
│   ├── file_dev.c        # 普通文件的 fops：read/write/lseek/release
│   ├── char_dev.c        # 字符设备的 fops：/dev/tty（写 COM1、读键盘共享环）
│   ├── block_dev.c       # 块设备的 fops：/dev/hd（off → (lba, 扇内偏移)）
│   ├── pipe.c            # 管道的 fops：一对 fd 共享一个 512 字节环（非阻塞、短写）
│   ├── elf.c             # 极小的 ELF32 装载器：PT_LOAD 搬段、清 .bss、返回 e_entry（段可跨页）
│   └── exec.c            # 盘上的字节 → 一段连续用户页 + 栈页 → ring3 → 由任务的 dtor 收回
├── lib/
│   ├── string.c          # memcpy/memset/...（必须 -fno-builtin，否则 -O2 会自己调自己）
│   └── time.c            # 时间换算：tick ↔ 日历，与 RTC 那两路对齐的那一侧
├── shell/                # 自造：内核里的命令行（0.11 的 shell 是用户态程序，这里没有对应）
│   ├── shell.c           # 提交行的消费者：行协议（空白/引号/参数上限）+ 命令表 + 分派 + `!!`
│   ├── state.c           # 每个 owner 一份的持久态、参数缓冲、证据访问器
│   ├── var.c             # 变量表：set / unset / `$name` 展开的存放与查找
│   ├── console.c         # 8 行滚动内存输出 + VGA 第 1 页（唯一直接写 0xB9000 的地方）
│   ├── fmt.c             # 十六进制/十进制/有符号/字面量与返回码（30 条命令共用）
│   ├── cmd_fs.c          # disk / fs / ls / cat / fd / write / rm / dev
│   ├── cmd_prog.c        # pgd / exec / sys（程序与地址空间）
│   ├── cmd_info.c        # stat / irq / tasks / log / heap / kstress / panic（纯观测）
│   └── cmd_time.c        # clock / date（时间类，含只服务 clock skew 的 parse_int）
├── include/              # 头文件：0.11 只有 linux/ 与 asm/ 两个目录
│   ├── linux/            # 50 个内核头，平铺（不再按 core/arch/sched/... 分组）
│   │                     #   调度：task.h taskint.h tune.h
│   │                     #   内存：page.h pageint.h pgdir.h lease.h aspace.h heap.h
│   │                     #   文件：mosfs.h fsint.h fd.h exec.h elf.h bcache.h
│   │                     #   设备：vga.h timer.h rtc.h serial.h keyboard.h kbd_queue.h kbdline.h
│   │                     #         ata.h blkdev.h part.h
│   │                     #   体系结构：idt.h intr.h exception.h stack.h tss.h
│   │                     #   输出与观测：printk.h panic.h klog.h kv.h stat.h vector.h
│   │                     #   命令行：shell.h state.h console.h var.h cmds.h fmt.h
│   │                     #   其余：kernel.h bootparam.h clock.h syscall.h demo.h report.h
│   │                     #         string.h time.h
│   └── asm/
│       └── io.h          # 端口 in/out、cli/sti/hlt 这类机器指令级的东西
├── tests/
│   └── expect/
│       └── boot-screen.txt   # 回归基准：24 行屏幕，抖动字段写成 <N> 占位
├── tools/
│   ├── mkimg.sh          # 拼接 1.44MB 软盘镜像（dd 版）
│   ├── qemu-debug.sh     # 无头 QEMU + gdb 远程调试（断点 / 寄存器 / 屏幕还原）
│   ├── vga-screen.py     # 把 VGA 显存 dump 还原成 80x25 屏幕文本
│   ├── verify.sh         # 回归自检：跑 N 轮 → 抓 row 行 → 交给 verify.py
│   ├── verify.py         # 结构比对 + 抖动项区间断言（--update 重写基准）
│   ├── stack-report.py   # 解析 build/*.su，报单帧大小与报警线
│   ├── qemu-qmp.py       # 走 QMP 执行一条 HMP 命令（pmemsave / info 之类，不停 CPU）
│   ├── qemu-sendkey.py   # 走 QMP 往 8042 注入真按键（KBD_GAP 可调间隔）
│   ├── kbd-echo.sh       # 真按键回显验证：等 kernel_idle → 注键 → pmemsave → 断言 row 24
│   ├── kbd-edit.sh       # 输入行分段验证：退格 / 回车 / 方向键 / Home / End / Del / 历史（十二段）
│   ├── kbd-line.sh       # 回车提交验证：4 格提交队列是否被空闲循环取走、取走即出队
│   ├── kbd-subq.sh       # 提交队列深度：4 条排得满、顺序不乱、满了只丢新的
│   ├── kbd-owner.sh      # 队列归属验证：attach/select/隔离/detach 四项对表
│   ├── kbd-lane.sh       # 每 owner 一条环：一个人打满不挤掉别人（peak / drop / 两条环计数）
│   ├── kbd-depth.sh      # 深度可配：环深/提交深设了真生效、改回也生效
│   ├── kbd-task.sh       # 归属随调度器走：owner_switches / owner_seen / owner_last
│   ├── kbd-shell.sh      # 提交行的消费者：自检 sh_* 五项 + 注入 `echo hi` 看 runs/shell_rc 增量
│   ├── shell-console.sh  # 双页控制台：CRTC 切页、横幅、输入行前缀、hist/!!、第 0 页零改动
│   ├── shell-vars.sh     # 变量表：set/unset/$展开/表满拒绝/按 owner 隔离 + 端到端
│   ├── neg.sh            # 负向组：26 条变异全在这一个文件里，一条 case 一个函数
│   ├── negcommon.sh      # 负向判据：neg_case 只认退出码，ok/MISS + PASS/FAIL 两行文案
│   ├── negpatch.py       # 补丁器：精确单次替换，锚点漂了/多处命中直接报错
│   ├── dbg-hist.sh       # 历史翻页的逐键调试：每按一键取一次屏 + 读 hist_pos/hist_count
│   ├── disk.sh/fs.sh/fs2.sh/fs3.sh/fs4.sh   # ATA → mosfs 读 → 写 → 删 → 间接块
│   ├── exec.sh/elf.sh/pg.sh                # 裸二进制 / ELF e_entry / 跨页两页
│   ├── pgdir.sh                             # 每任务页目录：四槽私有表、放行/收回闭环（判据走串口 KV）
│   ├── kv.sh                                # 机器可读证据通道：KV 记录独立可用、屏幕两路并存
│   ├── kv.py                                # 从串口日志里取一条 KV 记录（tag/key/--index）
│   ├── pghigh.sh                            # 用户态 #PF 一律越权：高地址不进补页、系统仍可用
│   ├── wx.sh                                # 用户页写保护：只读代码页写不进去（fe=0x7）、可写页无回归
│   ├── userpg.sh/qmp-watch.py               # 每任务 esp0：ring3 现场采样（一条 QMP 连接连采）
│   ├── datadisk.py/uprog.ld/uprog2.ld/uprog7.ld  # 造盘与三个用户程序的链接脚本（uprog7 是只读代码段）
│   ├── check-all.sh                        # 逐项跑上面全部脚本并汇总（35 项）
│   ├── check-neg.sh                        # 变异测试组：改坏一处代码，对应的回归必须 FAIL（26 项）
│   ├── quickfs.sh                          # fs 六项串跑（fs/fs2/fs3/fs4/vfs/fd），改文件系统时先跑它
│   ├── kernel-fit.py                       # 内核映像 vs 物理页分配器的边界体检（__kernel_end + 余量）
│   ├── linewidth.py                        # 全源 ≤79 列的构建前置检查
│   └── sshx.py           # 可选：免交互 SSH 助手，把代码同步到调试机
└── Makefile
```

## 构建与运行

依赖（Ubuntu 22.04）：

```bash
sudo apt-get install nasm qemu-system-x86 gdb gcc make
```

```bash
make            # 生成 build/mini-os.img（1.44MB 软盘镜像）
make run        # qemu-system-i386 -fda build/mini-os.img -boot a -m 16（图形窗口）
make debug      # 无头 QEMU + gdb，打印断点、寄存器、还原后的屏幕
make verify     # 回归自检：屏幕 24 行结构比对 + 抖动项区间断言（默认 2 轮）
make stack      # 栈用量报告（每个函数的单帧大小）
make clean
```

产物：`build/kernel.elf`（符号来源，`nm` / `gdb` 用）、`build/kernel.bin`（纯二进制，
`objcopy -O binary` 得到）、`build/mini-os.img`。

Makefile 里 C 源文件用 `vpath` 跨 `init/ kernel/ kernel/chr_drv/ kernel/blk_drv/ mm/ fs/ lib/ shell/` 查找，一条
`$(BUILD)/%.o: %.c` 规则通吃；头文件依赖由 `-MMD -MP` 自动生成 `.d` 文件（之前是
手写的一长串头文件依赖，改一次就等于跟自己较劲）。所以「改哪个模块」不再需要维护
Makefile，`make` 自己知道该编哪几个。目标文件仍然平铺在 `build/`，链接顺序写在
`OBJS` 里，不受目录结构影响。

## QEMU + gdb 远程调试

`tools/qemu-debug.sh` 一条命令完成：从 `nm build/kernel.elf` 取 `kernel_halt` 地址 →
启动 QEMU（`-S` 挂起等 gdb）→ gdb 加载符号、连 gdbstub、下断点、继续执行 →
打印寄存器 → `dump binary memory` 把 `0xB8000` 处 4000 字节显存整块导出 →
`tools/vga-screen.py` 渲染成 80x25 屏幕。

启动参数与断点都不写死：地址每次从符号表现取，改了 C 代码不用改脚本。

```
qemu-system-i386 -fda build/mini-os.img -boot a -m 16 -display none -gdb tcp::1234 -S
```

手工调试（想自己单步时）：

```bash
qemu-system-i386 -fda build/mini-os.img -boot a -m 16 -display none -gdb tcp::1234 -S &
gdb -q build/kernel.elf \
    -ex "set architecture i386" -ex "target remote 127.0.0.1:1234" \
    -ex "break *$(nm -n build/kernel.elf | awk '$3=="kernel_halt"{print "0x"$1}')" \
    -ex "continue" -ex "info registers eip esp ebp cs ss cr0" \
    -ex "dump binary memory /tmp/vga.bin 0xb8000 0xb8fa0" -ex "detach" -ex "quit"
```

镜像里没有 DWARF，所以断点只能用 `break *addr` 形式；想看源码行级单步需要另外产出带
调试信息的 ELF，属于后续工作。

### 在虚拟机（192.168.203.100）里跑

调试机是 VMware NAT 网段的 Ubuntu 22.04.5（用户 `seft`），`nasm 2.15.05` / `qemu 6.2.0` /
`gdb 12.1` / `gcc 11.4.0`。同步代码用 `tools/sshx.py`（paramiko，密码从环境变量
`SSHX_PASS` 读取，不落盘）：

```bash
python tools/sshx.py run "cd ~/mini-os && make && make debug"
python tools/sshx.py put Makefile /home/seft/mini-os/Makefile
python tools/sshx.py put mm/page.c /home/seft/mini-os/mm/page.c
```

源码现在分散在 6 个目录，逐文件 `put` 很啰嗦，省事的做法是整棵树打成一个包再解：

```bash
# 本地（Windows PowerShell）
tar -czf _src.tar.gz Makefile README.md boot include init arch mm sched drivers tools
python tools/sshx.py put _src.tar.gz /home/seft/mini-os/_src.tar.gz
# 虚拟机里
cd ~/mini-os && rm -rf boot include init arch mm sched drivers Makefile \
  && tar xzf _src.tar.gz && rm _src.tar.gz
```

从 Windows 写出的文件带 CRLF，bash 会报 `bad interpreter`，同步后归一一次：

```bash
cd ~/mini-os && find . -path ./build -prune -o -type f \
  \( -name '*.c' -o -name '*.h' -o -name '*.asm' -o -name Makefile -o -name '*.sh' -o -name '*.py' -o -name '*.ld' \) \
  -print0 | xargs -0 sed -i 's/\r$//'
chmod +x tools/*.sh tools/*.py
```

## 回归自检（`make verify`）与栈用量（`make stack`）

之前每次改完都靠“我看一眼 24 行数字对不对”，这不是验证，是祈祷。现在把它变成
可重复的断言：`make verify` 跑 QEMU+gdb，抓 24 行屏幕，和 `tests/expect/boot-screen.txt`
比对。两类检查刻意分开，因为它们抓的是不同的病：

| 检查 | 抓什么 | 具体做法 |
| --- | --- | --- |
| 结构比对 | 行数、行序、文案、行宽变了 | 抖动字段抹成 `<N>` 后逐字 `diff`；行号集合不一致直接报“是不是多打一行把第 25 行顶掉了” |
| 区间断言 | 格式没变但数字错得离谱 | `dtick∈[40,120]`、`C gap∈[4,7]`、`B gap∈[9,11]`、`U,V∈[4,8]` 且 `U+V∈[10,12]`、`H∈[7,11]`、`L∈[1,4]` 且 `H>L`、阶段四 `sw∈[0x1000,0x200000]`、三个 `Y≤8`、阶段六 `ticks∈[8,10]`、`rest` 三段为 `[1,2]/0/[1,2]`（中间 0 = 参数真的被读了）、`rq=0 sq=0`、done 行 `|tune` 三段必须精确 `1E/00/3C`、`frames=3/0x80`；阶段七 `slot=0x1`、`va∈[0x10000,0x90000)` 且必须含 `task killed` |
| 等值断言 | 某些数字必须精确等于某个值 | 阶段五醒来顺序必须是 `0x3142`（堆退化/变 FIFO 会得到别的排列）；阶段六优先级必须是 `2 → 0`（衰减失效就停在 2） |

被抹成占位符的只有**物理上就该抖动**的量：`counter`、`dtick`、`ticks`、`ms`、`gap`、
`t`、`sw`、`Y`，以及每阶段的 tick 账单 `U/V/H/L`。其余整行零容差——包括 `esp`、
页框号 `0x11000/0x12000`、`slot`、`arg`、`sq order`，这些一旦变了就是真变了。

`U/V` 也要给容差这件事是这轮才发现的：加入阶段五后内核变大，12 个 tick 的记账
边界抖出了 `6/4`（此前长期稳定在 `6/5`）。数字本身没错——那一两个 tick 落在
“换栈/退出瞬间”被记给了 `main`——但这说明**拿 ±0 当断言**是错的，于是改成
“各自 4..8、总和 10..12”，既不放过真实退化（12:0 那种），也不再对 1 个 tick 的
抖动红脸。

### 抓屏这一层自己也会坏：`qemu-debug.sh` 的端口退避

症状很误导人：`make` 之后**紧接着**的第一次 `verify` 报 `qemu/gdb 运行失败`，
日志尾部**一个字符都没有**；第二次跑就一切正常。连续几轮改代码时它频繁出现，
看着像内核不稳定，实际是抓屏脚本的竞态：

- 上一个会话的 QEMU 还没退干净，`-gdb tcp::1234` 就绑不上端口，新 QEMU 立刻退出；
- gdb 连上一个已经死掉的 stub，什么也没抓到，但**退出码仍是 0**——原来判成败只看
  退出码，于是“启动失败”被当成“跑完了，只是屏幕是空的”。

三处修法，缺一不可：

1. 起 QEMU 之前先**等端口真空出来**（最多 5 秒，`/dev/tcp` 探测）；
2. 判据从“gdb 退出码”换成“**VGA dump 文件非空**”——抓不到屏幕就是没成功；
3. 失败自动**重试一次**，两级都失败才报错。

实测：先 `touch init/main.c` 强制重建，再立刻 `verify.sh -n 3`，连做两轮，
每次都是 3/3 PASS（修之前同样的序列，第一轮必红）。

有意改屏幕时的流程是 `tools/verify.sh --update`：把本次输出写成新基准，然后**基准
文件和代码一起改**——基准也是代码，不许悄悄漂。

为了确认这套东西不是摆设，做了四次负向验证（都在 `/tmp` 的临时副本里做，验完删掉）：

```
# A. 删掉 task_yield() 里的 left = 0（yield 失去意义）
FAIL (1 项)
  - row 18: sw=0x8 不在 0x1000..0x200000（yield 生效时 sw 必须远大于 tick 数）

# B. 把 phase1 的标签改成 "phase1 uv:"
FAIL (1 项)
  - row 12 不一致
    实际:  phase1 uv: U=0x00000006 ...     基准:  phase1 U/V: ...

# C. 把睡眠堆的“上浮”删掉（退化成 FIFO）
FAIL (7 项)
  - row 19 不一致
  - row 19: 醒来顺序 0x1342 != 0x3142（…若是 0x1234 说明队列退化成按创建顺序）

# D. 让 note_quantum_burn() 一进来就 return（衰减失效）
FAIL (3 项)
  - row 20 不一致（基准是 2 -> 0，实际 2 -> 2）
  - row 20: 衰减 0x2 -> 0x2 != 0x2 -> 0x0
  - row 20: 烧掉 ticks=0xC 不在 8..10

# E. 让 page_identity_unmap() 不清 PTE（guard 页可写）
FAIL (2 项)
  - row 21 不一致
    实际:  phase7 guard: not-trapped
    基准:  phase7 guard: slot=0x00000001 va=0x00013FFC stack overflow -> task killed
  - row 21: 取不到 guard 命中行 → ' phase7 guard: not-trapped'（若打出 not-trapped，
    说明越界写静默成功了，guard 页没生效）

# F. 把 task_sleep 里的长睡回血整段删掉（回血失效）
FAIL (3 项)
  - row 1 不一致（代码长度变了，esp 跟着挪，属预期噪声）
  - row 20: 默认阈值下长睡之后级别=0x0 不在 1..2（回血没生效）
  - row 20: 阈值改回 30 之后级别=0x0 不在 1..2（改回来没生效）
    （中间“阈值抬到 60”那条反而过了——因为本来就不该回血）

# G. 把 task_sleep 里的阈值改成写死的 30（参数变死常量）
FAIL (2 项)
  - row 1 不一致（代码长度变了，esp 跟着挪，这属于预期噪声）
  - row 20: 阈值抬到 60 tick 之后级别=0x1 应为 0
    （不为 0 说明 task_sleep 没读 sched_tune.restore_ticks，参数是死常量）

# H. 把 sched_tune_apply 里的范围校验短路（if (0 && ...)）
FAIL (4 项)
  - row 1 不一致（代码长度变了，esp 跟着挪，属预期噪声）
  - row 18: Y1=0x0 超出 1..8            ← 偶发噪声，见下
  - row 22: 非法值 0 的返回值=0x3C 应为 0（范围校验没拦住）
  - row 22: 拒绝之后阈值=0x00 应为 0x3C（不该被非法值改写）

# I. 让 tune_syscall 不调 sched_tune_apply、也不写回 eax
FAIL (4 项)
  - row 1 不一致（同上，预期噪声）
  - row 20: 阈值抬到 60 tick 之后级别=0x1 应为 0（改值没生效）
  - row 22: 0x31 返回的旧值=0x02 应为 0x1E（返回值没回传或参数没生效）
  - row 22: 拒绝之后阈值=0x1E 应为 0x3C（不该被非法值改写）

# K. 把 task_create 里的 frame[12] = arg 改成 = 0（参数不摆进新栈）
FAIL (6 项)
  - row 1 不一致（同上，预期噪声）
  - row 16 / row 17 不一致：arg 变 0x00000000
  - row 19 不一致：醒来顺序 0x1432（基准 0x3142）
  - row 16: arg=0x0 != 0xDEAD0001
  - row 17: arg=0x0 != 0xDEAD0001
  - row 19: 醒来顺序 0x1432 != 0x3142
    （sleeper 的睡眠毫秒数也是走这个参数，所以参数断了堆顺序也一起乱）

# L. 把空入口校验短路（if (0 && !entry)）
FAIL (1 项)
  - FAIL: 没有解析到任何 row 行
    （空入口照样入队 → iret 跳到 eip=0 → 三重故障，屏幕一行都刷不出来）

# M. 调用门描述符的 DPL 摆成 0（(3ULL << 45) → (0ULL << 45)）
FAIL (2 项)
  - row 13 不一致：dpl 由 0x3 变 0x0
  - row 13: 调用门 dpl 应为 0x3（描述符 DPL 位没摆对，ring3 将 call 不进来）
    （门照样能从 ring0 调通，只有 dpl 这个数会变——所以这条只能靠读描述符来判）

# N. 调用门绕开白名单（tune_gate_mask → TUNE_MASK_ALL）
FAIL (3 项)
  - row 1 不一致（代码长度变了，预期噪声）
  - row 13 不一致：deny 由 0x00 变 0x03
  - row 13: 白名单外的写返回非 0（老化字段本该被门拒掉，白名单没生效）
    （0x03 就是 age_ticks 的旧值——说明老化字段真的被门写进去了）

# O. 回收点不调用 dtor（注释掉 reclaim_dtor(reclaim_arg)）
FAIL (3 项)
  - row 1 不一致（同上，预期噪声）
  - row 13 不一致：mag 由 0x00 变 0x0D
  - row 13: 载荷 magic 应为 0（析构没跑，所有权没真正交出去）
    （0x0D 是 0x600DF00D 的低字节：载荷原封不动，没人收尾）

# P. 回血退回“够阈值就还一级”（steps 一律折成 1）
FAIL (2 项)
  - row 1 不一致（代码长度变了，预期噪声）
  - row 20: 睡 2 个单价之后级别=0x1 应为 0x2
    （deep 的第二个数 0x1 还是对的——它本来就只睡一个单价；第一个数变了才说明
      “按倍数折算”被退回成“够阈值就还一级”）

# Q. guard 路径退回普通 task_exit（eip 改回 task_exit）
FAIL (2 项)
  - row 21 不一致：why 由 0x2 变 0x1
  - row 21: 被 guard 杀掉的任务，dtor 收到的死因=0x1 应为 0x2
    （横死被当成“显式 task_exit”，析构会走正常收尾——这类错误在日志里跟成功一样）

# R. task_die 不记死因（tasks[cur].death = reason → (void)reason）
FAIL (4 项)
  - row 13 不一致：why 由 0x0 变 0x1
  - row 21 不一致：why 由 0x2 变 0x1
  - row 13: dtor 收到的死因应为 0x0
  - row 21: 被 guard 杀掉的任务，dtor 收到的死因=0x1 应为 0x2
    （两处同时变成 0x1：槽位里存的是默认值 TASK_DEATH_EXIT，于是“忘了记死因”
      在屏幕上表现为“所有任务都是显式退出的”——这正是要防的那种沉默错误）

# S. 去掉 ltr（tss_init 里 ltr → str）
FAIL: 没有解析到任何 row 行
    （连第 0 行都抓不到：ring3 的 lcall 在换栈那一步 #GP(0)，错误码栈帧本身就
      落在“本该由 TSS 提供、却没有”的栈上，整机三重故障。这条负向的教训是
      “崩溃发生在屏幕之前”也能被 verify 抓到，但抓不到细节）

# U. 把门的每字段上限拆掉（gate_caps[RESTORE] 的 256 → 0）
FAIL (4 项)
  - row 14 不一致：rej 由 0x00 变 0x28、set 由 0x28 变 0x88、cap 由 0x100 变 0x000
    （rej=0x28 = 40：5000 那次被**接受**了，返回的是当时的值 40；
      set=0x88 是 5000 的低两位——阈值被写成了 5000，整段回血语义都废了；
      cap=0x000 说明“上限表”这个字段已经取不到值。三个数一起变，正是“上限与范围
      是两层不同校验”的指纹：范围校验对 5000 仍然放行）

# V. 门的窗口校验短路（`if ((caller_cs & 3UL) == 3UL)` → `if (0)`）
FAIL (26 项)
  - 行数/行号不一致：实际只有 [0..14]，基准 24 行
  - row 14 不一致：变成 `PF va=0x04000000 … read -> unhandled`
    （ring3 给的野指针 0x04000000 未被拦，内核替它 deref，直接缺页停机。
      wild 那个值本来就是 0，所以“返回值对不对”抓不住它——能抓住它的是
      “后面的行全都不见了”）

# T（**不咬**，如实记录）：删掉 page_mark_user(user_code)
PASS 24 行 —— 屏幕毫无变化
    （原因是 head 建的恒等映射每条 PTE 都带 0x07（U/S=1），用户页“本来就是
      用户可访问的”；page_mark_user 在这个 build 里是保险，不是开关。
      这条负向不红，说明当前的“用户页隔离”还没真正建立——已进下一步建议第 1 条）
```

H 里那条 `Y1=0x0` 是**另一个问题的信号**：阶段四的 `Y` 是三个 yield 任务各自的 tick
账，偶尔会有一个任务在两轮之内一次都没被轮到（`Y=0`），此时“三个 Y 加起来等于总 tick”
仍然成立，但单值 0 会被下界断言抓住。这属于断言过紧（已记在限制清单里）。

A 是区间断言抓逻辑回归、B 是结构比对抓布局漂移、C 是等值断言抓“数据结构被悄悄
换掉”、D 同时踩了两条（结构比对 + 等值/区间断言）、E 是“保护机制被摘掉”（结构
比对 + 一条专门写了病因的提示）、F 是“新加的规则根本没生效”（数值被占位符抹平，
所以只有区间断言报红——这也说明它确实是一条语义断言）、G 是“参数没接线”（写死常量
之后三段对照变 `1/1/1`，中间那个 0 消失）、H 是“对外入口没有校验”、I 是“入口压根没
生效”（返回值停在字段号 `02`）、K 是“参数没传进新栈”（一波带走三处：B、D、堆顺序）、
L 是“连入口为空都照建”（直接把机器打死，一行屏幕都不剩）、M 是“门描述符的特权位写错”
（只有读描述符才看得见）、N 是“白名单形同虚设”、O 是“所有权没人接手”
（载荷原封不动地躺在那儿）、P 是“倍数折算退回阶梯判定”（只有 `deep` 的第一个数动）、
Q 是“横死冒充正常收工”、R 是“忘了记死因”（两行 `why` 一起变成默认值），
报的话都能直接指到病根。

有一处**反例值得记下**：O 那次 `dtor=0x01` 仍然是 1——因为计数器是在“准备交接”的
分支里自增的，`dtor` 调用被注释掉之后计数照加。所以判“析构真跑了”不能只数次数，
必须看副作用（`mag` 是否被清零）。这也是为什么 row 13 上有两个数而不是一个。

D 这一条还改了一次设计：最初 `worker_decay` 是 `while (prio > 0) ;` 的死循环，衰减
一坏任务就永远不退，QEMU 停在无限循环里、`verify` 连一行都抓不到（只报“没有解析到
任何 row 行”）。于是给它加了 `timer_ticks() + 12` 的兜底期限——**一个能被关掉的
自检，必须保证坏掉时是“报红”而不是“挂住”**。

`make stack` 走 `-fstack-usage`：每个 C 文件编译时产出 `build/<name>.su`，
`tools/stack-report.py` 汇总成“每个函数自己吃多少栈”的表，报警线是 4KB 内核栈的
1/8（512 字节）。当前实测 64 个函数、最大单帧 96 字节（`main`）、没有越线：

```
  bytes  % of 4KB  qual    function       where
     96      2.3%  dynamic main           init/main.c:163
     64      1.6%  static  sched_tick     kernel/task.c:445
     48      1.2%  dynamic worker_b       init/main.c:120
```

`dynamic` 一列**不是告警**：`-m32` 且禁用 SSE 时，GCC 只要发现函数需要运行期栈对齐
就标 `dynamic`，和 alloca/VLA 共用这个标记。真正的 VLA 要靠人扫源码，所以这项只
供参考——报告的结论只有一条：“有没有单帧超过 512 字节”。

`-Werror` 已经打开：警告不再能被忽略着过去。

## 镜像布局（与 boot.asm 常量强绑定）

| 偏移 | 内容 | 对应常量 |
|---|---|---|
| 0x0000 | `boot.bin`，512 字节，末尾 `0xAA55` | `BOOTSEG=0x7C00` |
| 0x0200 | `setup.bin`，占 4 个扇区 | `SETUPLEN=4` |
| 0x0A00 | `kernel.bin`（head + C） | `sread = 1 + SETUPLEN`，`SYSSEG=0x1000` |

`kernel.bin` 上限 192KB（`SYSSIZE=0x3000`，单位 16 字节）。镜像必须补零到完整的 1474560
字节：`read_it` 会一直读到 `SYSSIZE`，短镜像会让重试分支死循环。

## 内存布局（启动完成后）

| 地址 | 用途 |
|---|---|
| 0x00000 | 页目录（`pg_dir`），head.asm 的 `.text` 起点 |
| 0x01000–0x04000 | 4 个页表，恒等映射前 16MB |
| 0x05000 | `tmp_floppy_area`（1024B） |
| 0x05450+ | head.asm 代码尾部、IDT(0x54b8)、GDT(0x5cb8) |
| 0x064c0+ | 中断入口桩、C 内核 `.text` / `.rodata` / `.data` / `.bss`（含 4KB 内核栈） |
| 0xA714 | 内核映像结束（`kernel.bin` 37976 字节）；`.bss` 里的 4KB 内核栈占 0x94C0–0xA4BF，`esp` 实测 `0xA4C0` |
| 0x10000–0x90000 | 物理页分配器管理的空闲区间（512KB，128 页，位图记录） |
| 0x90000 | setup 留下的 BIOS 参数区（`boot_params`） |

`setup_paging` 会 `rep stosd` 清零 0–20KB，因此 IDT/GDT/栈必须落在 0x5000 之后。

## 定时器（8254 → 100Hz）

8254 的输入时钟固定 1.193182MHz，通道 0 的分频值 = 输入频率 / 目标频率：

```
1193182 / 100 = 11931.82  ->  11932 (0x2E9C)，实际 99.9985Hz，误差 15ppm
```

写 `0x43 = 0x34`（通道0 | 先低后高字节 | 方式2 | 二进制），再依次写分频低/高字节到 `0x40`。
选方式 2（速率发生器）而不是方式 3（方波）：方式 2 每计满一次发一个脉冲，节拍语义最直白，
也是 Linux 等内核的选择；方式 3 的中断取决于方波上升沿，模拟器里节拍不如方式 2 稳。

时间换算有一个陷阱：`ticks * 1000` 会溢出，而 64 位除法需要 libgcc 的 `__udivdi3`（没有 libgcc
就链接失败）。所以毫秒按 `(t/hz)*1000 + ((t%hz)*1000)/hz` 分段算，全程 32 位。32 位毫秒在
100Hz 下约 49.7 天回绕。

分频值还有一道编译期保险：

```c
typedef char pit_divisor_selfcheck[
    (PIT_DIVISOR * PIT_TARGET_HZ <= PIT_INPUT_HZ + PIT_TARGET_HZ &&
     PIT_DIVISOR * PIT_TARGET_HZ >= PIT_INPUT_HZ - PIT_TARGET_HZ) ? 1 : -1];
```

把 `PIT_TARGET_HZ` 改错时（比如除法多加一个 1 变成另一个频率），这里直接编译失败。

### 怎么证明频率真的变了

光看 tick 数字没意义——不知道过了多久。所以用**另一路晶振**当墙钟：CMOS RTC 的秒计数。
关键是整秒边界的对齐方式：直接轮询秒值会读到更新瞬间的撕裂值，测出来的"秒"偏短（早期实测
一度只有 0.8s）；正确做法是用 reg C 的 **UF（update ended）标志**——读 reg C 自动清零，
"清标志 → 等标志 → 读数"的两个时刻刚好相差 1.000 秒。

在这套量法下换不同分频值实测（QEMU 6.2 / TCG，同一台机器）：

| 编程频率 | 分频值 | 一秒内实测 tick | 备注 |
|---|---|---|---|
| 18.2Hz（BIOS 默认，不重编程） | 0xFFFF | 18 | 对照组，与标称一致 |
| 50Hz | 0x5D38 | 49 | 与标称一致 |
| 100Hz | 0x2E9C | 65~84（多次 70 上下） | 达不到 100 |
| 200Hz | 0x174E | 112 | 明显撞到上限 |

结论要分两层说清楚：

1. **分频值是对的**：三个分频值与 `1193182/hz` 逐一吻合，编译期断言也守着这条；PIT 计数器
   读回值总是小于分频值，说明重载确实生效（若还是 BIOS 的 0xFFFF，值会远超 0x2E9C）。
2. **100Hz 在这个模拟器里拿不满**：18.2Hz 与 50Hz 都能精确对上，100Hz 只到 70 上下、200Hz
   只到 112，说明瓶颈是 QEMU/TCG 的定时器服务速率而不是分频算术（每 tick 的服务延迟把
   高频节拍压低了）。真实硬件或 KVM 直通下应当到 100Hz；这台 VM 没有 `/dev/kvm`。

因此自检断言只判"确实比 BIOS 默认快得多"（`dtick` 落在 40..120），而不是死等 100——
18Hz 与 70Hz 分得开，这才是本机真能判的事。

## 键盘（IRQ1，向量 0x21）

时钟是“节拍”，键盘是**第一条真正的外部输入**：它不提供时间，只提供事件。
所以这条路径的设计原则和时钟正好相反——**越短越好，绝不等待**。

管线是四步，全在中断里完成：

```asm
irq1_keyboard:          ; boot/intr.asm
    pusha
    mov ax, 0x10        ; 装内核数据段
    ...
    call irq1_keyboard_handler   ; 读 0x60 -> 解码 -> 入队 -> EOI
    popa
    iret
```

和时钟桩的差别只有两处：**不换栈**（键盘不调度，`popa`/`iret` 回的是同一个现场），
**不传 ctx**（处理体没有“要恢复哪个栈”的返回值）。

#### 拆成三个文件之后的分工

原来是 860 行一个 `kernel/chr_drv/keyboard.c`，三件事混在一起：**解一个扫描码**、
**字符归属谁**、**第 24 行怎么显示**。现在按调用方向切成一条直线：
IRQ1 → `kbd_feed()` 解码 → `kbd_queue_push()` 进环 / `kbd_echo()` 交给输入行。

| 文件 | 行数 | 拥有什么 | 不许做什么 |
|---|---|---|---|
| `kernel/chr_drv/keyboard.c` | 185 | `SC_*` 常量、`map_normal/map_shift` 两张表、`ext_state` 状态机、`ext_cmd()`、`kbd_feed()`、IRQ1 处理体、`keyboard_init()`、PIC bit1 | 不知道“第 24 行在哪”、不碰显存 |
| `kernel/chr_drv/kbd_queue.c` | 250 | `queue[4][8]` + 每槽 head/tail/count、`lane_depth/drop/peak`、归属（`owner_active`/`attach`/`detach`/`force`）、`kbd_queue_push()` | 不认扫描码、不知道屏幕 |
| `kernel/chr_drv/kbdline.c` | 438 | `line_buf[79]` + 4 条历史、提交环、`echo_render()` 唯一渲染路径、`kbd_echo()`、`echo_cmd()` | 不认识扫描码、不决定归属 |

三处关键改动：

- `kbd_feed()` 里原来内联的那段“满了就丢、否则进队、更新峰值”被提成
  `kbd_queue_push()`（在 `kbd_queue.c`）。这样**归属与容量**可以脱开扫描码单测，
  而 `kbd_feed()` 只剩“这个码出不出字符”。
- `echo_cmd()` / `kbd_echo()` 去掉 `static`（由 `include/linux/kbdline.h` 声明），
  因为解码层要往输入行递东西；`ECHO_CMD_NONE..DOWN` 的编号也搬进该头，
  两边不会各说各话。
- 对外 API 一个字节没动：`kbd_getchar_t` / `kbd_owner_*` / `kbd_lane_*` /
  `kbd_line_*` / `kbd_echo_*` 仍全部由 `include/linux/keyboard.h` 声明，
  所以 `init/main.c`、`shell/console.c` 一行没改。新增的两个头
  `include/linux/kbd_queue.h`、`kbdline.h` 只放“跨文件必需”的少量符号。

搬移是用脚本按行号机械切块做的（860 行手抄一遍必然带进 typo），
切完只补了三个文件头和数据流说明。kernel 从 56128 涨到 **56192 字节**
（`__kernel_end=0x1123C`，余量 60868 字节），`verify.sh -n 2` 两轮 PASS、
`kbd-shell.sh` / `shell-vars.sh` 全 PASS。



`kernel/chr_drv/keyboard.c`（现在的 185 行解码层）里的取舍，每条都能说出理由：

- **扫描码集 1 而不是集 2**：BIOS/head 已经把 8042 配成集 1，QEMU 也默认给它。
  换集 2 要先发 `0xF0` 命令并等 ACK，收益为零、失败面更大。
- **扩展码用一字节状态机，只认七个键**：`0xE0` 不拼成两字节码，只把
  `ext_state` 置 1，下一个码**按扩展码解释**；`0xE1`（Pause 那串六字节）
  整段丢弃并清状态。`ext_cmd()` 只认 Left(`0x4B`)/Right(`0x4D`)/Home(`0x47`)/
  End(`0x4F`)/Delete(`0x53`)/Up(`0x48`)/Down(`0x50`)，其余扩展键整码丢弃、
  `ext_state` 照样清回 0。
  扩展键的**释放码**（`0xE0` 之后的 `0x80|code`）直接忽略——它们不并入队列，
  也不会把 `ext_state` 留成 1（留 1 就会把下一个普通按键吞成扩展码，这是这类
  “偶尔少一个字母”bug 的根）。
- **释放码只用来清 Shift**：`0x80|code` 一律不入队，唯独 `0xAA`/`0xB6` 要清 Shift，
  否则按一下 Shift 之后所有字母都变大写（这是个经典 bug，这里用 `sh=1` 钉住它）。
- **每条环满了只丢自己这条的**，不覆盖还没人取的字符：`kbd_dropped()` 总数、
  `kbd_lane_drop(tag)` 分管计数。每格 4 深够挡“打字快过中断服务”的抖动——
  但这个深度挡不住一整段连续灌入，那时丢的也是自己这条环的字符。
- **`kbd_getchar()` 无字符返回 -1**，没有阻塞版本：在中断驱动的输入上做阻塞等待，
  等于把调度器交给一个可能永远不来的事件。

**屏幕证据**（接在 row 5 光标行的尾部——两者都是 `kernel/chr_drv/` 的事）：

```
cursor: hw=0x00000190 shape=0x00000E0F | kbd: mk=0xFC irq=0x1 q=0x3 sh=0x1
```

- `mk=0xFC`：**从 PIC 屏蔽寄存器读回**的值。bit0/bit1 为 0 表示 IRQ0（时钟）与
  IRQ1（键盘）都放行，其余六位仍然屏蔽。这不是“我写了 0xFC 所以是 0xFC”，
  是 `inb(0x21)` 读回来的；
- `irq=0x1`：自检用 `int 0x21` **软触发同一个门**，桩读 0x60、解码、回 EOI，
  计数从 0 变 1。无头 QEMU 没有键盘输入，这是唯一能证明“门 → 桩 → 处理体 → EOI”
  这条链通了的方式；
- `q=0x3`：喂 8 个扫描码之后队列里的字符数。序列是
  `Shift↓, h, h↑, Shift↑, e, i, 0xE0, 0x4B`：Shift 让 `h` 出大写 `H`，两个释放码不入队，
  `0xE0` + `0x4B`（Left）被扩展码状态机吃成一条编辑动作，所以正好 3 个字符。
  这里喂 `0x4B` 而不是只喂 `0xE0`，是因为只喂前缀会把 `ext_state` 留成 1，
  下一个真按键会被当成扩展码吞掉；
- `sh=0x1`：把队列真读出来，依次必须是 `H`、`e`、`i`——既证明 Shift 生效，
  也证明 Shift 释放后确实回到小写。

**两个负向都咬**（`tools/neg.sh -1 kbd`）：

- 删掉 `idt_set_gate(IDT_VEC_KEYBOARD, irq1_keyboard)` → `irq=0x0`：
  `int 0x21` 落到 `ignore_int` 兜底，桩根本没跑。这一条把“门装好了”和
  “门指向真桩”分开——只装门不指桩，外观上一样是 0x21 有项。
- 删掉那行 PIC 解屏蔽 → `mk=0xFE`：门在、桩在、软触发也通，但**硬件中断永远不会来**。
  这两条正好是“能调”和“能收”两件事，缺一不可。

### 真按键回显：第 24 行是一条可编辑输入行

上面那套证据（`mk=0xFC` + 软触发）只证明“中断会来”，**不证明按键真的被解码成字符**。
`-display none` 的无头 QEMU 没有输入设备事件，所以这里绕一个弯：
用 QEMU 的 `sendkey` 往 8042 里塞真按键。

落点选屏幕**第 24 行**（row 24，最后一行）：它是 24 行自检唯一没占用的地方，
而且写它不触发上卷。**没有按键时它保持空白**——所以 `verify.sh` 的 24 行结构比对
天然不受影响（`vga-screen.py` 跳过全空行）。

这一行不只是日志，而是**一条有真缓冲区的可编辑输入行**：字符存在
`line_buf[79]` 里，`echo_len` 是行内字符数，`echo_col` 是**光标列**（不是行尾）。
每次操作之后 `echo_render()` 把整行重画一遍（79 格写 + `vga_cursor_hw_set(ECHO_CELLS + echo_col)`），
所以“写一个字符”和“移动光标”走的是同一条渲染路径，屏幕上永远是这台状态机的真身。

| 键 | 行为 |
|---|---|
| 可打印字符（`0x20..0x7E`） | **覆盖**写在第 `echo_col` 列、`echo_col++`；若 `echo_col > echo_len` 则 `echo_len` 跟上（即行尾追加），`echo_count++` |
| 退格（`0x0E`） | 删**光标左边**那个字符：后面整体左移一位、`echo_len--`，`echo_col--`，`echo_erase++` |
| 回车（`0x1C`） | 先**提交**（入历史 + 送进 4 格提交队列，`submit_count++`），再整行清空、`echo_len=echo_col=0`，`echo_newline++` |
| 写满 79 列还来新字符 | 整行清空重来，`echo_wrap++`（不是丢弃，也不是换行滚屏） |
| Left `0xE0 0x4B` / Right `0x4D` | `echo_col--` / `echo_col++`（夹在 `0..echo_len` 之间），`echo_move++` |
| Home `0x47` / End `0x4F` | `echo_col=0` / `echo_col=echo_len`，`echo_move++` |
| Delete `0x53` | 删**光标处**那个字符（与退格差一位），`echo_erase++`、`echo_move++` |
| Up `0x48` / Down `0x50` | 翻输入历史（见下），`echo_move++` |

回车除了清行，**还会把这一行存进历史**（空行不存）：`hist_buf[4][79]` 四条、
`hist_pos=-1` 表示“正在编辑一条新行”，0..`hist_count-1` 表示翻到了第几条。
Up 第一次跳到最新那条、之后逐条变旧；Down 逐条变新，**翻过最旧那条就回到空行**
（正是“取消这次翻页”的语义）。手动打字、退格都会把 `hist_pos` 打回 -1——
历史是浏览用的，一动笔就回到“新行”，不会把历史条目偷偷改掉。

历史只有 4 条、满了就从最旧那条开始整排前移（4 × 79 = 316 字节 BSS，
比维护环形下标少一层错位风险）。

分发只有两个槽：`last_char`（字符）与 `last_cmd`（编辑动作，
`ECHO_CMD_NONE/LEFT/RIGHT/HOME/END/DEL/UP/DOWN`）。两者都在 `kbd_feed()` 入口清零，
所以“上一码的动作”不会残留到下一码：

```c
/* kernel/chr_drv/keyboard.c：中断里逐字节处理 */
(void)kbd_feed(sc);                  /* 进不进队列都先解码 */
if (last_char)
    kbd_echo(last_char);             /* 队列满也要回显：按键是真的 */
else if (last_cmd != ECHO_CMD_NONE)
    echo_cmd(last_cmd);
```

`last_char` 是关键：`kbd_feed()` 的返回值只说“有没有进队列”，**回显不依赖它**。
否则队列一满（16 格），屏幕上就再也看不到字符了——第一次真的踩到过。
`echo_col` 与 `echo_len` 分开之后，“光标在中间”才是可表达的：`echo_col` 决定
下一个字符落在哪、退格删谁，`echo_len` 决定 End 跳哪、Delete 有没有东西可删。

硬件光标跟着这一行走（`vga_cursor_hw_set()`）——注意它是**只搬 CRTC、不动
`printk` 的软件 cursor** 的那个接口：键盘回显把方块放到第 24 行，
`printk` 的下一步输出仍然从它自己记住的位置继续，两边互不抢账。

### 回车 = 一次提交：一圈 4 格的提交队列

光标移动和退格都只是“改屏幕上这一行”，**改完总得能交出去**。所以回车做三件事，
顺序不能换：入历史 → 把这一行原样送进提交队列 → 清行。清行必须最后，
因为它会把 `echo_len` 归零，而历史与提交都要用这个长度。

提交队列是 `submit_q[4][79]` + `submit_qlen[4]` 的一圈 FIFO（`submit_head` 写、
`submit_tail` 读、`submit_pending` 计数）：

- 入队从队尾排、取走从队头出，**先来的先被取走，旧行永远不会被覆盖**；
- 只有队满（4 条都还没人取）才丢，丢的总是**新的那条**，记 `submit_drop`；
- 回车和外部生产者走同一个 `submit_push()`，所以“排队规则”只有一份；
  `kbd_line_push(s, len)` 就是给别的生产者用的入口；
- 不做无限队列：4 行够挡“连按几次回车”，再多的量级应该由消费者自己流控。

> 上一版是一格 `submit_buf[79]`：新的一份直接盖掉还没人取的那份，等于
> “谁慢了谁丢行”。换成 4 格之后，回车的语义从“最近一份”变成“排着队的几份”。

消费者仍然是**空闲循环本身**（`init/main.c` 的 `kernel_idle()`）：`hlt` 被 IRQ1
唤醒、从队头取走一条，记下长度与次数。取走即出队：

```c
/* init/main.c：空闲循环里唯一的“读输入行”的地方 */
cpu_hlt();
n = kbd_line_take(line_taken_buf, 79);   /* 返回原始长度；取走即出队 */
if (n > 0) { line_taken_len = n; line_taken_count++; }
```

`kbd_line_take(dst, cap)` 的语义值得钉死：返回的是**原始长度**（可能大于 `cap`，
调用者据此发现被截断），`dst`/`cap` 传 0 等价于“丢弃”。谁取走谁负责那 79 字节，
内核这边不再留副本。

**证据只能从内存里看**（屏幕 24 行已排满，这一条没有行可占）：

1. `tools/kbd-line.sh` 注 `h i ⏎`，读 `line_taken_len=2`、`line_taken_count=1`、
   `line_taken_buf='hi'`、`submit_pending=0`（取走即出队）；`submit_count=5`、
   `submit_drop=1` 这两个数里含着自检 ⑤ 的 4 条 + 1 条与它压满的那一次
   （正常回车路径不再增加 drop）；
2. `tools/kbd-subq.sh` 读自检 ⑤ 留下的四个数：压 4 条 → `subq_peak=4`；
   第 5 条被拒 → `subq_full=0`；依次取出的首字符打包 → `subq_order=0x64636261`
   （a,b,c,d，顺序不乱）；`subq_drop=1`。

```
PASS  回车提交 -> 空闲循环取走：内容、长度、消费与计数全部符合预期
PASS  提交队列：4 格 FIFO 排得满、顺序不乱、满了只丢新的那条
```

两条负向各打一环，都必须 FAIL 且实测 FAIL：

| 负向 | 手法 | 现象 |
|---|---|---|
| `tools/neg.sh -1 kbd_line` | 空闲循环那次调用换成 `n = 0;`（打字、回车都是真的，只有消费失效） | `line_taken_count=0`、`submit_pending=1` 一直留着 |
| `tools/neg.sh -1 kbd_subq` | 满的判据 `submit_pending >= KBD_SUBMIT` → `>= 1`（退回一格） | `subq_peak=1`、`subq_order` 只剩 `'a'`、`subq_drop=2` |

### 队列归属：每 owner 一条独立环

输入行是“谁来编辑”，队列是“谁来消费”。第一版是 16 格全局队列 + 每格一个归属标签：
**取**的时候比对标签，所以谁都拿不走别人的字符。但它只解决了一半问题——
字符进的是同一条环，一个人打字快就能把 16 格全占了，别人的字符根本进不去
（“取不到别人的”做到了，“别人取得到自己的”没做到）。现在改成**每个所有者一条环**：

| 接口 | 语义 |
|---|---|
| `kbd_owner_attach()` | 领一个槽，返回 `1..3`；`0` = 槽满（0 本身是共享槽） |
| `kbd_owner_detach(tag)` | 还槽，并**清掉它那条环**（不留幽灵字符）；若它是当前归属则退回 0 |
| `kbd_owner_select(tag)` | 之后的按键进谁那条环（0 = 共享）；没 attach 的槽返回 -1 |
| `kbd_owner_active()` | 当前归属 |
| `kbd_getchar_t(tag)` | 只从 `tag` 那条环取一个，空了就 `-1`——**绝不顺手拿别人的** |
| `kbd_getchar()` | 等价于 `kbd_getchar_t(0)`，老调用者行为完全不变 |
| `kbd_queue_count_t(tag)` | `tag` 那条环里还有几个 |
| `kbd_queue_total()` | 四条环加起来还有几个（“队列还干净没”看这个） |
| `kbd_lane_drop(tag)` | `tag` 那条环因满丢掉的字符数 |
| `kbd_lane_peak(tag)` | `tag` 那条环的历史最高水位（0..4） |

4 条 × 4 格 = 16 格，总内存没变，但结构全变了：

- 每格只属于一个人，`queue[t]` 是槽 t 独占的 4 格，`q_head[t]` / `q_tail[t]` / `q_count[t]`
  是它自己的读写位置和长度——**一个槽写满不会让另一个槽的下标跟着动**；
- 入队时按入队那一刻的 `owner_active` 选环，满只丢自己这条环的字符（`lane_drop++`）；
- 取字符从“在大环里找第一个标签吻合的、再把后面整段前移”变成 O(1) 一步出队；
- 还槽从“顺序读出、只写回要留的项重建环形”变成 `q_head = q_tail = q_count = 0`——
  一条环只有一个人，清空不需要扫数据。

自检里有两组证据（`main` 的键盘自检 ③ ④），屏幕已排满，所以都放在**非 static**
全局里供 `xp` 读：

③ attach → select 到自己 → 喂一个 `a` → `kbd_getchar_t(1)` 拿到 `0x61`
→ 槽 0 的 `kbd_getchar()` 拿到 `-1` → select 回 0 → detach → 归属回到 0、队列干净。
④ attach 槽 1 → 连喂 6 个字符（环只有 4 格）→ 它装满、丢 2 个；再切回共享喂 4 个
→ 共享照样收下 4 个。四个证据数（`lane_peak1` / `lane_drop1` / `lane_q1` / `lane_q0`）：

```
lane_peak1=4  lane_drop1=2  lane_q1=4  lane_q0=4
PASS  每 owner 一条环：自己装满只丢自己，别人的 4 格不受影响
```

同一时刻共享环里还有 4 个，就是这轮要的差别：老方案在这一步只会得到 `lane_q0=0`
（16 格早被先来的槽 1 占满）。

两条负向各打一环，都必须 FAIL 且实测 FAIL：

| 负向 | 手法 | 现象 |
|---|---|---|
| `tools/neg.sh -1 kbd_owner` | `kbd_feed` 里 `lane = owner_active;` → `lane = 0;`（字符一律进共享环） | 槽 1 取不到 `a`（`-1`）、槽 0 反而拿到 `0x61` |
| `tools/neg.sh -1 kbd_lane` | 满的判据 `q_count[lane] >= depth` → `>= KBD_LANE_MAX`（单条环永远不“满”） | `lane_peak1=6`、`lane_drop1=0`、`lane_q1=6` |

### 深度可配：环深与提交深都是运行时数字

“4 格”和“4 行”这两个数一开始是编译期常量，于是“打字快过消费者”和“整段粘贴”
只有一个折中值。现在两处都改成**运行时可改**（数组按上限分配，实际用几格看变量）：

| 接口 | 语义 |
|---|---|
| `kbd_lane_depth(tag)` | 这条环现在用几格（默认 4） |
| `kbd_lane_set_depth(tag, n)` | 改成 n 格，clamp `1..8`，返回**实际生效**值；调小到装不下当前内容时把读写位置对齐（等价于从空开始），所以缩容前应先取空 |
| `kbd_line_capacity()` | 提交队列当前深度（默认 4） |
| `kbd_line_set_capacity(n)` | 改成 n 行，clamp `1..8`，返回实际值；缩小**不会**丢已经排着的行，只是从此“再进来就判满” |

上限 8 是内存约束（环 4 条 × 8 字节、提交 8 × 79 字节），下限 1（否则等于关掉输入）。
接口返回“实际生效值”而不是 0/1，这样调用者不用猜自己设的数有没有被夹。

自检 ⑥ 就是这套的可测形式：槽 1 的环临时调到 6 → 连喂 8 个字符 → 装 6 丢 2；
改回 4 也生效。提交队列调到 6 → 压到第 7 条才被拒；改回 4 也生效。七个证据数：

```
lane: depth=6 peak=6 drop=2 restore=4
subq: cap=6 full=0 restore=4 | 现在 lane_total=0 submit_pending=0
PASS  深度可配：环深 6 真装 6 丢 2、提交深 6 第 7 条才拒，改回 4 都生效
```

负向 `tools/neg.sh -1 kbd_depth` 把 `lane_depth[tag] = depth;` 改成写回默认值
（`set`/`get`/入队/丢弃判断全都在，只有“设进去的数”不落库）：

```
lane: depth=4 peak=4 drop=4
FAIL  环深设 6 应生效，实际 4 / 应塞到 6，实际 peak=4 / 8 进 6 应丢 2，实际 4
```


### 键盘归谁：`task_t.kbd_tag` + 调度器换人时一起换

上面那套“归属”还是全内核一个变量：谁调 `kbd_owner_select()` 谁就抢走键盘。
现在归属挂在任务结构里——**谁被调度上场，键盘就归谁**：

| 时机 | 动作 |
|---|---|
| `task_create()` | `tasks[slot].kbd_tag = kbd_owner_attach()`，槽满退回 0（照跑，只是和内核共用队列） |
| `schedule()` 换人 | `kbd_owner_force(cur ? tasks[cur].kbd_tag : 0)`，回 main/收工一律 0 |
| 槽位回收 | `kbd_owner_detach(tasks[cur].kbd_tag)`，把它名下的字符一并丢掉 |

`kbd_owner_force()` 是**给调度器专用的**：不做 attach 校验（标签是任务创建时领到的，
调度器只是替它拿回键盘），越界标签一律当 0，换人但归属没变时不记账——所以
“归属真的变了几次”是个有意义的数，而不是“调度了几次”。

证据仍然只能从内存看（三个静态计数，nm 拿得到地址）：

| 符号 | 含义 | 实测 |
|---|---|---|
| `owner_switches` | 归属真的变了几次 | `177699`（远大于 0） |
| `owner_seen` | bit n = 槽 n 真的当过主人 | `0x0000000F`（0/1/2/3 全出现过） |
| `owner_last` | 最后生效的归属 | `0`（收工回共享） |

```
PASS  键盘归属随调度器走：换人即换归属、三个任务槽都当过主人、收工回共享
```

负向 `tools/neg.sh -1 kbd_task` 把 `schedule()` 里那道 `kbd_owner_force(...)` 换成 `(void)0;`
（attach/detach/force 三件套都还在，只有“换人时搬过去”这一环失效）→
`owner_switches=0`、`owner_seen` 只剩 bit0，脚本必须 FAIL，实测 FAIL。

### 为什么收尾是 `kernel_idle()` 而不是 `kernel_halt()`

这件事是“真按键”逼出来的：`kernel_halt()` 是 `cli; hlt` 死循环，
**中断保持关闭**，于是自检一结束，8042 的 IRQ1 就永远进不来，
按键没有任何反应。所以现在有两个收尾入口：

| 入口 | 形态 | 用在哪 |
|---|---|---|
| `kernel_halt()` | `cli` + `hlt` 死循环 | 真异常（`exception_handler()` 里那些 `-> halt`） |
| `kernel_idle()` | `sti` + `hlt` 空闲循环 | 自检正常收尾：`hlt` 被 IRQ 唤醒，醒来继续等下一个 |

自检的最后一步（`main` 故意往 `0x04000000` 窗口外写 → `#PF unhandled`）
走的是 `exception_handler_pf()` 的“其余 → unhandled”分支，那里现在落到
`kernel_idle()`：打印完 `row 23` 就进空闲，键盘随时可用。
真正的异常仍然走 `exception_handler()` 的 `kernel_halt()`。

顺带改了所有抓屏工具：`tools/screen.sh`、`tools/qemu-debug.sh` 的断点从
`kernel_halt` 换成 `kernel_idle`（找不到才回退到 `kernel_halt`）。
不改的话 `continue` 会永远等不到断点——第一次换完就卡在那里了。

**验证脚本**（`tools/kbd-echo.sh` + `tools/qemu-sendkey.py` + `tools/qemu-qmp.py`）：

1. **不用 `-S`** 起 QEMU（`-qmp unix:/tmp/qmp.sock,server,nowait`），
   否则 CPU 停在开机第一刻，按键没人收；
2. **等 `EIP` 落到 `kernel_idle` 里**才注入，而不是 `sleep` 固定秒数——
   自检里有最长 4 秒的 RTC 等待，早注进去的键会在自检途中被吃掉，看起来像“丢键”；
3. `qemu-sendkey.py` 通过 QMP 的 `human-monitor-command` 逐条 `sendkey`（间隔 0.25s，`KBD_GAP` 可调）；
4. `pmemsave 0xb8000 4000 /tmp/vga-kbd.bin` 取屏（**不停 CPU**），
   `vga-screen.py` 断言 `row 24| hello`。

实测：`bash tools/kbd-echo.sh` → `PASS  真按键回显 = hello`（默认注 `h e l l o`，
也可以 `bash tools/kbd-echo.sh a b c`）。

**编辑行为的分段验证**（`tools/kbd-edit.sh`），八段各取一次屏、逐段断言要放下
四个约束：每段开头先按回车清行、QEMU 的 `sendkey` 连打同键会丢 1~2 个、
大写字母必须写 `shift-x`（写 `X` 根本不发键）、扩展键只有真按到才会动光标。

| 阶段 | 注入 | row 24 期望 |
|---|---|---|
| 1 | `a b c` + 两次退格 | `a` |
| 2 | `d e` + 回车 | 空 |
| 3 | `x y` | `xy` |
| 5 | `a b c` + Left ×2 + `z`（覆盖模式） | `azc` |
| 6 | `a b c` + Home + `q` | `qbc` |
| 7 | `a b c` + Left ×3 + End + `w` | `abcw` |
| 8 | `a b c d` + Left ×2 + Delete | `abd` |
| 9 | 连打 80 个 `f` | `fff`（撞满 79 列后再来一个才清行重来，剩下 3 个） |
| 10 | `a b c` ⏎ `d e` ⏎ + Up ×2 | `abc`（翻到更早那条） |
| 11 | 同上 + Up + Down | `abc`（Down 从最新一条退回更早一条） |
| 12 | 同上 + Up ×2 + Down ×4 | 空行（翻过最旧那条就回空行） |

阶段 4 没有单独列：它就是阶段 3 之后的清行重来，已被阶段 9 收进同一条断言。
实测十二段全 PASS，末段是 `回显计数 23 -> 103（目标 103） / row24='fff'`。

阶段 7 特意**先按三次 Left 回到行首**再按 End：不然光标本来就在行尾，
End 是空操作，测了等于没测。阶段 12 要连按四次 Down 也是同一个道理：
历史里已经攒着前几段的行（最多 4 条），“越过最旧”得按够次数才发生——
第一版只按一次 Down，断言随历史长度飘，改成一串 Down 才稳定。
排查那一步用的是 `tools/dbg-hist.sh`：每按一个键就取一次屏、并把
`hist_pos`/`hist_count` 一起 `xp` 出来，一眼就能看出是“键没到”还是“状态没动”。

**两条负向都咬**：

- `tools/neg.sh -1 kbd_echo`：把 `echo_render()` 里那句“缓冲区画到显存”换成 `(void)0;`
  （函数还在、调用还在、缓冲区也照填，就是不动显存）→ 真按键注入后 row 24 仍为空，
  脚本报 `FAIL  row 24 没有 'hello'`。这条专门打链条最后一环——
  前面 irq/回显计数照涨（解码是真的），只有“画屏”这一处失效；
  它必须打在 `echo_render()` 的循环里，而不是 `kbd_echo()`——升级成缓冲区之后
  `echo_render()` 才是唯一的写屏路径；
- `tools/neg.sh -1 kbd` 的两个老负向在 `kernel_idle` 版上重跑仍然咬：
  no-gate → `irq=0x0`、no-unmask → `mk=0xFE`。

> 三个踩过的坑，都是“工具不动、结论错”的那类：
> 1. `kbd-echo.sh` 原来把目录写死成 `$HOME/mini-os`，负向脚本 `cd /tmp/negke`
>    后跑的还是真目录，负向“通过”了。现在用 `cd "${MINI_OS_DIR:-$HOME/mini-os}"`；
> 2. 一开始用 gdb 的 attach/detach 取屏，**每次都要停一次 CPU**，
>    按键卡在这个窗口里就丢（阶段 3 期望 `xy`、实际只有 `x`）。改用 QMP `pmemsave`，
>    不停 CPU；
> 3. `-monitor unix:...` 是 readline 前端，从 socket 一次灌一整行会被逐字符回显
>    搅乱，`pmemsave` 直接报 `invalid char 't' in expression`。改走 QMP 的 JSON
>    （`pmemsave` 用同名 QMP 命令），稳。
>
> 还有一条不是工具的锅：`sendkey` 连打同一个键实测会丢 1~2 个/10 个，
> 所以 `kbd-edit.sh` 每段都先按回车清行、失败整段重来；最后一段按
> `echo_count` 补发，凑够 80 个才下结论。
>
> 第五个坑是脚本自己的：**大写字母不能写 `X`**，HMP/QMP 的键名里它叫 `shift-x`，
> 写 `X` 不报错、但一个键都到不了——首轮阶段 5 就是这样拿到 `abc` 的。
> 测试一律用小写，要测 Shift 就走 `shift-x`。

### 提交行谁来解释：`shell/shell.c` 的 `shell_run()`

前面那几节把行从按键一路送到提交队列，最后一环一直空着：`kernel_idle()`
取走一行只记长度，等于**收了但没读**。现在取走之后交给 `shell_run(line, len)`，
这条“ISR → 输入行 → 提交队列 → idle → 解释器”的链才真正闭合。

#### 拆成四个文件之后的分工

这一步之前 `shell/shell.c` 是 728 行：状态、变量表、显存写屏、解析分派全在一处，
任何新逻辑都能顺手读到 `states[bound]` 的任意字段。现在按“谁拥有什么”切开：

| 文件 | 拥有什么 | 不许做什么 |
|---|---|---|
| `shell/state.c` | `states[4]` + `bound` + 参数缓冲；`ST` 宏（`shell_state()`） | 不写显存、不解析 |
| `shell/var.c` | 变量表的查/写/删（挂在 `ST` 上，天然按 owner 隔离） | 不打印、不认识命令名 |
| `shell/console.c` | `out_buf[4][79]` 滚动缓冲 + 用户页 `0xB9000`，`con_row/col/chars` | 不知道命令是什么 |
| `shell/shell.c` | 解析、命令表、分派、`!!`、历史记账 | 不直接写显存、不自己存状态 |

好处不只是“文件短了”：**“谁写了第 0 页”这种事故从结构上消失了**——只有
`console.c` 碰显存，而它压根不认识命令；“状态被谁改了”也只剩一个入口。
代价是跨文件多了几个函数调用（`shell_out_puts` / `shell_con_report` / `shell_var_*`），
kernel 从 55900 涨到 56128 字节，`__kernel_end=0x111DC`，分配区余量仍有 60KB。

验收只看两件事：`verify.sh` 的 24 行一位不变（`shell.c` 拆出来的都是内部函数，
自检屏根本没用到），以及四个 shell 脚本全 PASS。

| 部件 | 位置 | 说明 |
|---|---|---|
| 行协议 | `parse()` | 空格/Tab 分隔、连续空白算一次、`"…"` 内不切分、最多 4 个参数（含命令名） |
| 命令表 | `cmds[]` | `help`(4) / `echo`(4) / `ver`(3) / `clear`(5) / `hist` / `set` / `unset` / `stat` |
| 输出 | `shell/console.c` 的 `out_buf[4][79]` | 4 行滚动缓冲，满了覆盖最旧那条；开用户页之后同步画到第 1 页 |
| 返回码 | `shell_run` | `0` 成功 / `-1` 未知命令 / `-2` 参数太多 / `-3` 空行 / `-4` 无历史 / `-5` 变量表满 |
| 计数 | `shell/state.c` 的 `runs`、`unknown_count`、`error_count`、`last_rc`、`shell_argc` | 都在 state 里，`nm` 拿地址、`xp` 读 |


为什么输出非得进内存：屏幕**已经一格不剩**。第 24 行归输入行，0..23 行铺满
上一节的证据，只要 shell 往显存写一个字符，`make verify` 的 24 行比对立刻红。
所以这一层先把“解释对不对”落在内存里，等下一步再谈“打到哪儿看得见”。

自检 ⑦ 在 `main.c` 里跑五行固定输入，证据读法（`nm -n build/kernel.elf` 现取地址）：

| 输入 | 期望 | 读哪个符号 |
|---|---|---|
| `echo hello` | rc=0、输出 5 字节 `hello` | `sh_rc_echo`、`sh_out_echo_len`、`sh_out_echo` |
| `help` | rc=0、输出缓冲 ≥1 行 | `sh_rc_help`、`sh_help_lines` |
| `bogus` | rc=-1、未知命令计数 1 | `sh_rc_unknown`、`sh_unknown_n` |
| `echo "a b"` | rc=0、**argc=2**（引号内不切分）、输出 3 字节 `a b` | `sh_rc_quote`、`sh_quote_argc`、`sh_out_quote_len`、`sh_out_quote` |
| `echo a b c d e` | rc=-2（6 个参数超上限）、解析失败计数 1 | `sh_rc_argv`、`sh_error_n` |

**端到端**由 `tools/kbd-shell.sh` 分两段跑：A 段读上面那组自检证据；
B 段在 `kernel_idle` 里注入 `echo hi` + 回车，断言 `line_taken_buf='echo hi'`、
`runs` **增量** 1、`shell_rc=0`。实测：

```
A. echo hello: rc=0 len=5 out='hello' | help: rc=0 lines=2 | bogus: rc=-1 unknown=1
   echo "a b": rc=0 argc=2 len=3 out='a b' | echo a b c d e: rc=-2 errors=1
B. 注入 'echo hi'：runs +1、shell_rc=0、取走次数=1、行内容='echo hi'
PASS
```

`tools/neg.sh -1 kbd_shell` 咬的是命令表本身：把 `{ "echo", 4, cmd_echo }` 的长度
改成 `5`，`echo` 永远匹配不上 → A 段 `rc=-1 / len=0 / out=''`、B 段
`shell_rc=0xFFFFFFFF`，脚本 `FAIL`；还原后重跑恢复 PASS。改长度而不是删表项，
是因为删了会连带编译告警，改动面太大、看不出“只错一处”。

> 两个踩过的坑：
> 1. `shell_run("echo \"a b\"", 11)` 多给了 1 字节——C 字符串只有 10 个字符，
>    多读的一位是相邻 rodata（实测拿到 `argc=3`、输出 5 字节）。**长度要自己数对**，
>    `parse()` 是信任调用方给的 `len` 的；
> 2. 注入空白要用 QMP 键名 `spc`，写 `space` 不报错但一个键都发不出去——
>    第一轮 `echo hi` 因此被拼成 `echohi`，`shell_rc` 拿到 -1（未知命令）。

### 用户页：让 shell 有看得见的出口

上一步的 shell 只会往内存里写，用户什么也看不见。这版给它一块**自己的屏幕**，
做法不是“盖掉自检”，而是**VGA 双页**：

| 页 | 显存 | 角色 | 谁写 |
|---|---|---|---|
| 第 0 页 | `0xB8000` | 证据页：24 行自检数字，`verify` 与所有 `pmemsave` 的基准 | `printk` / 键盘输入行 |
| 第 1 页 | `0xB9000` | 用户页：横幅 + 命令回显 + 输出，24 行滚动 | `shell` / 键盘输入行（镜像） |

页间距 `0x1000` 字节（2048 格）> 一屏的 2000 格，两页不会叠。切换只动 CRTC 的
`0x0C/0x0D`（起始地址，单位是字符格，所以第 1 页 = `0x800`），**显存一个字节都不搬**——
这就是“用户开始打字之后，`pmemsave 0xb8000` 仍然是自检那 24 行”的原因。

三个落地细节：

1. `vga_page_select(page)` / `vga_crtc_start()`（`kernel/chr_drv/vga.c`）——写/读起始地址寄存器；
2. 输入行要出现在**用户页**第 24 行，所以 `echo_render()` 除了画第 0 页，还在
   `kbd_set_echo_mirror(CON_BASE)` 之后把同一行**再画一份**到第 1 页；
   硬件光标用的是页内相对位置（`24*80+col`），切页后天然对得上；
3. `shell_console_open()` 在 `main` 打印完全部证据、**并且在往窗口外写之前**调用——
   那次写会 `#PF unhandled` 直接进 `kernel_idle()`，放在它后面就永远跑不到。

用户页行为：开页时清屏 + 写横幅 `mini-os shell 0.1  (help echo ver clear hist)`（45 字符）；
之后每行命令先回显 `> <原样输入>`，再输出结果；超过 24 行整页上滚（`con_scrolls` 计数）；
`clear` 命令既清内存输出缓冲、也抹掉用户页。证据：`console_start`（期望 `0x800`）、
`con_chars`、`con_row`、`con_scrolls`、`con_open`，全是 `xp` 能读的静态量。

### 提示符：`> ` 只画在用户页

用户页第 24 行不再是“裸的一行字”，而是 `> ` 开头。实现上刻意**只画镜像页**：

- `kbd_set_echo_prefix("> ", 2)` 存进 `echo_prefix[8]`；`echo_render()` 画镜像页时
  先铺前缀、再把输入内容右移 `prefix_len`，硬件光标也跟着右移（`echo_cursor_sync()`）；
- 第 0 页**一个前缀都不画**——于是所有老断言（`kbd-echo.sh` 的 `row 24| hello`、
  `kbd-edit.sh` 十二段的 `azc`/`qbc`/`abcw`）完全不用改。这是“新功能不许动老基准”的解法：
  把新东西放在新页，老页保持字节级不变。

实测（同一份 dump 两页对照）：打分 `hi` 未回车时，用户页 `row 24| > hi`、
证据页 `row 24| hi`。

### 光标跟着输出走，打完再交还输入行

原来只有输入行动过 CRTC 光标（`echo_render()` → `vga_cursor_hw_set(24*80+col)`），
输出区写多少字光标都纹丝不动——看上去像"卡在输入行上"，用户看不出输出长到哪了。

接法是三段，各管一段，谁也不抢账：

1. **输出期间**：`shell/console.c` 的 `con_cursor_sync()` 在每写一个字符、每次
   `shell_con_newline()`、`clear` 复位之后，把 CRTC 光标搬到 `con_row*VGA_COLS+con_col`。
   写 CRTC 用的是 `vga_cursor_hw_set()`——**只写硬件，不动软件 `cursor`**，
   所以第 0 页 `printk` 的位置记账完全不受影响。CRTC 光标号本来就是相对起始地址的
   偏移，而起始地址正是第 1 页，两边天然对齐。
2. **命令结束**：`shell_con_report()` 收尾时调 `kbd_echo_cursor_to_line()`
   （`kernel/chr_drv/kbdline.c` 新导出的一个入口），把光标交还给第 24 行提示符后的打字位置——
   否则用户下一句不知道该在哪儿敲。
3. **打字期间**：仍是老路径 `echo_render()` → `echo_cursor_sync()`，行为不变。

证据：`con_cursor_cell`（最近一次写进 CRTC 的格子，`shell_console_cursor()` 可读）。
`tools/shell-console.sh` 在 `help` + `stat` 之后断言两条：

- `con_cursor_cell == con_row*80 + con_col`——光标确实停在**输出末尾**；
- `con_cursor_cell != 24*80`——不是停在输入行（就是老毛病的那种表现）。

### 命令历史：`hist` 与 `!!`

shell 自己也有持久态了（和键盘那套 Up/Down 的**编辑历史**是两件事）：

| 东西 | 位置 | 说明 |
|---|---|---|
| `hist_cmd[4][79]` + `hist_cmd_len` + `hist_cmd_n` / `hist_cmd_head` | `shell/shell.c` | 4 条环形，存已执行过的命令 |
| `hist` | 内建命令 | 按“旧 → 新”每条输出一行，最多 4 条 |
| `!!` | `expand_repeat()` | 取最近一条（**除自己以外**）替换当前行再执行；没历史返回 `-4` |
| `hist_record()` | 每次执行有效命令后 | `!!` 展开出来的那条**不再记账**，否则会把自己套娃 |

开页时 `hist_cmd_head/hist_cmd_n` 一起清零：自检 ⑦ 跑过的那五行不该成为用户的历史。
证据：`hist_cmd_n`（期望 3：`help`、`hi` 这条未知命令、`echo a`）、`shell_hist_line(idx)` 访问器。

实测：`help` ⏎ `hi` ⏎ `echo a` ⏎ `!!` ⏎ 之后用户页：

```
row  1| > help          row  5| > echo a
row  2| help echo ver clear hist   row  6| a
row  3| > hi            row  7| > echo a     <- !! 展开的
                        row  8| a
```

`hist_cmd_n=3`、`last_rc=0`（`!!` 重跑成功）；`hi` 是未知命令所以 row 3 之后那行是空的。

**两个负向**：`neg.sh -1 console`（`vga_page_select()` 不写 `0x0C/0x0D` → `console_start=0x0`）、
`neg.sh -1 shell_hist`（`hist_record()` 不累加 `hist_cmd_n` → `!!` 返回 `-4`、用户页只出现一条
`> echo a`）。两条都把 `shell-console.sh` 顶成 `FAIL`，还原后恢复 `PASS`。

> 这一版也暴露了“证据量”这条线：`esp` 又因为 `main`/`shell` 多用几个栈变量从
> `0x0000D7A0` 漂到 `0x0000DB20`，`verify.sh --update` 更新基准即可；
> 24 行的文字一个字符都没变，双页设计正是为了这个。

**实测**（`tools/shell-console.sh`）：开页后 `console_start=0x800`、`con_chars=40`，
用户页 row 0 是横幅；注入 `help` + 回车 → `con_chars 40 -> 65`、`con_row=3`，
用户页 dump 出：

```
row  0| mini-os shell 0.1  (help echo ver clear)
row  1| > help
row  2| help echo ver clear
```

同时断言**第 0 页零改动**：`row 1` 的 `esp=`、`row 23` 的 `PF va=` 都还在。→ `PASS`。

`tools/neg.sh -1 console` 咬的是页切换本身：把 `vga_page_select()` 里写 `0x0C/0x0D`
的两句换成 `(void)start;`，`console_start` 变 `0x0`、测试 `FAIL`；还原后恢复。

> 这一版也暴露了“证据量”这条线：`esp` 因为 `main` 多用了几个栈上变量从
> `0x0000D2C0` 漂到 `0x0000D7A0`，`verify.sh --update` 更新基准即可；
> 屏幕 24 行的文字一个字符都没变，双页设计正是为了这个。

### 持久态：`struct shell_state` + 变量表

到这一版，shell 的东西不再是一堆散落的 static，而是收进一个结构、按 owner 分槽：

```
struct shell_state {
    hist[4][79] + hist_len + hist_n/head      /* 命令历史（hist / !!） */
    var_name[6][8] + var_val[6][24] + used    /* 变量表（set / unset / $name） */
    runs / unknown_count / error_count / last_rc
};
static struct shell_state states[4];          /* owner 0..3 */
shell_bind(owner)   -> 切绑定（越界当 0）
shell_bound_owner() -> 当前绑的是谁
```

`kernel_idle()` 取走一行后先 `shell_bind(kbd_owner_active())` 再 `shell_run()`：这一行归谁，
就解释进谁那份历史与变量。现在只有共享槽 0 在跑，但“多 owner”已经不是靠改 shell 来实现的。

**变量表**（6 格，名字 ≤7 字符、值 ≤23 字符）：

| 命令 | 行为 |
|---|---|
| `set` | 列全部，一行一个 `name=value` |
| `set name value...` | 写入（多个参数用单空格拼成值）；表满或名字非法 → `-5` |
| `unset name` | 清掉那一格（不存在也算成功） |
| `$name` | 解析参数时展开成值；未定义 → 空串；值里**不再**递归展开 `$` |

自检 ⑧（`init/main.c`）的证据：在 owner 1 里 `set who seft` → `echo $who` 输出 `seft`（长度 4）；
切到 owner 0 `echo $who` 长度 0（**看不见**）；回 owner 1 `unset who` 后再 `echo $who` 长度 0；
连塞 7 个变量第 7 个返回 `-5`。

`tools/shell-vars.sh` 两段验证：A 段读 `sh_rc_set`/`sh_var_val`/`sh_rc_iso`/`sh_var_iso_len`/
`sh_rc_unset`/`sh_var_after_len`/`sh_rc_full`；B 段在控制台注 `set x 42` ⏎ `echo $x` ⏎，用户页：

```
row  1| > set x 42
row  3| > echo $x
row  4| 42
```

`tools/neg.sh -1 shell_vars`：让 `var_set()` 永远返回 `-1` → `set` 报 `-5`、`$x` 展开成空、`42` 那行消失，`FAIL`。

> 一个自己挖的坑：`expand_var()` 第一版忘了先判 `line[i] == '$'`，只要下一个字符是字母就
> 当成变量名吃掉，于是 `help`/`echo` 全被吞成空参数、所有命令都 `-1`。修法是在函数入口
> 直接 `if (line[i] != '$') return 0;`——先认符号，再认名字。

## 任务切换（换栈式抢占调度）

不切 CR3、不动 TSS，就用最朴素的一招：**换栈**。时钟中断进来时 CPU 已经把
`eip/cs/eflags` 压在**当前任务自己的栈**上，桩再 `pusha` 一次，整个现场就在栈顶。
调度器只要把当前 `esp` 记进任务结构、取出下一个任务的 `esp`，桩
`mov esp, eax; popa; iret` 恢复的就是另一个任务的现场——切换就这么完成了。

`boot/intr.asm` 的时钟桩比异常桩多一步：

```asm
irq0_timer:
    pusha
    ...
    push esp                    ; 参数：刚压好的现场地址（PUSH ESP 压的是 push 前的值）
    call irq0_timer_handler     ; -> eax 是要恢复的栈指针
    add esp, 4
    mov esp, eax                ; 换栈
    popa
    iret
```

新任务的第一次“返回”靠手工摆帧，栈布局必须和 `popa` + `iret` 的读法对齐。
比上一版多两格：入口函数改成带参的 `void (*)(void *arg)`，而 cdecl 约定进入函数时
`[esp]` 是返回地址、`[esp+4]` 是第一个参数，于是栈顶上依次放返回桩和参数：

| 偏移 | 内容 | 说明 |
|---|---|---|
| frame[0..2] | edi / esi / ebp | 全 0 |
| frame[3] | esp | `popa` 不看这个格子 |
| frame[4..7] | ebx / edx / ecx / eax | 全 0 |
| frame[8] | eip | 任务入口函数 |
| frame[9] | cs | `0x08` 内核代码段 |
| frame[10] | eflags | `0x202`，关键是 IF=1——任务里可以被打断 |
| frame[11] | ret | `task_return_stub`：入口函数 return 之后落在这里 |
| frame[12] | arg | 入口函数的唯一参数 |

`tasks[slot].esp` 必须指向 **frame[0]**：桩先 `popa` 弹掉 frame[0..7]，`iret` 再弹
frame[8..10]，之后 esp 正好落在 frame[11]，入口函数读到的 `[esp+4]` 就是 frame[12]
里的 `arg`；函数一 `return` 就跳进 `task_return_stub`，自动 `task_exit()`。
（这一格曾经放错过：把 esp 指到 frame[11]，结果 `popa` 弹出的全是页外垃圾，
`iret` 直接 GP —— 现场表现为 `EXCEPTION: general protection err=0x0`，esp 落在
栈页顶端之外。）

调度器的槽位：slot 0 存 `main` 自己的上下文（第一次被抢占时才填），slot 1..4 是
worker，每个任务用 `page_alloc()` 拿一页当栈。

### 状态机与两个队列

```
UNUSED --task_spawn--> READY --被选中--> RUNNING
                        ^                  |
                        |    task_sleep    v
                    到期唤醒            SLEEPING
                        |
                        +--task_exit--> DEAD --> UNUSED（栈延后回收）
```

任务状态只记在任务结构里，**队列里只放槽位号**：

- **就绪队列 `rq`**：**每个优先级一条**环形数组，入队尾、从最高优先级的队头出队。
  被时钟剥夺的运行任务排回**自己那一级**的队尾。`main`（slot 0）**永远不排队**，
  它只是队空时的兜底上下文——取不到人就回到 main 的 `hlt`。
- **睡眠队列 `sq`**：以 `wake_tick` 为键的**小顶堆**（`sq_heap[]` 存槽位号，键直接
  读 `tasks[slot].wake_tick`）。插入是“放末尾 + 上浮”，出堆是“末尾搬到根 + 下沉”，
  都是 O(log N)。每个 tick 的唤醒只做一件事：**看堆顶**，到期才出堆改成 `READY`
  并推进 `rq`；一次 tick 里连着到期好几个也能顺手清完（`while` 不是 `if`）。
  键是现成字段而不是另存一份 `sq_wake[]`，少一份状态就少一处能对不上的地方——
  上一版是“升序数组 + 插入排序”，唤醒 O(1) 但要搬 O(N) 个元素。

`task_spawn(entry, arg, prio, slice)` 是唯一的创建入口：扫第一个 `UNUSED` 槽、
`page_alloc` 一页、摆好帧、按优先级推进对应队列，全程在 `cli/sti` 里做，避免入队
瞬间被 tick 打断。**这一步让槽位可复用**——`task_exit` 立刻把槽标成 `UNUSED`，
后来者就能拿到它。`task_reset()` 清空所有槽与队列，允许一个内核里反复换几组任务。

### 优先级、时间片、老化与衰减

四个机制的语义很不一样，混在一起才叫“带公平性的抢占调度”：

- **优先级（`rq` 分三级）**：取人就取最高优先级的队头。只要高优先级那一级还有人，
  低优先级**基本选不上**——这是“抢占式优先级”，不是“按权重分 CPU”。
- **时间片（`slice`，以 tick 计）**：每个任务有个递减的 `left`，`sched_tick` 里
  `left--`；**只有 `left` 归零才换人**，换下去时排回自己那一级的队尾。所以同优先级
  的轮转节奏是“每 slice 个 tick 换一次”，而不是每个 tick 都换。
  传 `slice=0` 表示按**当前**优先级取默认值，默认表是 `{prio0:8, prio1:4, prio2:2}`
  ——越高越短，让高优先级更快让位。
- **老化（aging，向上借，防饿死）**：一个任务在就绪队列里等够 `TASK_AGE_TICKS`
  （3 tick）就升一级（`rq_remove` 摘出旧队、`rq_push` 塞进新队），升到最高级后能在
  高优先级的时间片边界上抢到一小段；这一轮跑完（或自愿 sleep/yield）就降回
  `base_prio`。所以“高优先级一直不空”也不会让低优先级永远拿不到 CPU。
- **衰减（decay，向下压，罚贪心）**：一片时间片**真的被跑满**（不是 `yield`/`sleep`
  手动清零）就记一笔 `hogs`，连续 `TASK_DECAY_QUANTA`（4）片之后把 `base_prio`
  降一级，最低到 0。压的是 `base_prio` 而不是 `prio`：`prio` 每轮结束都会被
  `schedule()` 按 base 重装，改 `prio` 会被下一次重装抹掉。自愿让出会清零 `hogs`
  ——让出 CPU 本身就不是贪心。

一句话概括两者的方向：**老化是借（临时），衰减是罚（持久）**。

衰减的记账点必须在 `sched_tick` 里而不是 `schedule()` 的 `left==0` 分支，因为后者
既可能是真跑满，也可能是 `task_yield`/`task_sleep` 手动清零进来的，只有
“tick 里递减到 0 的那一下”才必然是跑满：

```c
if (tasks[cur].left > 0 && --tasks[cur].left == 0)
    note_quantum_burn(cur);             /* 这一片真跑满了：历史惩罚记一笔 */
```

顺手修掉了一个潜伏的 bug：**队里只剩自己时时间片不重新装填**。原来
`tasks[cur].left = slice_for(cur)` 只在“换人”那一条分支里做，`next == cur`
（不换人）时就没人装填；`left` 一直是 0，此后每个 tick 都跳进“跑满了”的分支，
衰减就会按 tick 而不是按片算，一个独跑的任务会被罚得莫名其妙地快（8 个 tick
降到最低级的事，3 个 tick 就做到了）。现在这条分支也装填：

```c
} else if (cur != 0 && tasks[cur].left == 0) {
    tasks[cur].left = slice_for(cur);   /* 不换人也要重新装填 */
}
```

两条规则的交界在 `schedule()` 开头这一小段：时间片还没用完、且没有更高优先级的
任务在等，就**直接返回原 `ctx`**——不换栈。高优先级一旦就绪，则立刻抢走，不等
slice 用完。

```c
if (cur != 0 && tasks[cur].state == TASK_READY && tasks[cur].left > 0) {
    next = rq_top(0);                       /* 只 peek，不摘 */
    if (next < 0 || tasks[next].prio <= tasks[cur].prio)
        return ctx;                         /* 时间片没用完、也没人抢：继续跑 */
}
```

**这里踩过一个坑，值得写清楚**：一开始时间片是“创建时按原始优先级定死的”
（`slice = default[base_prio]`），结果老化把 `prio0` 的任务升到 `prio2` 之后，它仍
拿着 `prio0` 的 8 个 tick 长片，实测账单变成 `H=5 / L=6`——低优先级反而吃得多。
所以时间片必须**跟着当前级别走**（`slice_for(slot)` 每次按 `tasks[slot].prio` 查表），
改完才回到 `H=9 / L=2` 这个合理形状。

任务结构里还留了 `ticks`（被记账的 tick 数），配合 `task_ticks_of(slot)` 就能在
屏上直接读出“谁占了多少 tick”——时间片、优先级与老化的证据全靠它。


### 栈回收

栈回收不能在自己还在用的栈上做。`DEAD` 的栈先记进 `reclaim_stack`，等**换到
别人的栈之后**的下一次调度才 `page_free`；最后一个退出的任务连这一步都等不到，
所以回 `main` 之后由 `task_reclaim()` 补一次归还（否则会漏一页）。

### 主动让出 = `int 0x30`

在 C 里手写 `mov esp` 换栈极其脆弱（编译器随时会重排/优化掉），于是借中断门
自己造一个调度点：向量 `0x30`（PIC 重映射到 `0x20-0x2F`，`0x30` 空着，不与设备冲突）
装一个和 IRQ0 同构的桩，只是**不回 EOI、不累 tick**：

```asm
isr_yield:
    pusha
    ...
    push esp
    call task_yield_handler     ; 只调度，不累 tick
    add esp, 4
    mov esp, eax
    popa
    iret
```

`int 0x30` 同样会把 `eip/cs/eflags` 压在**当前任务自己的栈**上，所以省下的现场和
被时钟抢走时一模一样，收尾代码可以完全复用。任务里让出就是：

```c
cpu_cli();                       /* 改状态与入睡眠队列之间不留窗口 */
tasks[cur].state = TASK_SLEEPING;
tasks[cur].wake_tick = timer_ticks() + ticks_to_wait;
sq_insert(cur, tasks[cur].wake_tick);
__asm__ volatile ("sti\n\tint $0x30");   /* sti 的中断阴影保证 int 先执行 */
```

最后一行是关键：`sti` 之后那条指令执行完才会重新认中断（x86 的一指令阴影），
所以 `int` 一定先跑，不会出现“刚开中断就被 tick 抢走、醒来又立刻让出”的空转。

### 主动让出（`task_yield()`）

同一个 `int 0x30` 还支持更轻的一种让出：任务**不退化成睡眠**，让完仍在就绪队列里
（下一轮该轮到它就轮得到），但**本轮剩余时间片直接算完**：

```c
void task_yield(void)
{
    cpu_cli();
    tasks[cur].left = 0;        /* 关键：把剩下的时间片一起交出去 */
    tasks[cur].wait = 0;
    tasks[cur].prio = tasks[cur].base_prio;
    __asm__ volatile ("sti\n\tint $0x30");
}
```

`left = 0` 不是可选项——`schedule()` 开头那段“时间片没用完、也没更高优先级在等 →
不换人”的短路会把这个 yield 整个吞掉。做过对照实验（把 `left = 0` 删掉重编跑）：

| 版本 | 阶段四结果（3 个同优先级任务各 yield） | 说明 |
| --- | --- | --- |
| 扣掉时间片（现版本） | `Y=3/4/4  sw=0x2EEF`（另一跑 `sw=0x33CE`） | 每次 yield 都真换人，12 个 tick 里切了一万多次 |
| 不扣时间片（对照） | `Y=4/4/3  sw=0x8` | yield 完全无效，只剩 tick 自己造成的 8 次切换 |

所以“yield 要不扣时间片”这个问题的答案是：**必须扣**。否则反复 yield 就成了
“洗时间片”的免费 cputime，和一个每 tick 都主动让出、却永远轮得到的贪心任务等价。

### 自检怎么证明这几件事

自检分七个阶段，每段换一组任务（`task_reset()` 之后重新 `task_spawn`）。为了腾出
屏幕行，阶段一/二并成一行、阶段四的“标签 + 账单”是同一条线：先打标签、
`run_workers()`、再把数字接在同一个行上（这些段的任务体自己不打印，所以不会插进
中间）。省下来的那一行（`row 13`）给调用门和参数所有权这两个对外契约。

**阶段一：时间片。** 两个 CPU 型任务（`demo_burn`，纯忙等、不 sleep 不 yield）
都置 prio=2、slice 取默认（2 tick），一起忙到 12 个 tick 后的终点。谁能把它们切开？
只有 tick 中断 + 时间片。若 slice 不生效，每个 tick 都会换人，切换次数会是 11~12；
若调度只看优先级不轮转，就只会有 1 次切换。实测 `U=6 / V=5 / s1=7`——12 个 tick
基本对半分，7 次切换 ≈ 12/2。差的那一个 tick 来自记账边界（某个 tick 正好落在
退出/换栈的瞬间，记到了 `main`）。

**阶段二：优先级 + 老化保底。** H 是 prio=2（slice 2），L 是 prio=0（slice 8），
老化阈值 3 tick，同样忙 12 个 tick。若没有老化，L 会是 `0x0`（上一版的实测结果）；
有了老化，L 每等够 3 tick 升一级、升到最高级后抢到 2 个 tick 的短片，然后降回原级
——实测 `H=9 / L=2 / sw=4`。**优先级仍然主导（9:2），但饿死消失了**，这正是老化
该给出的形状。这两段账目现在并成一行（`row 12`）：`p1 U=0x06 V=0x05 s1=0x07 |
p2 H=0x09 L=0x02 sw=0x04`——省下的那一行给了 row 13 的两个对外契约。

**两个对外契约（`row 13`）。** 调用门和参数所有权都在这行：
`gate dpl=0x3 deny=0x00 old=0x1E | own dtor=0x01 mag=0x00 seen=0x1 why=0x0`，
展开在两节里讲——「第二个入口：GDT 调用门」和「参数所有权：`dtor`」。
行尾的 `why=0x0` 是“死因”证据的一半，另一半在 `row 21` 的 `why=0x2`：同一个
所有权载荷，走正常 `return` 和走“被守卫杀掉”两条不同的路径，析构拿到的 reason 不同。

**阶段三：睡眠 / 动态创建 / 传参 / 槽位复用。**

| 任务 | 行为 | 证明了什么 |
|---|---|---|
| A | 一进来就 `return` | 返回地址是 `task_return_stub` → 自动退出，槽位与栈页立刻空出 |
| C | 睡 50ms，醒来打一行 | 后创建却先醒 → 睡眠队列按 `wake_tick` 排序，不是先来先醒 |
| B | 睡 100ms，醒来打一行，再带 `0xDEAD0001` 创建 D | 动态创建 + 参数传递（`arg` 两跳：main→B→D） |
| D | 打一行就退出，行里带 `slot` 和 `arg` | `slot=0x1`（A 让出的槽）、`arg=0xDEAD0001` |

参数不是靠全局变量偷渡的：`task_create` 把 `arg` 写进新栈帧的 `frame[12]`，入口函数
从 `[esp+4]` 读到它。B 收到的 `arg` 和 D 收到的 `arg` 都打在屏幕上（`row 16` / `row 17`
各一个 `arg=0xDEAD0001`），第二跳没丢参才算链路成立；阶段五的 4 个 sleeper 也是靠
这个参数拿各自的睡眠毫秒数（`sleeper_wake(void *arg)`），所以参数通路一断，`sq order`
会从 `0x3142` 变成 `0x1432`。

入口为空（`task_spawn(0, …)`）在建任务之前就被拒掉：行上 `null=0x00000000`。不拒的话
iret 会跳到 `eip=0`，白占两页栈还直接三重故障——负向验证里那条“短路这个 if”的补丁，
结果就是**整块屏幕一行都抓不到**。

每行带的 `gap` 是任务**自己栈上的局部变量**（从入口记到打行时）：切换没真的换栈
就会串味；而它同时说明 `task_sleep` 的到期比较是准的——`gap` 应当等于 `ms/10`
（100Hz 下 50ms=5 tick、100ms=10 tick）。

**阶段四：主动让出（yield）真的会换人。** 三个 prio=2 的任务每跑一趟就
`task_yield()` 一次，同样忙 12 个 tick。它们的 tick 账应当是均分的（证明没谁靠
反复让出偷便宜），而切换次数会**远大于** tick 数——阶段一同样 12 个 tick 只有 7 次
切换（一个时间片 2 tick 才切一次），这里每个时间片都被 yield 提前用光。实测
`Y=3/4/4 / sw=0x2EEF`（另一跑 `sw=0x33CE`），一万多次切换。对照实验（不扣 `left`）
只剩 `sw=0x8`，见上一节。

**阶段五：睡眠堆真的按键排序。** 4 个任务睡 100/200/50/150ms，创建顺序正好是
slot 1/2/3/4；每个醒来时把自己的槽位号塞进 `wake_order`（高半字节在前）。醒来顺序
若真按 `wake_tick` 排序，应当是 **3,1,4,2**——既不是创建顺序（1,2,3,4），也不是
“先睡先醒”（同样是按创建顺序），只有按键排序的结构才给得出这个排列。实测
`sq order: 0x3142`。这条被 `verify.py` 写成**等值断言**（不是区间），因为它是
“睡眠队列退化成 FIFO”最直接的指纹：堆一旦忘记上浮，这行立刻变成 `0x1342`。

**阶段六：历史惩罚（衰减）真的会把它压下去。** 一个 prio=2、`slice=1` 的纯 CPU
任务**独跑**（没有别人跟它抢），每片跑满记一笔，4 片降一级，8 个 tick 后从 prio2
落到 prio0；**接着长睡 400ms** 再打一次自己的级别——同一个任务、同一行，前后两段
就是“罚”与“还”的对照：

```
phase6 decay: prio 0x00000002 -> 0x00000000 ticks=0x00000008 rest=0x1/0x0/0x1
```

三个细节让这条证据站得住：

- **独跑也能降级**：说明罚的不是“被抢占次数”（那是老化/抢占的语义），而是它自己
  烧掉的时间片；
- **8 个 tick 是精确值**：`slice=1` 时一片 = 1 tick，降两级正好 8 tick。`verify.py`
  对 `2 -> 0` 做等值断言、对 8~10 做区间断言（留 2 tick 给打印抖动）；
- **必须先有一个能复现的坑**：这条证据是修掉“队里只剩自己时不装填时间片”那个
  bug 之后才成立的——之前 `left` 一直是 0，每 tick 都记一笔，3 个 tick 就降到最低
  级，`ticks` 会打出 `0x3` 而不是 `0x8`；
- **回血那一半是按“睡了多久”算的**：`sched_tune.restore_ticks = 30`（300ms）是
  **回血单价**——每睡满一个单价还一级，睡 k 个单价还 k 级（封顶最高级）。
  worker 睡 400ms（40 tick）够一个单价，醒来 `rest=0x1`；阶段五那种 50ms 的
  排队等不够单价，一档都不还。于是“睡一小会儿就把惩罚洗掉”这条口子被堵住——
  短睡只是把 `hogs` 清零（不再继续往下降），级别要睡够单价才还回来；
- **三段 `rest` 是同一段睡眠、不同阈值的对照**：`1/0/1` 依次是“默认 30 tick 回血”、
  “阈值在线抬到 60 tick 后同一段睡眠不回血”、“改回 30 tick 又回血”。中间那个 `0`
  是**参数真的被读了**的指纹——如果 `task_sleep` 里写死常量，它会变成 `1`；
- **`deep` 两段是“按倍数折算”的对照**：`deep=0x2/0x1` 是两次独立的实验——
  第一次从最低级睡 600ms（60 tick = 2 个单价）**一次还两级**，第二次从最低级睡
  300ms（30 tick = 1 个单价）只还一级。老实现（够阈值就还一级）两段都会是 `1`，
  所以第一个 `2` 就是“线性回血”与“够阈值就还一级”的分界线——它证明的是
  “睡多久就还多少”，而不是“多睡几次才还一级”。

关掉衰减做负向验证（`note_quantum_burn` 一进来就 return），这行立刻变成
`2 -> 2 / ticks=0xC`（12 tick 的兜底期限用完），`make verify` 报红。

**阶段七：栈溢出被杀掉，内核继续活。** 一个任务的入口拿到自己的栈底
（`task_stack_bottom()`），往 `bottom - 4` 写一个字——正好落在 guard 页里：

```
phase7 guard: slot=0x1 va=0x00013FFC stack overflow -> task killed why=0x2
```

三件事同时被证明：`va` 落在 `[0x10000,0x90000)` 内（是那块低端页里的页），`slot`
就是越界的那个任务；紧接着的 `sched: done ... rq=0 sq=0` 说明这个任务已经按**正常
退出路径**被回收（`frames=3/0x80` 说明栈页和 guard 页都还回去了），而 `head` 之后
的 `main` 仍在跑；最后那行 `PF va=0x04000000 unhandled` 仍是**故意**的（用它证明
“不认识的缺页照样停机”这条边界还在）。

`why=0x2` 是这一轮新加的：这个任务带了一个带析构的所有权载荷，缺页处理把现场
`eip` 换成 `task_guard_exit`（不是 `task_exit`），于是 dtor 收到 `reason=2`——
“被守卫杀掉”这件事从**横死的那条路径**里带出来了，而不是被当成正常收工。
槽位号从 8 位压成 1 位（`slot=0x1`）就是为了省出这 8 列。

负向验证：把 `page_identity_unmap` 里清 PTE 那句去掉（guard 页仍然可写但页还在），
这行变成 `phase7 guard: not-trapped`，`make verify` 报两条红（行不一致 + 专门提示
“越界写静默成功说明 guard 没生效”）。**注意**：负向验证要改成“什么都不做”，不能
改成塞一个假 PTE（`pte = 0x07`）——那会把恒等映射里那一格指到物理页 0，属于制造
故障而不是撤掉保护，副作用会盖住真正想观察的失败。

## 调度器拆成 `task.c` + `queue.c`

`kernel/task.c` 原来是 845 行，里面其实是两件事：**任务的一生**（建栈、换栈、
死亡、延后回收）和**谁下一个跑**（三级就绪队列、睡眠小顶堆、老化、时间片记账）。
两者只在 `schedule()` 里握手：它调 `wake_sleepers()`、`rq_top()`、`rq_push()`，
其余时间各管各的。于是按这条缝切开：

| 文件 | 行数 | 拥有什么 | 不许做什么 |
|---|---|---|---|
| `kernel/task.c` | 608 | `task_t` 与 `tasks[]`、`cur/active/switches`、`task_create/reset/spawn*`、`schedule()`、`sched_tick`、`task_sleep/yield/exit`、延后回收（栈页/guard/dtor/租约）、借用表 | 不碰 rq/sq 的下标 |
| `kernel/queue.c` | 256 | `rq[3][5]` + 三级 head/count、`sq_heap` 小顶堆、`slice_for`、`note_quantum_burn`、`aging_pass`、`wake_sleepers` | 不分配栈、不换栈、不认识 dtor 与租约 |
| `include/linux/taskint.h` | 82 | 两边共享的 `task_t`/`tasks[]`/`cur`/`active`/`switches` 的 extern，加上 queue.c 的门面声明 | 不是对外接口（`include/linux/task.h` 才是） |

三条边界约定：

- **状态跟着用它的地方走**。`task_t`/`tasks[]`/`cur` 只有生命周期要写，留在
  `task.c`；`rq`/`sq` 的下标只有队列要动，搬进 `queue.c` 并且**定义在那里**，
  别的地方连读都读不到——想绕过队列手改下标，结构上就没有门。
- `task.c` 不再直接读 `rq_count`/`sq_count`：三个门面（`queue_reset()`、
  `rq_depth_total()`、`sq_depth()`）覆盖了 `task_reset`、`task_rq_depth()`、
  `task_sq_depth()` 的全部需求。
- 对外 API 一个字节没动（`include/linux/task.h` 原样），所以 `init/demo.c`、
  `init/main.c`、`except` 那一侧、shell 侧一行都没改。

代价与收益：kernel 从 56192 涨到 **56672 字节**（`__kernel_end=0x1141C`，
分配区余量 60388 字节），换来的是“队列”可以脱离换栈单独推理——`rq_top(0)`
是 peek、`rq_top(1)` 是 pop、`sq_insert` 相等键不交换，这些现在都在一个
只认 `tasks[]` 三个字段的文件里。验收：`verify.sh -n 3` 三轮 PASS，
`kbd-shell` / `shell-vars` / `shell-console` / `ring3-io` 四个脚本全 PASS。

## 调度参数（运行时可变）

老化阈值、衰减片数、回血阈值、各优先级时间片原来都是 `task.c` 里的 `#define`。
改成一个可写全局结构 `sched_tune`（`include/linux/tune.h` + `kernel/tune.c`），
默认值与原常量逐字节相同（3 / 4 / 30 / `{8,4,2}`），但运行时能改：

```c
struct sched_tune {
    unsigned long age_ticks;      /* 就绪排队多少 tick 升一级（老化） */
    unsigned long decay_quanta;   /* 连续跑满多少片降一级（衰减） */
    unsigned long restore_ticks;  /* 一次睡够多少 tick 还一级（长睡回血） */
    unsigned long slice[3];       /* 各优先级默认时间片 */
};
```

`kernel/queue.c` 里这四处读的都是结构字段（`slice_for`、`note_quantum_burn`、
`aging_pass` 在 queue.c，`task_sleep` 在 task.c），`main` 启动时调一次
`sched_tune_init()`（装默认值 + 挂 0x31 门）。
谁改谁负责：读写都在关中断的上下文里发生（中断门内部，或自检里显式 `cli`），单核下
不再需要更重的同步。

**为什么值得单独做**：阈值是编译期常量时，想验证“阈值在起作用”只能改代码、重编译、
再跑一遍；变量化之后内核能自己改自己——自检在同一个任务里把 `restore_ticks` 从 30
改成 60，同一段 400ms 睡眠立刻不再回血，屏幕上直接出现三段对照
（`rest=0x1/0x0/0x1`，见阶段六）。这条同时也是“参数被真正读到了”的证明：
写死常量会得到 `1/1/1`，负向验证 G 就是这个改法。

### 对外入口：`int 0x31`

只把阈值放进全局变量还不够——外部（另一个任务、将来的用户态）得有办法改。
新增软中断 0x31（IDT 里与 0x30 一样走中断门，桩在 `boot/intr.asm` 的 `isr_tune`）：

```c
eax = 字段号（TUNE_FIELD_AGE / DECAY / RESTORE / SLICE0..2）
ebx = 新值
返回 eax = 旧值；被拒绝返回 0
```

桩的写法和 0x30 几乎一样（`pusha` → 装段寄存器 → `cld` → 把现场地址传进去），
差别只在**不换栈**：处理体 `tune_syscall()` 从 `ctx[7]`（pusha 里的 eax）读字段号、
从 `ctx[4]`（ebx）读新值，把结果写回 `ctx[7]`，`popa` 就把返回值带回任务。

**为什么必须有校验**：把阈值交给外部改，等于把“调度行为”交给外部。一个 0 值的
`restore_ticks` 能让所有任务一睡就回血；一个 0 值的 `slice` 能让某一级任务拿到 0
tick 的时间片。所以 `sched_tune_apply()` 先过 `tune_range_ok()`：老化/衰减 1..100、
回血 1..10000、片长 1..100，不认识的字段号直接拒。拒绝返回 0 是安全的——所有合法
值都 ≥ 1，**旧值不可能是 0**，所以 0 这个哨兵不会误伤。

**屏幕证据**（done 行末尾，省出新行）：

```
row 22| sched: done ... rq=0 sq=0 |tune 1E/00/3C
```

三段分别来自三次 `int 0x31`：`1E` 是第一次改阈值时回传的旧值（30）；
`00` 是拿非法值 0 去改的返回值（被拒）；`3C` 是拒绝之后 `restore_ticks` 的实际值
（还是 60）——最后这个数排除了“先写进去再改回来”的假拒绝。`verify.py` 对这三个数
做**等值断言**，负向验证 H（短路掉范围校验）会让它变成 `3C/3C/00`。

**踩坑：80 列刚好等于“多顶一行”**。一开始这行写成 ` | tune 1E/00/3C`，整行正好 80
列，行尾的 `\n` 于是多走了一行：屏幕出现一条空行、把 banner 挤出可见区（`row 0` 变成
`esp`、`row 22` 变空）。VGA 文本模式下写满一行的最后一列会让光标进入“待换行”状态，
再遇到 `\n` 就跳两级。所以规定：**单行内容必须 ≤ 79 列**（标签从 `| tune` 改成
`|tune` 就是为了省这一格）。

### 第二个入口：GDT 调用门（DPL=3）

`int 0x31` 是**中断门**，它只对内核有用：中断门的 DPL 决定“`int n` 指令能不能被发起”，
DPL=0 意味着 ring3 执行 `int $0x31` 直接 `#GP`。要让将来的用户态也能改参数，得换成
**调用门**——一种放在 GDT 里的描述符，由 `lcall` 触发，DPL 为 3 时 ring3 也能进。

具体做法（`sched_tune_gate_init()`）：

1. 在 GDT 第 3 项（选择子 `0x18`）现场拼一个 8 字节的 386 调用门：
   类型 `0x0C`、`P=1`、**`DPL=3`**、参数个数 0（参数不从栈上复制）、
   目标是内核代码段 `0x08` 里的 `tune_gate_entry`；
2. `boot/intr.asm` 加一个门桩：`pusha` → 装段寄存器 → `cld` → `push esp` →
   `call tune_gate_call` → `popa` → **`retf`**。和中断门桩的两处硬区别：
   栈上是“返回的 CS:EIP”双字（远返回用 `retf` 而不是 `iret`），
   且门帧里**没有 eflags** 这一格（跨特权 `lcall` 压的是 `[EIP][CS][ESP][SS]`，
   别拿异常现场的形状去套）；
3. C 侧入口 `tune_gate_apply()`，与 0x31 **共用同一条写路径**，但带自己的白名单。

**参数 ABI 已经从“两个寄存器”改成“一个指针”**：`eax` 指向
`struct sys_req { nr, a0, a1, a2 }`（四个字）。两个寄存器只够传“号 + 值”，
将来要一次改多个字段就必须有结构体；而一旦开始读用户给的地址，
“这个地址能不能 deref”就成了必须先回答的问题。四个号的表与分派见
「系统调用表」那一节（`nr=0` 就是本节这门 tune 自己）。

**白名单**（`tune_gate_mask`，默认 `0x3C`）：门只放行 `RESTORE` 与三级时间片
（`SLICE0/1/2`），老化 `AGE` 和衰减 `DECAY` **不放行**——那两个直接决定“谁先跑”，
交给外部等于把调度策略交出去。位图第 n 位 = 字段号 n，`0x31` 用 `0x3F` 全放行。
位图是运行时可改的全局变量（自检里临时改小过），所以“白名单”本身也能被拿来验证。

**跨字段一致性**（`tune_slice_order_ok()`）：`slice[0..2]` 必须“低级长、高级短”。
之前只能逐个字段查范围，所以 `slice[0]=1, slice[2]=2` 这种倒挂能写进去——高优先级
拿着更长的片，优先级会被时间片反过来压住。现在是**先写、再整套复查、不一致就回滚**：
跨字段的约束没法预先判，只能写完看。

**可写上限**（`gate_caps[]`）：白名单回答“这个入口能不能改这个字段”，上限回答
“能改到多大”。两者不是一回事——范围校验管的是“值荒不荒唐”（`slice=0` 会让某级
永远跑不完一片），上限管的是“这个入口配不配”（ring3 能把回血阈值抬到 256 tick，
内核自己走 `0x31` 能抬到 10000）。两个都要过；超上限是**拒绝**，不是截断。

**指针先过门禁，再过校验**：`tune_gate_call()` 用门帧里的 `cs` 低两位判来源——
`CPL=0`（内核自己 `lcall`）允许内核指针；`CPL=3`（ring3 打进来）要求指针过
`sys_ptr_ok()`：落在 `tune_gate_user_window()` 注册的窗口内、4 字节对齐、
整个请求体（16 字节）不越界，任何一条不满足就**返回 0、一次也不解引用**。
让用户指针把内核带进 `#PF` 是最糟的失败方式：内核会以为是自己的 bug。
读到的四个字段立刻拷进局部数组再用，不做两遍解引用
（这是“将来搬去多核”的预防，不是当下的必需品）。

**出口两个**：`nr = 2`（`sys_exit`，带退出码，`exec` 会把它读回来）、
老旁路 `nr = 0xFF`（不带退出码）。两者都把返回帧改写成
`(eip=user_return_stub, cs=0x08)`，门桩的 `retf` 就落在内核里——
去哪儿由内核决定，这才是正常方向（用户不能决定内核的 CS）。

### 让门真的对 ring3 可用：TSS + 用户页（row 14）

DPL=3 只解决“谁能 `call`”。`lcall` 从 ring3 进 ring0 要**换栈**，新栈只能来自当前
TSS 的 `ss0:esp0`；没有 `ltr` 过的 TSS，这一步就是 `#GP(0)`，门再合法也进不来。
`kernel/tss.c` 把三件事一起做：

1. GDT 第 6 项（`0x30`）摆 32 位 TSS 描述符：类型 `0x89`、DPL=0、界限 `0x67`（104 字节-1）；
2. TSS 本体只填 `ss0=0x10`、`esp0=专用 4KB 栈顶`（16 字节对齐），
   `iomap_base=104` 表示“没有 I/O 位图”→ ring3 的任何 `in/out` 都 `#GP`；
3. `ltr` 装载，之后 `str` 读回 `0x30`；描述符的 busy 位由 CPU 置上（类型变 `0x8B`）。

同表里再摆两项 ring3 段：索引 4 = `0x23`（代码，`0xFA`/gran `0xC`）、
索引 5 = `0x2B`（数据，`0xF2`/gran `0xC`），都是 4GB 平坦，只有 DPL 是 3。
不做硬件任务切换（不 `ljmp $0x30:$0`），TSS 只当“换栈要用的那一对”用。

**进/出 ring3**（`boot/intr.asm`）：

- `user_enter(eip3, esp3)`：记下 `user_kernel_esp`（里面压着返回地址）→ 装 `DS/ES=0x2B`
  → 压 `SS3, ESP3, EFLAGS=0x002, CS3, EIP3` → `iret`。
  **IF=0 是必须的**：ring3 期间若有中断进来，调度器会在 TSS 栈上跑，那时 `cur==0`，
  它会把 main 的 esp 记成那张栈上的地址，下次切回 main 就 `iret` 到垃圾上。
  出来之后由 C 侧 `cpu_sti()` 补开（`retf` 不恢复 eflags，`lcall` 也不改它）。
- 用户代码是 `boot/intr.asm` 里的 `user_stub … user_stub_end`，**整段拷**进一张
  用户页再执行（stub 只用相对 `call far` 和 esp 相对写，位置无关）。这样内核 text
  页不必对 ring3 开放——方向不能反过来。
- `user_return_stub`：`mov esp,[user_kernel_esp]; pop ebp; ret`，回到 C 调用点。

**用户态那段代码干的四件事**（结果写在自己的栈页上，内核回来取证）：

1. `{RESTORE, 40}` → 门返回旧值 `0x1E`（放行，且证明 `retf` 回到了 ring3、eax 带回了值）；
2. `{RESTORE, 5000}` → 超每字段上限 256，返回 0；
3. `eax=0x04000000`（**未映射**且不在窗口）→ 返回 0，且内核没有替它去 deref；
4. `{0xFF, 0}` → 请求回内核。

**屏幕证据**（row 14，一行 72 列）：

```
ring3: tr=0x30 cpl=0x3 ret=0x1E rej=00/00 set=0x28 cap=0x100 knl=5505 ls=1/3/1
```

- `tr=0x30`：`str` 读回的 TR——TSS 真的挂上了；
- `cpl=0x3`：门处理体里看到的调用者 `cs` 低两位。**0 就说明这一趟其实还是内核
  自己 `lcall` 的**（自检里 `gate_set` 走的是同一条门），这是最容易骗过自己的一个值；
- `ret=0x1E`：用户态自己写下的返回值；
- `rej=00/00`：两次被拒——超上限（5000 > 256）与窗口外指针（`0x04000000`），
  两个都必须回 0（原来分两段叫 `over`/`wild`，为了给 `knl`、`ls` 腾位置合并成
  一个字段并去掉 `0x` 前缀）；
- `set=0x28`：被放行那次真的把 `restore_ticks` 写成了 40（取证后立刻复位成 30，
  免得把阶段六的“默认 30”改掉）；
- `cap=0x100`：上限取自 `gate_caps[]` 表，不是硬编码、也不是“无上限”。
  这里必须用**三位**十六进制打印——`print_hex2` 只打低两位，256 会显示成 `0x00`；
- `knl=5505`：四次越权探测的错误码连写（不是 `0x` 形式），见下一节。

**踩过的两个坑**（都很安静，值得记下来）：

- GDT 描述符里的“粒度/标志”是**半字节**（G|D/B|L|AVL），不是“粒度字节”。
  第一版把 `0xCF` 整个左移 52，限长高位 `0xF` 溢出到 bits 56-59，被 CPU 当成
  `base_hi` 读成 `0x0C` —— `cs=0x23` 看着完全正常，段基址却悄悄变成 `0x0C000000`，
  取指线性地址成了 `0x0C013000`，屏幕上只看到一条“unhandled PF at 0x0C013000”。
  现在 `gdt_write()` 只接受 `flags & 0xF`，限长高位单独由 `limit >> 16` 填。
- 用户 stub 第一版把三个返回值写在 `[esp-4]/[esp-8]/[esp-12]`，同时又用 `sub esp,8`
  复用同一段栈当请求结构体——结果“第二次调用把第一次的结果覆盖成 5000”这种
  自我踩踏，屏幕上表现为 `over=0xFF`（最后一次 `{0xFF,0}` 的结构体字段）而不是
  预期的 0。现在是“先 `sub esp,32` 留出结果区，结构体固定放 `[esp]/[esp+4]`”，
  两个区域不重叠。

### 收口恒等映射的 U/S：`page_lock_kernel` 与四次探测（`knl=5505`）

上面那步留下一个空洞：`page_mark_user()` 名义上是“放行一页给用户态”，实际上
`head.asm` 建恒等映射时**每条 PTE 都带了 `0x07`**（实测 PTE `0x13067`/`0x14067`，
U/S 本来就是 1）。于是 `page_mark_user` 只是个保险，删掉它屏幕一点变化都没有——
“用户态能访问什么”从来没有被真正关上过。`mm/page.c` 因此补了一对正反接口：

```c
unsigned int page_lock_kernel(unsigned int lo, unsigned int hi);  /* 清 U/S，返回清掉的页数 */
unsigned int page_mark_kernel(unsigned int phys);                 /* 收回一页的 U/S，返回是否真关了 */
```

`page_lock_kernel` 逐页把 `[lo, hi)` 的 PTE 清成 `pte & ~0x04` 并 `invlpg`，跳过
PDE/PTE 不存在和本来就没开 U 的格；起点先向下页对齐。**只动 PTE 不动 PDE**：
清了 PDE 的 U 位，整个 4MB 里就再也不能有用户页，ring3 的 demo 自己会先死。

**为什么范围是整张恒等映射（0–16MB），不是只锁 1MB**：1MB 只盖住内核镜像
（~`0x9AD4`）、页表（`0x1000`）、VGA（`0xB8000`）、页分配器（`0x10000–0x90000`）；
而 `0x100000–0x1000000` 那 15MB 空白内存**同样是每页 `0x07`**（P|RW|U），ring3 拿
一个常数指针就能读、还能写。上一版只锁 1MB，等于把大门锁了、窗户留着——这就是
`knl` 第二位专门钉的那件事。现在 `KERNEL_LOCK_END` 直接等于 `0x01000000`，
`main` 调 `page_lock_kernel(0, KERNEL_LOCK_END)`，一次锁完整张表。

一次清掉之后，分配器发出去的空闲页、任务栈页也一律“只有 CPL0 能用”——任务本来
就在内核态跑，不需要 U；ring3 想碰任何一页，都必须先被 `page_mark_user` 显式放行。
**先后顺序是死的**：`page_lock_kernel()` 必须在两次 `page_mark_user()` **之前**，
反过来会把刚放行的页一起清掉。

顺带把按需分页窗口也改成了“只给内核”：`page_fault_map()` 建 PDE/PTE 时用的不再是
`0x07` 而是 `PTE_KERNEL 0x03`。窗口在 32MB，本来就在锁定范围之外，靠 `page_lock_kernel`
够不着——那就在源头不给 U。

**怎么证明 CPU 真的在拦，而不是“恰好没人去碰”**：`boot/intr.asm` 里紧跟 `user_stub`
之后是第二段用户代码 `user_probe`，位置无关、和 `user_stub` 连着一次拷进同一张用户页。
它只做一件事：从 `[esp-4]` 取出内核写好的目标地址，然后 `mov eax,[ecx]` 去读。
目标地址从栈上取而不是编进指令里，是为了**一段代码探四个不同地址**，不用拷四份。

`exception_handler_pf` 因此多了第 4 条出路：

```c
if ((error & 0x04) && cr2 < KERNEL_LOCK_END) {   /* 用户态越权碰我们自己锁掉的区 */
    ring3_fault_va = cr2; ring3_fault_err = error;
    frame[PF_FRAME_EIP] = user_return_stub;      /* 把返回帧改成“回内核” */
    frame[PF_FRAME_CS]  = KERNEL_CS;
    return ctx;
}
```

两个条件缺一不可：`err & 0x04` 说“这次是用户态发起的”，`cr2 < KERNEL_LOCK_END`
说“落点确实是我们故意关掉的页”。只判前一半的话，ring3 在别处缺页也会被当成
“被拦住”悄悄放回内核，把真正的 bug 藏起来。

这条不是“任务跑炸了”，而是“权限位把不该发生的事拦住了”，所以既不杀任务也不停机，
只把控制权交回内核。回到 CPL0 之后 `iret` 只弹 3 项（EIP/CS/EFLAGS），
`esp3/ss3` 两项留在 TSS 栈上、无害；落到 `user_return_stub` 时它第一句
`mov esp,[user_kernel_esp]` 正好把栈切回 main，兜住了这个偏移。

**四次探测与 `knl=5505`** 四位数字依次是：

| 位 | 目标 | 期望 | 它证明什么 |
| --- | --- | --- | --- |
| 0 | `0x8000`（内核 text） | `5` | 1MB 以内锁住了（`err=0x05` = P\|U、读） |
| 1 | `0xA00000`（1MB 以上空白页） | `5` | 锁的范围确实盖到 16MB，不是只锁了 1MB |
| 2 | 用户栈页（刚放行） | `0` | **正对照**：被放行的页 ring3 真读得到，5 不是缺页处理的默认输出 |
| 3 | 同一页（`page_mark_kernel` 关上后） | `5` | 反向接口真的生效，“临时放行、用完关上”这条路通了 |

第 3 次探完再 `page_mark_kernel(user_code)`，把代码页也关上：此后整个地址空间对
CPL3 全关，页框仍在分配器名下（`frames` 还是 5），只是没有一页还留着门。

**三个负向都咬**（都是脚本化跑一遍 `verify.sh` 看的真实输出）：

- 删掉 `page_lock_kernel()` 调用 → 屏幕**照样 24 行**，但 `knl=0000`：四次读全都成功。
- 把范围退回旧的 1MB（`page_lock_kernel(0, 0x00100000U)`）→ `knl=5005`：第二位变 0，
  1MB 以上那 15MB 果然没锁住。这一条就是“为什么必须锁 16MB”的直接证据。
- 删掉 `page_mark_kernel(user_stack)` → `knl=5500`：末位变 0，那页关上之后还能读。
- 删掉 `page_mark_user(user_code)` → ring3 连取指都过不去，同一分支把它劫持回内核，
  于是 `cpl=0x0 ret=0x00 set=0x1E`（门一次都没调进来）。**同一个负向，在收口之前
  是不咬的**——这正是“`page_mark_user` 从保险变成唯一开关”的证据。

`lk`（被清掉的页数）不进屏幕：列宽不够，而且它的效果已经由 `knl` 直接证明。

### 给“放行”上账：用户页租借（`ls=1/3/1`）

`page_mark_user` / `page_mark_kernel` 是一对**裸开关**：谁放行谁必须记得关，
忘了就是永久开着，而且没有任何计数器能发现这件事。`mm/lease.c` 把“放行”改成
一次**租借**，代价只有一张 8 槽的静态表：

```c
int page_lease_open(unsigned int owner, unsigned int phys, unsigned int ttl);
int page_lease_close(unsigned int owner, unsigned int phys);
int page_lease_release_owner(unsigned int owner);   /* 一次收掉某 owner 名下全部 */
unsigned int page_lease_tick(void);                 /* 每 tick：ttl--，到 0 强制回收 */
```

**键是 `(owner, phys)`，不是 `phys`**。这是 v1 到 v2 唯一实质性的改动，也是它必须
改的原因：v1 只有 `phys` 一个键，于是两张租约落在同一页时，`ttl` 谁长谁说了算——
A 借一小时、B 借一秒，B 死了 A 还在，页就一直开着，**“忘了还”这件事故意被别人的
长 ttl 续命**。v2 里每个借用者一行：

- `owner = PAGE_LEASE_OWNER_KERNEL (0xFF)` 是内核自己借的；任务是 `task_self()` 的槽位号；
- 同一页可被不同 owner 各借一份，`(owner, phys)` 相同才合并 `refs`；
- **只有“这一页最后一个租约消失”才 `page_mark_kernel`**（`lease_page_held(phys)` 判定），
  中间任何一个 `close` 都只递减，不动 PTE。

四条规矩：

- **`open` 同一页多次 = 多个借用者**，每一个都必须各自 `close` 一次；
- **`close` 只递减**，`(owner, phys)` 的 `refs` 归零只是“这一行没了”；页真正关上要
  再看还有没有别的 owner 在里面。返回值就是“这一页现在关上了吗”；
- **`tick` 是“忘了关”的兜底**：`ttl` 到 0 就强制回收这一行（不管 `refs` 剩几），
  页若因此失去最后一个租约就强制关上、`forced` +1；
- **`release_owner(owner)` 是“借的人死了”的兜底**：一次收掉该 owner 名下全部租约，
  `revoked` 累加“因此被真正关上的页数”。`owner==0 || owner==0xFF` 直接返回 0——
  内核自己借的页不随任务回收走。

槽位满、页没映射、`ttl=0` 时 `open` 一律返回 0 并计入 `denied`——**明确失败，
不静默成功**。这是这个内核里最容易被忽略的一条：`page_mark_user` 原来对
“没映射”也只返回 0，调用方多半没看。

**账要有人按时收**：`page_lease_tick()` 现在挂在 `sched_tick()` 里（`aging_pass()`
之前），也就是真时钟中断每 10ms 走一遍。中断上下文里只有“线性扫 8 槽 + 必要时一次
`page_mark_kernel`（含 `invlpg`）”，**不能睡、不能等锁**——这条约束决定了表只能是
静态定长数组。

**主人死了要能收回来**：`schedule()` 的 `TASK_DEAD` 分支把死掉的槽位记进
`reclaim_owner`，`task_reclaim` 的 `release_pending()` **最后一步**才
`page_lease_release_owner(reclaim_owner)`。放最后是因为 `dtor(arg, reason)` 里可能
还要合法地读它借的那一页——先收页、再回调，回调就会读到被收走的页。回收完把
`reclaim_owner` 清 0。

**屏幕证据**（row 14 末尾，`knl` 后面）：

```
ls=1/3/1
```

三位分别是 `page_lease_forced()` / `page_lease_revoked()` / 撤销通知：

- `forced=1`：代码页**故意忘了还**，手动 `tick` 到 `ttl=0` 时被强制回收了 1 页；
- `revoked=1`：一个借了页就 `return` 的任务（`worker_lease`）从不 `close`，
  它的页由 `task_reclaim` 收回来了 1 页；
- 末位 `1`：见下一节——**通知真的来了，而且是在 `dtor` 之后到的**。

前两个数字是**两条独立的兜底路径**：一个走时钟、一个走任务死亡。
（中一位从 `1` 变成 `3` 是因为演示任务现在借 5 页、当场还 1 页：超时的只有代码页 1 页，
按借用表随任务收走的是剩下 3 页。）

**ttl 为什么从 10 提到 200**：`tick` 挂进真中断之后，租约会在自检跑的过程中被
真时钟提前扣账，`forced` 就可能在 ring3 那趟之前就 +1，证据被弄脏。把 ttl 提到
200（约 2 秒）远大于自检时长，超时只可能发生在 `main` 手动那句 `for` 里。

`knl` 中间那位与末位也跟着改了语义，现在它们是**引用计数**的证据：

| 位 | 场景 | 期望 | 含义 |
| --- | --- | --- | --- |
| 2 | 栈页 `refs` 从 2 还到 1，再探同一页 | `0` | 还有借用者 → 页**仍开着**，ring3 读得到 |
| 3 | `refs` 归零后，再探同一页 | `5` | 最后一个借用者说了算，U/S 真的收回去了 |

这两位数合起来说明“关一次不等于关掉”——这正是裸开关时代做不到的区分。

### 撤销要通知：`bind` 与 `notify`

把页收回来只解决了“后门关上”，没解决“借用者那边的体验”：它下次访问才发现页
没了——而那一次访问要么是 `#PF`，要么是一段静默的脏数据。所以补一条**可撤销契约**：

```c
typedef void (*page_lease_notify_fn)(unsigned int owner, unsigned int phys, unsigned int why);
int  page_lease_bind(unsigned int owner, page_lease_notify_fn cb);   /* 登记 */
int  page_lease_unbind(unsigned int owner);                          /* 注销 */
unsigned int page_lease_notified(void);                             /* 已发通知数 */
```

三条规矩，每条都能在屏幕上咬出来：

- **只对“不是自己还的”两种收法发通知**：`why = PAGE_LEASE_WHY_TTL (0)`、
  `WHY_OWNER (1)`；`WHY_CLOSE (2)` 是自己的动作，它本来就知道，再通知一遍是噪音。
- **通知必须晚于 `dtor`**：`dtor` 里还可能合法地用着那页（`arg` 就是页地址），
  所以 `release_pending` 的顺序是“还栈/guard → `dtor` → 收租约 → 通知”。
  这条在代码里是顺序，在屏幕上靠一个哨兵验：`dtor` 死前往页里写
  `LEASE_SENTINEL 0xC0FFEE`，通知回调只有在 `why==OWNER` **且看到哨兵**时
  才把 `lease_notice` 置 1。所以末位那个 `1` 是“两件事同时成立”，不是“回调被调过”。
- **回收 = 解约**：`page_lease_release_owner()` 收完租约顺手 `unbind(owner)`。
  任务槽位是复用的，旧回调指针留在表里等于让下一个任务继承一个野指针。

**为什么回调表是另一张小表（4 条）而不是塞进租约槽**：回调是“按 owner”的，
租约是“按 (owner, phys)”的——一个 owner 借 3 页只有一个回调；解约时也是按 owner
一次性注销。混在一起会让 8 个槽里出现 3 份同一个指针。

**负向（证明末位是活的，不是常量）**：

- 把租约回收挪到 `dtor` 之前（`tools/neg.sh -1 notify_order`：在
  `reclaim_dtor(...)` 那一行前面插一段 `page_lease_release_owner`）→ `ls=1/1/0`。
  前两位都不动（页照样收回来了），**只有末位掉到 0**——因为通知到的时候 `dtor`
  还没跑，页里没有哨兵。这条负向同时钉住“顺序写死在代码里”和“末位确实是时序证据”。

### 回收要有索引：每任务一张借用表（`lt=0x1/0x4`）

上面两条兜底都靠“有人按时来收”，但**收的时候怎么知道该收哪几页**？原来的答案是
“线性扫 8 个全局槽、owner 对上的就收”。它能用，但有两个毛病：一是 O(全局槽) 且
扫的是别人的账；二是任务借到第 5 页时没有任何上限，8 个槽会被一个任务吃光。

所以把账记在**借的人自己身上**：

```c
#define TASK_LEASES 4
int task_lease_note(unsigned int slot, unsigned int phys);   /* 借一条记一条 */
int task_lease_forget(unsigned int slot, unsigned int phys); /* 还了就划掉（不搬动其它格）*/
unsigned int task_lease_at(unsigned int slot, int idx);
unsigned int task_lease_peak(void);                          /* 水位，自检用 */
```

三条规矩：

- **记不下就不借**：`page_lease_open` 在 `page_mark_user` 之后紧跟
  `task_lease_note`，后者返回 0（表满）就**把刚放开的 U/S 收回去**、`denied++`、
  返回 0。宁可借不出来，也不留一笔“放行了但没人记账”的租约——那种页只会等 ttl 兜底。
- **回收按表走**：`page_lease_release_owner()` 现在按下标遍历这张表，
  表里有几条就 `lease_find` 几个槽位。表里没有的页一定不是它借的，
  “收错别人的页”从结构上就不可能，也不再有 8 槽全扫。
- **表活到回收那一刻**：`schedule()` 的 `TASK_DEAD` 分支**不清**这张表——
  清了就没得收了。清表放在 `release_pending()` 收完租约之后（同一处，
  在 `dtor` 和 `release_owner` 之后）。

**屏幕证据**（`lt=` 打在两处以外最空的一行——row 17，`D slot=`/`arg=` 那条）：

```
lt=0x1/0x4
```

- 前一位 `1`：第 5 页被借用表拒了（`denied`）。演示任务借 5 页、表只有 4 格，
  所以必须正好拒绝 1 次；
- 后一位 `4`：表的历史最高水位顶到容量 4——证明表真的被填满过，
  不是“看起来能借 4 页”。

配合 row 14 的中一位一起读：任务借 5 页、当场还 1 页，回收时表里剩 3 条 →
`revoked=3`。所以 `ls=1/3/1` 与 `lt=0x1/0x4` 是同一件事的两个视角：
**借的时候拒绝得对、死的时候收回得对**。

**负向（证明表不是装饰）**：

- 让 `task_lease_note` 永远返回“成功”但从不真记（`tools/neg.sh -1 lease_table`）→
  `lt=0x0/0x0`、`ls=4/0/0`：第 5 页借出去了（不再拒绝）、水位 0、回收时表里空空
  所以一页也没收；那 3 页只能等 ttl 到 0 被强制回收，`forced` 从 1 涨到 4。
  一条补丁同时咬两行，说明 `lt` 与 `ls` 中一位确实由这张表驱动。




**负向**（都能咬，且指向不同的兜底路径）：

- 删掉 `release_pending()` 末尾那句 `page_lease_release_owner(reclaim_owner)`
  （`tools/neg.sh -1 lease_owner`）→ `ls=1/0/0`：超时那路还在，任务死亡那路收了 0 页。
  这是 v2 唯一能证明 `revoked` 是真 1 的证据——**同一串数字在不同版本下含义不同，
  只有拿掉某一半才能看出这一半是不是真的在跑**。
- 让 `close` 不看 `refs`（一还就关）→ `knl=5555 ls=0/1`：第 2 位从 0 变 5，
  且前一位变 0（那一页在 refs 还有 1 的时候就被关掉了）。
- 删掉 `tick` 里的 `ttl--` → `ls=1/0`：末位变 0，忘记还的代码页再也收不回来
  （`knl` 四位不受影响——说明这两条证据是**互不顶替**的两件事）。

**这一轮没做的**：`page_fault_map` 仍然不看缺页是“谁”发起的（`error & 0x04`）。
想过在它里面加一句“用户态不允许触发补页”，但这条路径一旦真被触发就会直接走到
“unhandled → 停机”，屏幕上什么都留不下——**没有可观测证据的功能不如不做**，
留到有了“ring3 缺页也走劫持回内核”的计数器之后再说。

- `dpl=0x3` 不是打印常量，而是 `tune_gate_dpl()` 从 GDT 第 3 项里**读回来**的
  （取描述符的 45..46 位）。写错一位就会读到 0/1/2；
- `deny=0x00`：拿老化字段（白名单外）去走门 → 返回 0，`age_ticks` 保持不变；
- `old=0x1E`：拿回血阈值（白名单内）去走门 → 返回旧值 30，值原样写回；
- 门桩的返回路径踩过一次坑：一开始在 `add esp,4` 之后写了
  `mov [esp+28], eax`，于是把 C 函数的返回值（现场地址）当成结果又覆盖了一遍 eax，
  屏幕上打出 `deny=0xE8 old=0xE8`（栈地址低字节）。处理体已经把返回值写回 `ctx[7]`，
  `popa` 本来就会把它弹回 eax，这行是多余的。

## 参数所有权：`dtor`

传参只是“把指针递进去”，真正难的是**它什么时候能释放**。原来的 `task_spawn` 只把
`arg` 塞进栈帧，之后就不管了——任务死了，`arg` 该由谁收尾没有答案，于是所有调用方
都得自己记住“我这个指针还被哪个任务引用着”。

`task_spawn_owned(entry, arg, dtor, prio, slice)` 把这个约定写死：

- `dtor == NULL`：`arg` 仍归调用方，调度器不碰（老 `task_spawn` 就是这条路径）；
- `dtor != NULL`：**spawn 成功那一刻起 `arg` 归任务所有**。任务无论怎么死
  （入口 `return`、`task_exit()`、被 guard 页杀掉），都会在调度器的回收点被交接
  一次：先还栈页与 guard 页，再 `dtor(arg, reason)`，然后计数 +1；
- **`dtor` 的第二个参数是死因**（`TASK_DEATH_RETURN 0` / `TASK_DEATH_EXIT 1` /
  `TASK_DEATH_GUARD 2`）。三条死亡路径都必须先记死因再进 `int 0x30`：入口
  `return` 掉进 `task_return_stub` → 0，自己调 `task_exit()` → 1，栈溢出被守卫
  杀掉时缺页处理把现场 `eip` 换成 `task_guard_exit` → 2。
  为什么要多这一个参数：同一个 `arg` 在不同死法下该做的事不一样（正常收工记账、
  横死打点告警、被取消回滚）。不传死因，`dtor` 只能一律当“正常结束”处理，
  被 guard 杀掉的任务会悄悄走正常收尾——这类错误在日志里看起来跟成功一样。
- 只调一次：回收点把 `reclaim_dtor`（连同 `reclaim_reason`）清空，重复
  `task_reclaim()` 不会重复析构。
- 调用时机是**换到别的栈之后**（此时任务自己的栈已经是僵尸页，随后就被 `page_free`），
  所以 `dtor` 里不能假设自己跑在任务上下文里——它在 `task_reclaim()` 的调用点上执行，
  中断状态跟随调用方（`schedule()` 里是关中断的）。

**屏幕证据**（row 13 后半段 + row 21 行尾）：

```
| own dtor=0x01 mag=0x00 seen=0x1 why=0x0            <- 入口 return，死因 0
phase7 guard: ... stack overflow -> task killed why=0x2  <- 被守卫杀掉，死因 2
```

自检给任务一份 `struct own_job { magic, id }`（`magic=0x600DF00D`），析构的职责就是
把 `magic` 清零并记下死因。于是 row 13 上的四个数分别证明：`seen=0x1` 任务活着时
**读到了**载荷（传参对）、`dtor=0x01` **恰好析构一次**、`mag=0x00` 载荷**真的被释放**、
`why=0x0` 死因是“正常返回”。row 21 的 `why=0x2` 则来自另外一条路径：那个任务
带着同样的所有权载荷去写穿栈，被 guard 页杀掉，析构收到的仍然是 2 而不是 0。
`verify.py` 四条都做等值断言。


## 任务栈的 guard 页

4KB 的栈溢出从来不报错：栈往下越界写，写的是**下一张物理页**——可能是别的任务的
栈、可能是页表、也可能什么都没坏，于是 bug 变成“过几天随机崩”。guard 页把这件事
变成一次干净的 `#PF`。

做法有三步，每一步都有个必须讲清楚的理由：

1. **要连续两页**（`page_alloc_pair()`）：低的那页当 guard、高的那页当栈，只有
   紧挨着，“往下溢出”才必然落到 guard 上。位图里相邻两位同时为空就算命中；
2. **撤掉 guard 的恒等映射**（`page_identity_unmap(phys)`）：任务的 `esp` 就是物理
   地址（head 建的 0–16MB 恒等映射），所以“这页访问不到”只能靠把恒等映射里那一格
   PTE 清零 + `invlpg`。物理页仍归分配器所有，不会被别人拿走——它是“存在但不可见”；
3. **归还前恢复**（`page_identity_restore(phys, pte)`）：guard 页最终要还给分配器，
   而 `page_alloc` 的清页是**通过恒等映射写**的。不恢复映射就还页，下一次有人拿到
   这一页时一清零就缺页。所以存下原 PTE、归还前原样写回。

**缺页处理据此分流**。桩改成 `isr_page_fault` 把现场地址（`esp`）传给 C，C 可以改
现场并返回新的 `esp`：

| 命中 | 处理 |
|---|---|
| 地址落在某个任务的 guard 页 | 打一行 `slot=.. va=.. stack overflow -> task killed`，把现场里的 `eip` 改成 `task_exit`，`iret` 之后该任务走进**正常退出路径**（槽位立刻空出、栈页与 guard 页延后回收），内核继续跑 |
| 按需分页窗口内、页不存在 | 补页 + `iret` 重试原指令 |
| 其余 | 打现场、停机 |

为什么不是直接 panic：栈溢出不是“程序写错了地址”，而是“这个任务跑炸了”。让一个
任务把整个内核拖下水是最糟的结果；把它变成“一个任务死亡 + 内核存活”，才叫隔离。
阶段七就是这条路径的实证（见下）。

**这里踩过一个坑**：现场里错误码和 `eip` 的位置。`pusha` 之后 `ctx[0..7]` 是 8 个
寄存器，错误码在 `ctx[8]`、`eip` 在 `ctx[9]`。一开始按 `ctx[0]/ctx[1]` 读，等于把
`edi` 当成错误码——于是“缺页(not-present)”被判成“保护错误”，补页逻辑直接放弃，
开机的第一个缺页就变成 `err=0x00000001 (protection)` 停机。

**屏幕预算的连锁反应**：阶段七要一行，但屏幕只有 24 行可用（写第 25 行立即上滚）。
省出来的那行是把 `readback` 和 `unmap` 并成一行——它们之间没有缺页输出打断，所以
可以合；合并后行宽 69 列，仍在一行 80 列内。顺带把 `frames` 从这行去掉（紧跟的
`touch again` 行本来就打着同一个数）。

## VGA 光标

文本模式下“字写在哪”和“光标停在哪”是两套独立状态：字符由软件变量 `cursor`
决定写进显存哪一格，而屏幕上那个闪烁方块由 **CRTC** 的两个寄存器决定。CRTC 通过
`0x3D4`（索引）/`0x3D5`（数据）访问，光标位置占 `0x0E`（高字节）、`0x0F`（低字节），
形状占 `0x0A`（起始扫描线，bit5 关闭光标）、`0x0B`（结束扫描线）。

不做同步时的现象是：屏幕文字已经排到第 10 行，闪烁光标还停在 BIOS 输出结束的位置。
所以 `vga.c` 在三个出口统一 `cursor_flush()`：

| 出口 | 为什么必须刷 |
|---|---|
| `vga_putc()` 每个字符结束 | 普通字符、换行、整屏上滚之后位置都变了，一处统一收口 |
| `vga_clear()` | 清屏后光标必须回左上角，否则旧光标停在空白处 |
| `vga_cursor_set()` | 主动搬光标后要立即生效 |

对外接口（`include/linux/vga.h`）：

- `vga_cursor_position()` 软件记录的位置；`vga_cursor_hardware()` 从 CRTC 读回，
  两者相等才算同步成功——这是自检能拿到实证的地方；
- `vga_cursor_set(pos)` 直接把光标搬到某个字符格（越界钳到 1999），不动显存内容；
- `vga_cursor_shape(start, end)` 设形状，文本模式用 14/15 得到下划线光标；
  `vga_cursor_shape_read()` 把起始/结束扫描线打包成 `0xSSTT` 便于一次打印；
- `vga_cursor_show(0/1)` 操作 `0x0A` 的 bit5 隐藏/恢复光标，不必猜扫描线。

同步代价是每个字符 4 条 `outb`（索引 + 数据 × 2 个字节）。在 QEMU 里可以忽略，
真机上约几微秒；若要批量输出把 flush 提到 `printk` 收尾即可，当前没有这个必要。

## 每向量中断计数：`vec_hit` + shell `irq`

`kernel/stat.c` 记的是**业务事件**（补了几页、杀了几个任务、shell 跑了几条命令）。
缺的是另一把尺子：中断和异常**本身**来了几次。这两件事不能互相替代——
`stat pf=0x003` 说明缺页处理器干了三件活，但看不出 CPU 一共撞了多少次 `#PF`；
反过来，`tick` 涨得快也不能说明时钟在干活，可能只是在空转。

于是加了一层极薄的计数：

- `include/linux/vector.h`：`VEC_SLOTS 0x40`（0x00–0x3F 覆盖 0–15 号异常与前 16 条 IRQ，
  够放 0x30/0x31 这两个软中断），三个函数 `vec_hit(v)`、`vec_count(v)`、`vec_total()`；
- `kernel/vector.c`：`static unsigned long hits[VEC_SLOTS]`。`vec_hit` 越界直接忽略——
  "不认识的向量宁可漏计，不许写坏内存"；`vec_count` 越界返回 0。

写和读彻底解耦：打点的一方不知道谁在读，读的一方不知道谁在写。六个打点位：

| 向量 | 位置 | 含义 |
| ---- | ---- | ---- |
| `0x0D` | `exception_handler_gp` | ring3 探端口、门参数越权都会走到这里 |
| `0x0E` | `exception_handler_pf` | 按需分页补页、以及补不了的那次 |
| `0x20` | `irq0_timer_handler` | 8254 100Hz 真的送来了几次 |
| `0x21` | `irq1_keyboard_handler` | 键盘 IRQ 次数（与 `kbd` 的轮询计数独立） |
| `0x30` | `task_yield_handler` | 主动让出的**入口**次数（和 `sw=` 切换次数不是一回事） |
| `0x31` | `tune_syscall` | 调参入口次数（GDT 门调用不算在这里） |

看的是 shell 命令 `irq`，不是自检屏：24 行已经排满，新观测数据的默认落点在用户页。

```
> irq
irq gp=0x000001 pf=0x000008 tick=0x000222 kbd=0x000008 yield=0x028718
      tune=0x000004
```

两个细节值得记：

- **六位宽，不是三位**。现成的 `hex3` 在 `v > 0xFFF` 时会静默钳到 `0xFFF`，
  而 100Hz 的时钟 41 秒就撞上限——"看起来没涨"的计数器比没有更糟，
  所以 `cmd_irq` 自己带一个 `hex6`。
- **一行放不下就换行**，续行缩进 5 空格，拼串前先算 `k + 14 > SHELL_OUT_COLS`——
  和 `stat` 用的是同一套收口，不靠事后截断。

`tools/shell-irq.sh` 是这条路的端到端验证，证明力来自**两种独立读法能对上**：

1. A 段用 QMP 的 `xp` 直接读 `hits[]` 的原始格子（`nm` 找 `hits`、按 `4*vec` 偏移）；
2. B 段真按键敲 `irq` ⏎，用 `pmemsave 0xb9000` 拿用户页、`vga-screen.py` 解码后
   从屏幕文字里把数字抠出来；再顺带断言敲键让 `0x21` 上涨、`pf` 两读一致、
   `tick` 只增不减（它是活的，中间又涨了几百次）。

写这个脚本时踩了个坑：`xp /1dw 0x$((0x$a + 4 * 0x20))` 里 `$(( ))` 算出来是**十进制**，
再拼上 `0x` 就被当成十六进制读，地址偏了一大截（0xE240 变成 0x57920）。
正确写法是先 `printf '0x%x'` 再拼。这类错误非常安静——读回来是 0，看着像"没打点"。

体积代价：kernel 56736 → 57296 字节（`__kernel_end=0x1179C`），
556 字节换来六个向量的实测次数加一条 shell 命令。再加后来的 `tasks` 命令、8 行输出缓冲与"光标跟着输出走"，当前是
**58760 字节 / `__kernel_end=0x11EB8`**，分配区余量 58024 字节。

两个后来补上的小口子：

- **读非法向量号返回 `VEC_BAD`（`~0UL`），不再返回 0。**"这条向量一次没来过"和
  "你给的号根本不存在"是两件事，共用一个 0 会把它们混成一件；`irq` 遇到它就打 `??`。
- **末行多打一个 `sum=`（`vec_total()`，覆盖 0..0x3F 全部槽位）。** 我们点名的只有
  六个，但内核被中断的总次数可能更多；`sum` 小于任何一条就是账错位了，
  `tools/shell-irq.sh` 就是按"六条 ≤ sum 且 sum ≥ tick/kbd"断言的。

## `tasks`：每个槽位吃了多少 CPU

`sw=` 只说"换了多少回"，说不出"换了多少 CPU"。这条命令补的就是后半句：

```
> tasks
t0 run p=0 s=1/0 c=0x00007A r=0
t1 unused p=2 s=0/2 c=0x000000 r=1
t2 unused p=0 s=1/0 c=0x000000 r=0
t3 unused p=0 s=1/0 c=0x000000 r=0
t4 unused p=0 s=1/0 c=0x000000 r=0
```

每行的含义：

| 字段 | 含义 |
| ---- | ---- |
| `t<slot>` | 槽位号（0 = main 的空转上下文，1..4 = worker） |
| 状态 | `run`（正在跑）/ `ready` / `sleep` / `dead` / `unused` |
| `p=` | 当前优先级（老化会临时抬，看到的可能高于 `base_prio`） |
| `s=a/b` | 配置的时间片 / 本轮剩余；差得越多说明这一轮快用完 |
| `c=` | **累计占用的 tick 数**（100Hz 下 1 tick = 10ms），换人不清零 |
| `r=` | 被选中次数 |

三个实现上的决定值得记下来：

1. **main 的 tick 也记账**（`sched_tick` 里 `tasks[cur].ticks++` 移到了 `cur != 0`
   判断之外），但**时间片只对 `cur != 0` 递减**——main 没有 slice 可扣。
   不记 main 的话，自检结束后所有 worker 都已回收，屏幕上会有一列全是 0，
   这条命令就什么也证明不了。
2. **槽位回收不清 `ticks`/`runs`**（`task_reclaim` 只清 `stack/guard/esp/state`），
   所以 `t1 unused … r=1` 这种行是"这个槽位确实被用过"的残留证据。
   反过来说，一次任务在**同一次 tick 内**跑完（比如入口只 return）就真的记不到
   tick，`c=0` 是诚实的 0，不是漏记。
3. **输出缓冲从 4 行提到 8 行**（`SHELL_OUT_LINES`）：`tasks` 本身就是 5 行，
   缓冲只有 4 行时最旧的 slot 0 会被自己的输出挤掉——"命令打不下自己的结果"
   比多占 316 字节糟得多。

`tools/shell-tasks.sh` 的验证方式是"账 + 屏"两读对账：A 段直接读 `tasks[]` 里
`ticks` 的原始格子（`sizeof(task_t)=88`、`ticks` 偏移 24，这两个数是用
`char probe[sizeof/offsetof]` 探针在 VM 上实测的，改结构要跟着改脚本），
B 段按键敲 `tasks` 再解码屏幕，断言 `slot 0` 的 `c=` 落在"两次内存读数之间"，
同时 `sum(ticks) ≤ 时钟向量计数`。

## `make check`：一条命令跑完全部回归脚本

回归脚本多了之后，两件事全靠人手记：**QEMU 只能一个一个起**（必须串行）、
**每次起机前先 `killall`**（否则撞端口或读到上一轮状态）。`tools/check-all.sh`
把这两条固化下来，只负责"跑 + 汇总"，判定标准仍写在各脚本内部：

```
$ make check
  PASS  verify.sh        全部通过（2 轮）
  PASS  kbd-shell.sh     PASS  shell 解释器：行协议/引号/未知命令/超参数/端到端取行 全部符合预期
  PASS  shell-vars.sh    PASS  变量表：set/unset/展开/表满拒绝/按 owner 隔离/端到端 全部符合预期
  PASS  shell-console.sh PASS  双页控制台：CRTC 切页、横幅、命令回显与输出、证据页零改动
  PASS  ring3-io.sh      PASS  ring3 端口探测：I/O 位图拦下 in、#GP 收回内核、自检走完
  PASS  shell-irq.sh     PASS  每向量计数：打点/读取/渲染三段一致，tick=0x000204 pf=0x000008 kbd=0x000008
  PASS  shell-tasks.sh   PASS  tasks：slot 0 的 CPU tick 在涨（31 -> 229），且与屏幕 0x000083 对得上；5 行槽位可见
  PASS  serial.sh        PASS  串口：26 行启动日志按序落到文件，崩溃前最后一行也在
  PASS  klog.sh          PASS  日志环：内存 26 条，屏幕 8 行带 tick 前缀，启动那 24 行滚掉也还在
  PASS  heap.sh          PASS  内核堆：懒要页（0 -> 1）、图案校验全过、全部归还后账回 0
  PASS  disk.sh          PASS  ATA PIO：认盘(0x800 扇区) + 读 LBA0/LBA1 + 内存与文件逐位一致
  PASS  fs.sh            PASS  mosfs：挂载/列目录/读短文件/跨块折行/二进制点号/错误分支 全部符合预期
  PASS  exec.sh          PASS  exec：盘上的程序在 ring3 跑完（magic 自证、val=36、write 出声、exit 36）、
  PASS  elf.sh           PASS  ELF 装载：e_entry 起跑（ep=0x0060）/ 指纹与结果自证 / 裸二进制老路径不变
  PASS  pg.sh            PASS  exec 多页：跨页 ELF 要到连续两页（pg=2 / val=1834）、页全归还、单页与超限路径不变
  PASS  fs2.sh           PASS  mosfs 写路径：新建/追加/跨块 + 关掉 QEMU 后从盘字节读回来 全部符合预期
  PASS  fs3.sh           PASS  mosfs 删除：目录项/inode/位图三处都改了，重启后也没复活
  PASS  fs4.sh           PASS  mosfs 间接块：3000 字节读得全，满 4 块之后写第 5 块自动新建间接块
  PASS  userpg.sh        PASS  每任务 esp0：ring3 落在自己的任务页（0x26000）、空闲回默认栈、页全归还
-------------------------------------------------
合计：PASS 19 / FAIL 0（共 19 个脚本）
```

失败时只回打该脚本里的 `FAIL` 行（最多 5 条），不把整段输出灌出来。

## COM1 串口：让内核有一条脱屏的调试通道

在此之前，所有证据都得从显存里"抓屏 + 解码"：`pmemsave 0xb9000 4000` 再跑
`tools/vga-screen.py`。这条路线一怕滚动（启动日志在 24 行页上必然丢头部），
二怕崩溃（屏幕上只剩最后一行）。串口把同一份 `printk` 直接落到文件，`-serial file:`
之后崩溃前的每一行都还在。

| 文件 | 内容 |
|---|---|
| `include/linux/serial.h` | `SERIAL_COM1 0x3F8` 与 5 个寄存器偏移（`THR/RBR`、`IER`、`FCR`、`LCR`、`MCR`、`LSR`） |
| `kernel/chr_drv/serial.c` | `serial_init()`：关中断 → DLAB=1 写分频 1（115200）→ 8N1 → FIFO `0xC7` → MCR `0x0B`；`serial_putc()`：先查 LSR bit5（THR 空），自旋上限 200000 次；`serial_ready()/serial_puts()` |

自旋上限是刻意的：**设备不在就丢字，不卡死内核**。`serial_init()` 在
`init/main.c` 里排在 `sched_tune_gate_init()` 之后、`vga_clear()` 之前——必须早于
第一次 `printk`，否则启动日志的第一行就漏在串口外面。

`tools/serial.sh` 是它的验收：`-serial file:/tmp/serial.log`，等到 `kernel_idle`
后断言首行横幅、`[idt] 00 div`、`[mem] demand base=0x02000000`、
末尾 `unhandled`，以及总行数 ≥20（实测 24）。

## `%t` 与日志环：把 24 行证据串成时间线

`printk` 原来三件事只做了一件（写 VGA）。现在 `kernel/printk.c` 里只有一个出口
`emit(c)`，每个字符同时喂给三处：

```
emit(c) = vga_putc(c) + serial_putc(c) + klog_putc(c)
```

于是"屏幕上有的、串口里有的、环里存的"天然是同一份字节，不会出现三处各写一套、
互相漂移。`%t` 是新增的格式符：打 `t=0x` + 6 位十六进制 `timer_ticks()`，
**不吃参数、不带尾随空格**（`printk("%t[mem] ...")` 紧贴着正文）。

日志环 `include/linux/klog.h` + `kernel/klog.c`：

| 常量/接口 | 语义 |
|---|---|
| `KLOG_LINES 32` / `KLOG_COLS 67` | 67 列是因为 shell 的 `log` 前缀 `"t=0xNNNNNN "` 占 11 列，合计 78 ≤ 79 |
| `klog_putc(c)` | 按 `'\n'` 切行；行首（`pend_len==0`）取 `timer_ticks()` 存时间戳；超宽丢字符计 `dropped`（**不拆行**）；环满覆盖计 `overwrote` |
| `klog_get(idx, &tick)` | `idx 0` 是**最旧**的一条 |
| `k_count/k_dropped/k_overwrote` | 三条账，shell `log` 与测试都读它 |

已知的账：实测 `k_count=25 dropped=42 overwrote=0`（加了 ATA 之后多一行）。
那 42 个丢字符是第 23 行（`[mem] PF 0x04000000 … unhandled`）超 67 列被截掉的
尾巴——不是 bug，是预算。

`tools/klog.sh` 的写法值得记一笔，因为它踩过一次坑：`log` 打二十几行，而用户页
只有 24 行，**最早的 `[cpu]` 必然滚出屏幕**。所以断言拆成两半：

- **屏幕侧**证明"渲染对"：至少 5 行带 `t=0xNNNNNN` 前缀，含 `[mem]` 与 `unhandled`；
- **内存侧**证明"全量还在"：`sym k_text` 取基址，`xp /67bx` 依次读前 3 格
  （每格 67 字节，偏移用 `printf '%x'` 现算），把字节拼回字符串，断言其中一条是 `[cpu]`。

**为什么是"前 3 格"而不是"第 2 格"**：启动日志的条数会随驱动增加而变（加 ATA 之后
`[cpu]` 就从第 2 条挪到了第 3 条），写死序号等于给未来埋一个必然要改的断言。
这也是后来所有"读静态数组"的脚本照抄的模板
（偏移必须 `printf '%x'` 现算，不能写 `0x$(( ))`——那是十进制拼十六进制）。

## 内核堆：`kalloc` / `kfree`

文件系统、块设备、`exec` 三件事的共同前置。`include/linux/heap.h` + `mm/heap.c`，
块头 8 字节（`size` + `magic`），对齐 8、最小 16。

关键决定是**页是懒要的**：`heap_init()` 只清账、一页不占。理由是 24 行自检屏里
`frames=0x00000005/0x00000070` 这个数字必须保持不变，堆不能一开始就动分配器。

| 机制 | 做法 |
|---|---|
| 页来源 | `HEAP_CHUNKS 8` 个页块从低往高 bump，页不够就 `page_alloc()`；**懒要**，`heap_init()` 不占页 |
| 空闲表 | `HEAP_FREE_SLOTS 64` 条，first-fit；够大就切余料，余料 ≥16 才切 |
| 归还 | 整页**永不**还回 `page_alloc`（页是稀缺资源，留着复用）；不做相邻块合并（文档写明，代价是碎片，收益是代码短一半） |
| 校验 | `HEAP_MAGIC 0x48454150` / `HEAP_FREE_MAGIC 0x46524545`；`kfree` 校验 magic，拒野指针与 double-free；空闲表满计 `leaks` |
| 自检 | `heap_selftest(n, size)` n≤16，写 `0xA5 ^ (i*31 + j)` 图案后逐字节校验再全还，返回失败字节数 |

shell 三条命令：`log`（环里每行，带 `t=` 前缀）、`heap`
（`heap p=<页数> u=0x<6位> l=<live>`）、`kstress`
（`kstress 8x100 bad=0x<6位> u=0x<6位> l=<live>`）。`cmds[]` 8 → 11 条。

`tools/heap.sh` 的判据是"前后两次 `heap` 的差"：

```
  A. 未用堆之前：pages_taken=0（应为 0，初始化不占页）
  heap（前）: heap p=0 u=0x000000 l=0
  kstress: kstress 8x100 bad=0x000000 u=0x000000 l=0
  heap（后）: heap p=1 u=0x000000 l=0
  B. 内存侧：pages_taken=1 used_bytes=0 fails=0 leaks=0
PASS  内核堆：懒要页（0 -> 1）、图案校验全过、全部归还后账回 0
```

8×100 字节的块（8×108 = 864 < 4096）刚好一页——这行 `0 -> 1` 就是"懒要"的证据：
初始化不动页，第一次 `kalloc` 才向分配器要。再加上 `used_bytes==0` 与
`leaks==0`，说明全还回去了且没漏空闲表。

## ATA PIO：把磁盘上的字节原样拿到内存

`include/linux/ata.h` + `kernel/blk_drv/ata.c`，主 IDE 通道（`0x1F0`）两个盘，**只做读**
（LBA28，一次一个扇区）。不实现写是有意的：这一层只需要回答一个问题——内核能不能
把盘上的字节读进内存；写要缓存、坏块、断电一致性，那是文件系统的事，提前出现的
写接口只会让人误以为它可靠。

| 端口（相对 `0x1F0`） | 用途 | 这一层踩到的点 |
|---|---|---|
| 0 | 数据（16 位） | 一个扇区 = 256 次 `inw`，顺序不能乱 |
| 2 / 3 / 4 / 5 | 扇区数 / LBA 低 / 中 / 高 | 只读一个，所以 `NSECT=1` |
| 6 | 驱动器/头 | `0xE0` 是 LBA28 模式，bit4 选盘，低 4 位是 LBA 的 bit24..27 |
| 7 | 状态（读）/ 命令（写） | `0x20` 读、`0xEC` IDENTIFY |
| `0x3F6` | 备用状态 | 读它不改变中断，正好当 400ns 延时（连读四次） |

两个容易写反的等待：**发命令后等 BSY 落下**（顺带看 ERR/DRDY），
**真正搬数据前等 DRQ 置起**。方向相反，混起来就是读到全 0 或挂死。
两处都带自旋上限 `ATA_SPIN=200000`——盘不在时宁可返回 `-2`，也不能把内核停在 `hlt`。

认盘靠两个"一看就判死"的信号：选盘后状态为 `0x00`（无设备）或 `0xFF`（浮空总线）
直接返回不存在；IDENTIFY 之后如果 LBA 中/低字节非零，说明是 ATAPI（光驱）而不是
ATA 盘，同样不当盘用。总扇区数取 IDENTIFY 的 word 60/61。

两个实现细节值得单独记：

- **`sec_buf` 是一块内存两个名字**（`union { unsigned short words[256]; unsigned char bytes[512]; }`）。
  第一版写成了两个数组 + "别名"，那是两个不同的地址——`inw` 写进 A、调用方从 B 读，
  测试会读到全零而看起来像"驱动坏了"。用 union 把"同一块内存"这件事写进类型里。
- **`printk_dev()`**：启动时留一行 `[ata] m=0 s=1 sec=0x000800`，只进串口和日志环、
  **不进显存**——24 行自检屏是按行算死的。它内部就是 `emit_screen = 0` 的开关，
  `emit()` 的屏幕那条腿被短路，串口与环照常收字节。
  这行日志刻意放在 `rep_banner()` **之后**：串口的第一行必须是横幅，
  "我起来了"不该被任何设备结论抢到前面（`serial.sh` 的第一行断言就是这么来的）。

shell `disk [lba]`（默认 LBA 0，十进制参数），四行输出各管一件事：

```
row  2| ata m=0x0 s=0x1 sec=0x000800
row  3| drv=1 lba=0x000000 rc=0 rd=1 er=0
row  4| d0: 4D 49 4E 49 2D 4F 53 20
row  5| sig=0xAA55
```

`m`/`s` 是主/从盘的存在位，`sec` 是 IDENTIFY 报的总扇区数，`rc` 是这次读的返回码
（负数打成 `E1..E3`，因为最小格式化器里没有 `%d` 的负号路径），`rd`/`er` 是累计账；
`d0:` 是扇区前 8 字节；`sig` 是 510/511 两个字节按小端拼成的字值 = `0xAA55`
（同一件事两种写法：字节序列 `55 AA`、字值 `AA55`，这里按后者打，因为"小端机器上
读出来是多少"是不需要翻译的事实）。

`tools/disk.sh` 造一张**真盘**（1MB raw，第 0 扇区写图案 + 尾签名，第 1 扇区写哨兵串），
挂成 IDE 从盘，判据三层：

1. **认得盘**：`ata m=0x0 s=0x1 sec=0x000800`（2048 个扇区，来自 IDENTIFY）；
2. **读得回**：`rc=0`、屏上 `d0:` 与图案一致、`sig=0xAA55`；
3. **内存里也对**：从符号 `sec_buf` 读回 16 字节，与镜像文件前 16 字节逐位相等；
   再 `disk 1` 一次，读回的字节应等于文件的第 512 字节起——这条证明 LBA 参数
   真的走到了盘上，而不是永远读第 0 个扇区。

屏幕只能证明"渲染对"，内存与文件比对才能证明"内核真读到了"——这是从 `klog.sh`
沿用下来的同一条规矩。

## mosfs v1：盘上的东西有名字了

`include/linux/mosfs.h` + `fs/mosfs.c`，一个自造的最小文件系统（v1 先能读，写路径见下一节）。自造而不直接读 FAT12 的理由
写在头文件开头：FAT12 要先把 BPB 的二十多个字段、簇大小、FAT 链、定长根目录区、
坏簇与尾簇语义全部实现对，才能读出一个文件；而这一层要的只是"名字 → 块号"。

盘上布局（LBA 0 留给图案/MBR，文件系统从 1 开始）：

| LBA | 内容 |
|---|---|
| 1 | 超级块：`magic "MFS1"`、块大小 512、总块数、inode 数、根 ino、inode 表起址、每块 inode 数、位图起址、数据区起址、数据块数 |
| 2..3 | inode 表，每个 32 字节（一块 16 个）：`mode`(1 文件/2 目录)、`links`、`size`、4 个直接块 + 1 个间接块 |
| 4 | 位图：`bit i` = 块号 `i` 被占用（写路径从这里要块） |
| 5.. | 数据区 |

目录项 16 字节（名字 12 + ino 4）。**目录项里不存 size**——size 在 inode 里，
这正是"名字只是一个名字→号的表"这件事的直接体现。

动作少是这一层的目的：读超级块、读一个 inode、读一段数据三件事就能读完一个文件。目录遍历不是第四个
动作，它是"读根目录 inode 的数据块再按下标切片"。刻意不做的：二级间接块、多级目录、
时间戳、权限、硬链接——每一条都会让"读一个文件"多一个必须同时正确的地方。
（一级间接块后来补上了，见「mosfs 间接块」那一节。）

`fs_mount()` 的校验链（失败码 `-1..-10`，全部进串口的 `[fs] rc=`）：
magic 逐字节比 → 块大小 = 512 → `total ≤ IDENTIFY 报的扇区数` → `data_start + data_blocks ≤ total`
→ inode 数 ≤ 64（`FS_V1_MAX_INO`）→ `bitmap_start` 必须非 0 且落在数据区之前（`-10`，写路径要用它）
→ 根目录能装进 `dent[]`。

**两个实现上的坑，都是实测踩出来的：**

1. **`cdecl` 下参数从右往左求值**。第一版在 `init/main.c` 里写
   `printk_dev("[fs] rc=%d b=%d i=%d\n", fs_mount(), fs_blocks(), fs_inodes())`，
   串口打出来是 `[fs] rc=0 b=0 i=0`——`fs_blocks()` 在 `fs_mount()` 之前就被求值了。
   改成先 `int fs_rc = fs_mount();` 再打，立刻正确。这类错误不会崩，只会让证据说谎。
2. **一块缓存会冲刷**。最初每次读块都过一遍 1 块缓存（键是 LBA），而挂载后
   `ls`/`lookup` 在 inode 表块（LBA 2）与目录块（LBA 5）之间来回跳，缓存互相冲掉，
   实测 `rd=31`。改成**挂载时把根目录整张表读进 `dent[8]`**（名字 + 号 + 大小），
   `ls`/`cat` 的查找从此是内存比较，读块数落到 `rd=12`。

shell 三条命令：`fs`（`ok=0x1 b=2048 i=32 rd=12 er=0`）、`ls`（名字 20 列 + 十进制大小）、
`cat <名字>`（按块读、按 `\n` 断行、超 79 列硬折，不可打印字节打成 `.`）。

`tools/datadisk.py` 造盘（纯字节操作，产物就是 `/tmp/fs.img`），里面写三个文件：

- `hello.txt` 52 字节 / 3 行；`long.txt` 169 字节（跨 2 块，第一行 78 个 A、第二行 90 个
  数字——正好验证"78 不折、79 折"）；`blob.bin` 20 字节全不可打印。

`tools/fs.sh` 的判据四层：挂载（`[fs] rc=0 b=2048 i=32`）、列目录（名字 + **大小来自 inode**，
`52/169/20`）、读回（`cat` 三行逐字等于源文件、78 个 A 的行长度实测 78、90 位数字被折成
79+11 两行）、边界（`cat nope.txt` → `cat: no such file`、`blob.bin` → 20 个点号、
`rd` 在 1..16 之间说明缓存真的起了作用）。

## mosfs 写路径：`create` / `write`

只读那一层跑通之后，写只加了三件东西：**一块能写下去的 ATA 命令、一张位图、一个把
字节摆回 inode 的函数**。`fs_create(name, &ino)` + `fs_write(ino, buf, len)`（v1 的写只有追加，
不改已有字节），shell 上是一条 `write <名字> <文本...>`。

**落盘顺序是"数据块 → inode → 位图"**，与教科书里的"先记账后写数据"相反。理由很实际：
这一层没有一致性恢复（没有 fsck、没有日志），断电只可能留下两种残局——"块里已有数据但
inode/位图没记"或"记了块号但块里还是旧的"。前一种只是浪费一块空间，后一种会让
`cat` 读出别的文件的字节、看起来像文件损坏。选前者。

`fs_write` 的路径（`size` 是文件当前长度，追加从 `size` 起）：

1. 先算落点：`bi = size / 512`、`off = size % 512`，逻辑块号上限 `FS_MAX_BLOCKS = 132`
   （> 67584 字节返回 `-3`），超过第 4 块时由 `blk_ensure` 从间接块里取块号；
2. `blocks[bi] == 0` 时向位图要一块（`bitmap_alloc`：从 `data_start` 扫第一个 0 位，置位后
   **立刻把位图块写回盘**，这样即便后面数据写失败，这块也只是"登记了但没用"而不是被二次分配）；
3. **新块第一次写也要先 `blk_read`** —— 缓存里那一块可能是别的块的内容，直接改再写会把
   同块里已有的字节抹掉；
4. 逐块拷、逐块 `blk_write`（`ata_write` 写 256 个字，写完查 `ERR/DF`）；
5. 最后 `inode_write` 把 `size` 与块号表写回，并同步内存里 `dent[]` 的 size——否则紧接着
   的 `ls` 显示的还是旧大小。

`fs_create` 是把"根目录 16 字节表项"追加到根目录数据块的尾巴：`inode_alloc` 找一个
`mode == 0` 的 inode 占位 → 写名字（补 NUL 到 12）与 ino → 根 inode 的 size +16。名字超过
`FS_NAME_MAX`（12）直接拒（`-4`），不截断——截断之后 `fs_lookup` 就再也找不到它。

`tools/datadisk.py` 也补了一步：**造盘时把已经用掉的块在位图里置位**。位图原先全 0，
内核的 `bitmap_alloc` 会把根目录那块当空闲分出去，第一次 `write` 就踩坏文件系统。

`tools/fs2.sh` 的判据分三层，最后一层是这轮的关键：

1. 接口层：`write hi abcdef` → `rc=0 sz=7`；再 `write hi ghi` → `sz=11`；`ls` 的大小跟着变；
2. 内容层：`cat hi` 先是一行 `abcdef`，再是 `abcdef` + `ghi` 两行；
3. **落盘层**：kill QEMU 之后在宿主机上直接解析 `/tmp/fs2.img` —— 目录项里有 `hi`、
   inode 的 `size=11`、数据块里逐字节是 `b"abcdef\nghi\n"`、位图里那一块的 bit 是 1。
   `big` 写 11 次 × 48 字节 = 528 字节，`blocks=[11,12]` 两块都置位。

为什么第 3 层非做不可：`cat` 读的可能是内核那一块 `blk` 缓存，内存里对、盘上没写，
屏幕照样正确。**只有关掉机器再从盘上读，才叫"写进去了"**。

实测输出（`fs2.sh` 全绿）：

```
write hi rc=0 i=6 sz=7 w=6 n=1
write hi rc=0 i=6 sz=11 w=2 n=0
fs ok=0x1 b=2048 i=32 rd=47 er=0 wr=36 cr=2
hi ino=6 mode=1 size=11 blocks=[10] bits=[True] content=b'abcdef\nghi\n' match=True
big ino=7 mode=1 size=528 blocks=[11, 12] bits=[True, True]
```

顺带记一个 shell 的既有约束：单个参数上限 `SHELL_ARG_MAX = 24`（实际 23 字符），超了**静默截断**。
所以上面的 `big` 用两个 23 位数字串拼成 48 字节一行，而不是一个长串——测试自己按这个上限
构造输入，才不至于测出"少写了字节"的假象。（这一轮已经把静默截断改成整行拒绝：
屏上打 `arg too long (max 23)`，`fs2.sh` 里有一条断言守着它。）

## mosfs 删除：`rm`

写通了之后的自然一问是"能不能删"。`fs_unlink(name)` + shell 的 `rm <名字>`，
三步顺序**与写路径相反**：位图清位 → inode 归零 → 目录项抽掉（后面的项整体前移 16 字节、
末项清 0，根 inode 的 size 减 16）。

为什么反过来：位图是"谁在用这块"的账。让它先归零，最坏残局是"目录里还剩一个指向空
inode 的名字"（`ls` 显示 0 字节、`cat` 说没有）——看得见、能修；反过来则是
"名字没了、块却永远占着"——看不见，只能扫盘。删除**不擦数据块的内容**，
只把块还回去，这是有意的（擦一遍要多写几个扇区，而且对"这盘还能不能用"没有帮助）。

`tools/fs3.sh` 的判据三层，重点是第三层：

1. 接口：`rm hi` → `rc=0 n=6->5`；`rm nope.txt` → `rc=E1`（目录项数不变）；`cat hi` → `no such file`；
2. 内存视图：`ls` 里不再有 `hi`；
3. 落盘：造盘后先留一份 `fs3.img.before`，跑完再和 `fs3.img` 比——
   **位图逐字节相同**（块真的还回去了）、根目录 size 不变、目录项集合与造盘时一致、
   `hi`/`big` 的 inode 回到 `mode=0 size=0 blocks=0`；
   最后**重启一次机器**再 `ls`：删掉的文件没有复活（内存缓存清零后重新从盘上读）。

实测：

```
write hi rc=0 i=7 sz=7 w=6 n=1
rm hi rc=0 n=6->5
rm nope.txt rc=E1 n=5->5
fs ok=0x1 b=2048 i=32 rd=51 er=0 wr=42 cr=2 un=2
位图：删完之后与造盘时逐字节相同（块全部还回去了）
inode 7: 造盘时不存在，现在 mode=0 size=0 blocks=[0, 0, 0, 0]
DISK OK
```

一个测试写法上的坑：`grep -E '| hi '` 里的 `|` 是**正则的或**，左边空意味着"匹配任何行"，
于是"ls 里还有 hi"永远成立。要么写 `grep -F '| hi '`，要么把 `|` 转义成 `\|`。

## exec 的第二种格式：ELF32

`include/linux/elf.h` + `fs/elf.c`，一个"只认一段装得进一页"的 ELF32 解析器。加它不是为了
支持什么高级特性，而是为了把"程序从页头开始跑"这条隐含约定变成一个**读文件头得到的
明确数**：`e_entry`。

`exec_run` 现在先问 `elf_probe`，认得出就走 ELF 路径，认不出就仍当裸二进制（老路径一个
字节都没改，`ep=0x0000` 就是它）：

- `elf_probe`：`\x7FELF` + `ELFCLASS32` + `ELFDATA2LSB` + `ET_EXEC` + `EM_386` +
  `e_phoff/e_phnum` 非 0 + `e_phentsize == 32`；任一条不满足就当"不是 ELF"，
  不是"坏 ELF"——**格式分派不该在头部半对半错时抛错**；
- `elf_load`：遍历程序头，只处理 `PT_LOAD`；每个段要求 `vaddr + memsz ≤ 一页`、
  `filesz ≤ memsz`、`p_offset + filesz ≤ 文件长度`；`filesz` 之后的 `memsz - filesz`
  清 0（`.bss`）；返回的 `e_entry` 是**页内偏移**，`exec.c` 自己加页基址再进 ring3；
- 失败码 `-1..-7` 都在头文件里，`exec_run` 把它折成自己的 `-6`（`rc=E6`）。

两个坑，都是这轮踩出来的：

1. **`e_entry` 是偏移，不是地址**。第一版直接把 `elf_load` 返回的 `0x60` 交给
   `user_enter`，ring3 从物理 `0x60` 取指——不是"程序跑错"，是整个落到内核区，
   表现为 `rc=E5`（没有指纹）、`sy=0/0`（一次门都没过）、`ex=-`（没调 exit）。
   加一句 `entry += code` 就好；`ep=0x0060` 这个证据也是为此加的（它会直接暴露这类错）。
2. **默认 `ld` 让文件头尾重**。`ld -Ttext 0` 会把 `.text` 放到文件偏移 `0x1000`，
   拼出一个 4.6KB 的 ELF，而 `exec` 当时只给一页（如今是 4 页，但这条依然是
   整个文件都被抬高的浪费）。改用 `tools/uprog.ld`：`FILEHDR PHDRS` 让第一个
   `PT_LOAD` 从文件第 0 字节开始，代码从 `SIZEOF_HEADERS` 起，产物 **596 字节**、
   `p_offset == p_vaddr == 0`——这也正好满足 `elf.c` 那条"原地搬迁只有 offset == vaddr 才安全"。

`tools/elf.sh` 三条判据：宿主机上直接从 `/tmp/elf.img` 里取出 `uprogel` 的 `e_entry` 与
`PT_LOAD` 范围；`exec uprogel` 的 `ep=` 必须等于那个 `e_entry`、`rc=0`、指纹与 `val=36`
都在；最后再跑一遍裸二进制 `uprog`，`ep` 必须是 `0x0000`——**证明新格式是加分，不是替换**。

实测：

```
A. ELF: e_entry=0x60 PT_LOAD offset=0 vaddr=0 filesz=239 memsz=239
B. exec uprogel rc=0 mag=0x45584543 val=36 ex=36 sy=2/0 t=1 d=0 n=1/0 ep=0x0060
C. exec uprog rc=0 mag=0x45584543 val=36 ex=36 sy=2/0 t=1 d=0 n=2/0 ep=0x0000
```

## mosfs 间接块：单文件从 2KB 到 66KB

`fs3.sh` 全绿之后，这一层最刺眼的限制不是多级目录，而是**单文件 2048 字节**：
inode 只有 4 个块号槽，`fs_write` 到第 2049 字节就返回 `-3`。四条边都够用了
（读、写、删、ELF 装载），只有上限这一条会天天撞到——`exec` 读程序、以后放别的东西都会撞。

解法选了最小的一种：**inode 第 5 个槽当间接块**，一块 512 字节装 128 个块号。
`FS_INODE_BLOCKS = 5`、`FS_IND_PER_BLK = 128`、`FS_MAX_BLOCKS = 132`、
`FS_MAX_BYTES = 4*512 + 128*512 = 67584`。二级间接不做——那要再来一块指"间接块的间接块"，
而 66KB 已经够这一层所有的测试写满一个文件。

`inode_read/inode_write` 从"4 个写死的 `u32_at(off+8+4*j)`"变成 `FS_INODE_BLOCKS` 的循环，
然后所有"按逻辑块号找物理块"的地方收敛成两个函数：

- `blk_lookup(inode, bi)`：`bi < 4` 直接返回 `blocks[bi]`，否则读 `blocks[4]` 那一块的
  第 `bi-4` 项。**空洞即停**——`fs_read` 遇到返回 0 就结束，因为追加式写不会在中间留洞。
- `blk_ensure(inode, bi, &blk)`：缺块就 `bitmap_alloc` 要一块；`bi >= 4` 且 `blocks[4] == 0`
  时先要一块当间接块，**整块清 0**（不清的话那 128 个"垃圾块号"会被当成真块读）。

两个必须记住的坑，都是这一轮实测踩出来的：

1. **`bitmap_alloc` 会冲掉那块 1 块缓存**（键是 LBA）。所以"要一块 → 读间接块 → 改第 k 项"
   这个顺序必须写成"要块 → 重新 `blk_read` 间接块 → 改 → 写回"，否则读到的是位图那块的内容，
   写回去就把间接表写成了位图。
2. **新建的间接块不清 0 等于给文件系统埋雷**。`bitmap_alloc` 只写位图，不擦数据；
   盘上那块可能是之前删掉的文件留下的旧块号，`fs_read` 会照着它去读别的文件。
   清 0 是唯一便宜且必须的一步。

`fs_unlink` 跟着变长：原来是"4 个直接块逐个 `bitmap_clear`"，现在还要
「读间接块 → 对 128 项里非 0 的逐个 `bitmap_clear` → 清间接块自身」。这里有个有意的取舍：
每次清一位前后都要重读间接块（`bitmap_clear` 同样换缓存），一个满 132 块的文件删除
会做 128 次重读 + 128 次位图读写。也可以"先把 512 字节间接表搬到栈上再一次清"，
但那要 128 个字压在 4KB 内核栈上，而删除是低频操作——**慢一点可以，栈溢出不行**。

`tools/datadisk.py` 加了第 5 个位置参数 `big`（只有等于 `big` 才开），开的时候多造两个文件：

- `big3k` 3000 字节 / 6 块 → 造盘时就有一个间接块，专测**读**；
- `full2k` 正好 2048 字节 / 4 个直接块满 → 专测**写第 5 块时新建间接块**。

默认不开，是为了不撑爆根目录的 8 项上限（`FS_V1_MAX_FILES`）——现有 `fs2/fs3`
的 create 步还要往根目录里加项。

`tools/fs4.sh` 的判据四层：

1. 读：`cat big3k` 屏上要出现 `L059`（第 60 行、第 2959 字节起）。24 行屏只看得到尾部，
   所以"最后一行在不在"就等于"2048 之后的内容读出来了"，而那段块号只能来自间接块；
2. 写：`write full2k z` → `sz=2050`、`ls` 里 `full2k 2050`、`big3k 3000`；
3. 删：`rm big3k`（6 块里有 1 块是间接块）→ `n=6->5`，`ls` 里只剩 5 项，
   **造盘时那 6 块的位图 bit 必须全清**——间接块本身也是要还的块，这一条最容易漏；
4. 落盘：关掉 QEMU 后在宿主机上直接解析 `/tmp/fs4.img`——`full2k` 的 `blocks[4]=21`、
   间接块第 0 项指向数据块 22、那块开头是 `b'z\n'`、两个新块的位图 bit 都是 1、
   前 2048 字节仍是 2048 个 `A`；最后一条是**位图总账**：置位的 bit 数必须正好等于
   "根目录 + 剩下 5 个文件"占的块数（多一个就是 `rm` 漏还了块）。

实测：

```
full2k: mode=1 size=2050 blocks=[16, 17, 18, 19, 21]
full2k 间接块=21，第 0 项 -> 数据块 22
  那个数据块开头 = b'z\n\x00\x00\x00\x00\x00\x00'（应为 b'z\n'）
big3k 的 6 块（含间接块 15）位图 bit 全清
位图置位数=11，按根目录+文件算出来=11
DISK OK
PASS  mosfs 间接块：3000 字节读得全，满 4 块之后写第 5 块自动新建间接块
```

那条位图总账第一次跑就报了"悬空 1 位"，查下来是**根目录自己占的那个数据块**
没算进去（`names` 里只有文件，没有 ino 1）——判据写错会冤枉代码，这一层的教训是
"先把预期用同一套 inode 读法算出来，再跟位图比"。

kernel 从 75760 涨到 **76304 字节**，`__kernel_end=0x000175A0`，分配区余量 35424。
`check-all.sh` 现在 17 项，**PASS 17 / FAIL 0**。

顺带记一条测试写法的教训（`fs3.sh` 里踩过、`fs4.sh` 差点再踩）：
`grep -E '| hi '` 里的 `|` 是**正则的或**，左边空 = 匹配任何行，断言永远成立。
要么 `grep -F '| hi '`，要么转义成 `\| hi '`。

## exec：把盘上的字节跑到 ring3

前面三件事合起来才成了这一件：`ata_read` 拿到扇区、`fs_read` 按名字拿到字节、`ring3 +
用户页租借 + GDT 调用门` 让一段用户态代码能出去再回来。`include/linux/exec.h` + `fs/exec.c`
把它们串成一个动作：**`exec_run(name, &magic, &value)`**。

它做的事，顺序不能乱：

1. `fs_lookup(name)` 找 inode，拒绝大于一页的程序（`-2`）；
2. `page_alloc()` 要两页——一页放代码、一页当用户栈；`fs_read()` 把字节拷进代码页；
3. **最后**才 `page_lease_open(KERNEL, 页, 500)` 放行 U/S：先要页、再拷、最后放行，
   反过来就会出现"已经对用户态开放、内容还是半个空页"的窗口；
4. `tune_gate_user_window(esp0-16, esp0)` 只把 `{field, value}` 这一个结构体开给门解引用；
5. 先把结果区清成 `0xDEAD0000`（这是个哨兵：程序没写指纹的话，这里一定还是它）；
6. `user_enter(code, esp0)` iret 进 ring3，接着 `cpu_sti()`（ring3 期间 IF=0 见
   `user_enter`），回来立刻 `tune_gate_user_window(0, 0)` 把窗口关死；
7. 读回 magic 与 value，关租约、`page_free` 两页，magic ≠ `0x45584543` 就返回 `-5`。

（第 6、7 步现在不是 exec 自己做的了：它把这两页交给一个新任务，任务里下潜、
任务死后由 dtor 收回——见「从"exec 自己下潜"到"交给一个任务下潜"」那一小节。
下面这段讲的是**协议本身**，与谁下潜无关。）

为什么要那个指纹。`exec` 跑完"没崩"什么都证明不了：`iret` 进去落在一个空页上，
第一个字节是 `add [eax],al` 之类，也可能碰巧不崩、然后被 `#PF` 劫持回内核。
所以判据必须来自**程序自己写下的东西**：`boot/uprog.asm` 里两句
`mov dword [esp-24], 0x45584543` 与把 1..8 累加成 36 的循环，都是位置无关的裸指令，
它不认识内核、内核也不认识它。结果区先被写成哨兵，程序若没跑起来（或跑歪了），
读回来的就是 `0xDEAD0000`，`-5` 立刻报出来。

程序怎么回内核。用户页上没有 `int` 可用（IDT 是全内核的、门是 DPL=0），
所以出口还是那条已经验过的 GDT 调用门：`[esp-16] = 0xFF`（`TUNE_REQ_EXIT`）、
`lea eax,[esp-16]; call 0x18:0`，门的处理函数把中断帧改成
`(eip=user_return_stub, cs=0x08)`，`iret` 就把 CPU 送回内核态。**同一条门**
既当参数入口又当退出通道，这是"先跑通"的取舍——真要做系统调用，
得按号分派（现在 `field` 只有一个 0xFF 有意义）。

实测（`tools/exec.sh`，第一次跑就通了功能，只错在显示宽度）：

```
  A. 跑之前 pages_taken=0
  B. exec uprog rc=0 mag=0x45584543 val=36 n=1/0
  C. 跑之后 pages_taken=0（应回到 0）
  D. 错误分支：exec nope rc=E1
PASS  exec：盘上的程序在 ring3 跑完（magic 自证、val=36）、两页收干净、错误分支不崩
```

`mag=0x45584543` 是程序写的、`val=36` 是程序算的、`n=1/0` 是"跑了 1 次、失败 0 次"、
`pages_taken` 跑前跑后都是 0 是"两页都还了"、`exec nope rc=E1` 是"找不到文件"。

**一个显示上的坑**：第一次跑出来是 `mag=0x584543`，看着像 magic 错了，其实是
`hex6()` 只输出 6 位十六进制——`0x45584543` 被截成了低 24 位。修法是加 `hex8()`
（`"0x"` + 8 位），`cmd_exec` 改用它，`exec.sh` 的正则从 `{6}` 跟着改 `{8}`。
这类错误不会崩、不会报，只会让证据说谎，和 mosfs 那次 `cdecl` 求值顺序是同一类。

`boot/uprog.asm` 是 `nasm -f bin` 的纯二进制，`Makefile` 里单独一条规则产出
`build/uprog.bin`，`tools/datadisk.py` 收第二个参数把它挂成盘上第 4 个文件
（`files=4`）；不给参数时写入 `0xF4`（`hlt`）占位，那种盘上跑 `exec` 必然走 `-5`
——正好是"指纹不匹配"这条负路径的现成素材。

## exec 多页：`page_alloc_run` 与一个跨页的 ELF

间接块把文件上限抬到 66KB 之后，卡住"程序能多大"的不再是文件系统，而是
`EXEC_MAX_BYTES = 一页`。这一轮把 exec 从"一页"改成"一串连续页"，上限 **16KB（4 页）**。

**为什么必须是"连续"**：这套内核只有恒等映射，虚拟地址就是物理地址。一段跨页的程序
在 `vaddr = 0x1800` 处取指，映射到的是 `code_base + 0x1800`；只有物理页也紧挨着，
这一串地址才是同一个能用区间。所以页分配器补了一个连续版：

- `page_alloc_run(k)`：从低地址往上找第一段 k 个连续空位，逐页清零，返回低地址那页；
  和 `page_alloc_pair` 一样**不动 `next_free`**（相邻性要求把低地址也看一遍）。
- `page_free_run(phys, k)`：就是 k 次 `page_free`，校验全交给它（范围/对齐/重复释放）。

**要几页在要页之前就得知道**，而 ELF 的 `p_memsz` 常常比文件本身大（`.bss`）。于是
`exec_run` 多了一步"先读一页临时缓冲"：

1. `page_alloc()` 一页临时缓冲，`fs_read` 只读前 `min(size, 4KB)`；
2. `elf_probe` 认得出 ELF 就调新加的 `elf_span`——所有 `PT_LOAD` 的
   `max(vaddr + memsz)`（返回 0 表示没一个能算的段）；
3. `need = max(文件长度, elf_span)`，`pages = ceil(need / 4KB)`；超过
   `EXEC_MAX_PAGES`（4）就按 `-2` 拒掉；
4. `page_alloc_run(pages)` + 一页栈 → **整段重读一次**（临时缓冲这时用完即还）→
   再 `elf_load`（`dst_bytes` 从"一页"变成 `pages × 4KB`，一个段可以跨页）→ 逐页放行租约。

`elf_load` 本身的逻辑没变（还是一段一段搬、`filesz` 之后清 0），变的只是上界。
程序头表必须落在文件第一页之内（`elf_span` 只在那一页上工作），否则按"装不进去"处理
——`ld` 产出的正常 ELF 全在文件最前面，这条限制实际碰不到。

**测试程序 `boot/uprog2.asm` + `tools/uprog2.ld`**：`.bss` 被硬摆到 `0x1800`，
`p_memsz = 0x1804`（实测 6148 字节），于是 `elf_span` 算出 2 页。程序做三件事：

- 用 `call .here / pop eax / sub eax, .here - uprog2` 算出 `code_base`
  （符号地址是从 0 起的 `vaddr`，运行时才知道基址）；
- 读 `[base+0x1800]` 必须是 0（`.bss` 的清零真的跨过了页边界），不是 0 就把结果写成
  `0xDEAD0000` 让内核判失败；
- 往 `[base+0x1800]` 写 `0x2A`、往 `[base+0x1FF0]`（第二页最后一个双字附近）写 `0x07`，
  读回来拼成 **1834 = 0x72A** —— 一页程序算不出这个数。

**证据多了一行**：`cmd_exec` 在页数 >1 时补打一行 `exec pg=2`（拼进原来那行会超 79 列），
`heap` 那行也补了 `pf=used/total`（物理页账），因为"要了几页、还不还"在这套屏上
没有别的地方看得见。

`tools/pg.sh` 的判据：

1. 格式：宿主机上确认 `uprog2` 的 `p_memsz = 0x1804`（> 一页），算出预期 2 页；
2. 要页：`exec uprog2 rc=0 mag=0x45584543 val=1834 ex=42`，屏幕上还要有
   `uprog2:pg2 ok` 与第二行 `exec pg=2`；
3. 老路径：`exec uprog`（裸二进制、一页）仍然 `rc=0 val=36 ep=0x0000`，
   且**不该**出现 `pg` 那行；
4. 上限与归还：盘上再放一个 20KB 的 `huge`，`exec huge` 必须 `rc=E2`（`-2` 超 16KB）；
   `heap` 的 `pf=` 在整轮前后必须一样（5/112 → 5/112）。

实测：

```
A. uprog2: e_entry=0x60 offset=0 vaddr=0 filesz=291 memsz=6148
B. heap 基线 pf=5/112
C. exec uprog2 → rc=0 mag=0x45584543 val=1834 ex=42 sy=2/0 t=1 d=0 n=1/0 ep=0x0060
   第二行   exec pg=2
D. exec uprog  rc=0 mag=0x45584543 val=36 ex=36 ... ep=0x0000
E. exec huge   rc=E2 mag=0x00000000 val=0 ex=- ... n=2/1
F. heap 收尾 pf=5/112
PASS  exec 多页：跨页 ELF 要到连续两页（pg=2 / val=1834）、页全归还、单页与超限路径不变
```

**踩到的坑**：第一版把 `exec pg=2` 藏在了 `job.pages` 里，而 dtor 收尾时正是把
`job.pages = 0`（它表达"页已经还了"）。程序明明跑对了（`val=1834` 只有两页才可能），
屏幕上却没有那行——于是页数另存一份静态量 `last_pages`，"这一趟要了几页"和
"现在还占着几页"变成两个数。

kernel 从 76304 涨到 **78000 字节**（`__kernel_end=0x17BC4`，分配区余量 33852），
`check-all.sh` 现在 18 项。

## 每任务一套用户页：TSS.esp0 跟着任务走

上一节之后还剩一个"全局单值"：`kernel/tss.c` 只有一张静态 `tss_stack`，
`tss_set_esp0()` 虽然存在，但**全项目没有一处调用**。于是不管谁在跑，
ring3 进门都落在同一张栈上——这既是"同时只能有一个用户行程"的根，
也让"这个任务用了多少内存"根本无从统计。

这一步把两样东西从"全局"挪到"任务"：

1. **进内核的那张栈**：`task_t` 里新增 `esp0_stack`（页物理地址）与 `esp0`（栈顶）。
   `task_create` 一次要三页——guard、栈、进内核栈；`schedule()` 在换人处
   （以及"全部收工回 main"那条路径）写 `tss_set_esp0(tasks[cur].esp0)`。
   slot 0（main）不占任务页，它的 `esp0` 就是 `tss_default_esp0()`
   （`&tss_stack[4096]`，在 `.bss`，地址 < `0x20000`）。
2. **用户页**：`ucode / upages / ustack` 三个字段挂在 `task_t` 上。
   exec 不再"自己收页"，而是在 spawn **之前**用 `task_user_offer(code, pages, stack)`
   把页寄存在 `kernel/task.c` 的静态量里，`task_create` 在同一次 `cli` 段内取走记账——
   因此不存在"任务已经在跑、页还没记上"的窗口。任务死时，`release_pending()`
   逐页关租约（代码页 + 栈页）、`page_free_run()`、再还 `esp0_stack`，
   最后才调 dtor；**dtor 现在只置 `done` 与死因**，不再碰任何页。

为什么"页归任务"比"页归 job"更好：任务的死只有调度器知道（三种死法：
`return` / `task_exit` / guard 命中），把回收挂在"任务死亡"这一个事件上，
就不会出现"job 单槽被下一趟覆盖、上一趟的页没人还"的漏。

**自检 `tools/userpg.sh` 的判据**（探针是新加的 `boot/uprog3.asm`）：

1. 空闲时 `tss.esp0 == tss_stack+4096`，且 `== tasks[0].esp0`（都在 < `0x20000`）；
2. 用 `call 0x18:0` 的忙等程序在 ring3 里待几秒（ring3 期间 `IF=0`，
   tick 不会涨，所以只能烧 CPU）；采样器 `tools/qmp-watch.py` 在**一条 QMP 连接**
   里反复读 `CS`、`tss.esp0`、以及 5 个槽位的 `tasks[i].esp0`——必须同一瞬间读，
   因为槽位在任务死的那一刻就 `UNUSED` 了，事后再对表已经对不上；
3. 断言：`CS=0x23` 时 `tss.esp0` 落在 `[0x20000, 0x90000)`、页对齐、
   ≠ 默认栈顶，并且**等于某个槽位的 `esp0`**；
4. 跑完 `tss.esp0` 回到默认栈顶，`heap pf=` 前后一分不差。

实测（`qemu-system-i386`，无 KVM）：

```
A1. 空闲：tss.esp0=0x16870  tasks[0].esp0=0x16870  默认=0x16870
B. 采样 5 次，其中 ring3（CS&3=3）5 次
   ring3 那一行：S 0023 155648 92272 155648 0 0 0
   ring3 时的 esp0 就是 slot 1 的 esp0
C. 收尾：tss.esp0=0x16870  heap pf=5/112
```

那一行 `S 0023 155648 92272 155648 0 0 0` 的全部含义：`CS=0x23`（真在 ring3）、
`tss.esp0=155648=0x26000`、`tasks[0]=0x16870`（main 仍是默认栈）、
`tasks[1]=0x26000`（就是这个任务自己的页），其余槽位 0。

**踩到的三个坑**：

1. `QEMU` 的 `xp /Nd` 输出是**十进制**（`xp /8dw` 打的是 `0 92272 16 0`），
   脚本里不能当十六进制用；`0x16870` 与 `92272` 是同一个数。
2. **QMP 的 unix socket 一次只能挂一个客户端**。第一版让 `qemu-sendkey.py` 与
   采样器并发跑，后连的那个直接连不上——键根本没发出去，屏幕上连 `exec` 行都没有，
   采样 400 次全是 `CS=0008`。改成"先敲完键、再开始采样"，一次就稳。
3. `exec` 主行加上 `ep=` 之后到了 **81 列**，被 79 列上限截断成 `...ep=0x0`。
   于是把 `ep=` 与 `u=` 一起挪到第二行 `exec u=2 ep=0x0000`；`elf.sh` 也从
   "主行末尾取 ep"改成"整屏找 `exec u=… ep=0x…` 那一行"。

kernel 从 78000 涨到 **78864 字节**（`__kernel_end=0x18024`，分配区余量 32732），
`check-all.sh` 现在 19 项，全绿。


## 系统调用表：门从"一个 0xFF"变成四个号

上一节末尾留下的问题是"程序跑得起来，但问不了任何事"。`kernel/syscall.c` 把门
（GDT 0x18）的语义从"一种"扩成"一张表"，`include/linux/syscall.h` 是这张表的契约：

| 号 | 干什么 | 参数 | 返回 |
|---|---|---|---|
| 0 | tune | a0 = 字段号，a1 = 新值 | 旧值（被拒为 0） |
| 1 | write | a0 = 串首址，a1 = 长度 1..63 | 写出的字节数（被拒为 0） |
| 2 | exit | a0 = 退出码 | 不返回，执行流回内核 |
| 3 | ticks | 无 | 已收到的时钟 tick |

请求体从两个字变成四个字：`struct sys_req { nr, a0, a1, a2 }`（16 字节），
eax 仍然是指针。**0xFF 保留原义**：老程序用 `field = 0xFF` 说"我干完了"，
等价于 exit 但不带退出码，走的是一条不做记账的旁路。

四层校验里有两层是新的、都跟"用户给的指针"有关：

1. `sys_ptr_ok(ptr, bytes)`：4 字节对齐 + 落在窗口内 + `bytes` 不越界；
   越界判断写成 `bytes > hi - ptr`（不是 `ptr + bytes > hi`），因为后者会溢出；
2. 窗口只覆盖"用户栈顶往下 128 字节"（`exec.c` 的 `EXEC_WINDOW`）。窗口外
   的指针**一次也不解引用**，直接返回 0 并记一次 `denied`——让用户指针把内核
   带进 `#PF` 是最糟的失败方式，内核会以为是自己的 bug。

write 只接受可打印 ASCII（0x20..0x7E）与 `\n`、`\t`，有一个字节不合格就整条拒掉
（不做替换）。理由不是"安全"，是"证据可信"：往显存里塞控制码会把屏幕搞花，
看到的就不是原文了。

**最难修的一处不是新功能，是旧证据。** ABI 一变，`boot/intr.asm` 里那三段
ring3 自检代码（`user_stub` / `user_probe` / `user_io`）都得跟着改：
`user_stub` 的请求体从 `{field,value}` 变成四个字，`user_probe`/`user_io`
的 `sub esp, 8` 要改成 `sub esp, 16`——否则门处理体一次读 16 字节，会读到栈顶
外面 8 个字节，被 `sys_ptr_ok` 判越界，自检的返回值就全是 0。
`make verify` 当场抓住了它（row 14 的"旧值应为 0x1E"变成 0），改完两轮确实全过。
这正是基准文件的价值：**改 ABI 的人不会记得所有旧调用点，回归会记得**。

`tools/exec.sh` 现在跑两趟：

```
  B. exec uprog rc=0 mag=0x45584543 val=36 ex=36 sy=2/0 n=1/0
  B2. 程序自己打的字：uprog:ring3 ok       ← 屏幕上真出现了这一行
  E. exec bad rc=E5 mag=0xDEAD0000 val=3735879680 ex=7 sy=2/1 n=1/2
```

`B` 的 `sy=2/0` 是"这一趟过了两次门、一次没被拒"（write + exit）；
`E` 是负向程序 `boot/badprog.asm`：write 时故意传 `esp-4096`（窗口外）、
也不写指纹。于是 `sy=2/1` 证明门真的拒绝了那次解引用，`mag=0xDEAD0000`
（十进制 3735879680）证明 exec 的哨兵没被改过，`rc=E5` 是"没跑完"的正确判断，
`ex=7` 反而是"程序自己交回的退出码"——**三件事彼此独立，谁也糊不了谁**。

`sy` 的账按"这一趟"算（`cmd_exec` 里跑前跑后各读一次取差值）。用累计值会把
自检里那些过门的调用算进来，"这一趟打了几次 syscall"就看不出来了。

### 从"exec 自己下潜"到"交给一个任务下潜"

上一版 exec 是同步的：它在主上下文里 `user_enter`，自己在环 0 等程序回来，期间
IF=0、连调度都不发生。现在这一趟被搬进了一个任务：

1. `exec_run` 只做"读文件 → 要两页 → 拷 → 租约放行 → `task_spawn_owned` → 等"；
2. 真正的 ring3 下潜在 `exec_task_body` 里，跑在**新任务的栈**上；
3. 收尾在 `exec_task_dtor` 里——入口 return 之后掉进 `task_return_stub`，
   `task_die(RETURN)` → 调度器换栈之后才调 dtor，它**只记死因并置 `done`**；
   页的回收由调度器在 `release_pending()` 里做（页在 spawn 之前就已经
   `task_user_offer` 交给任务了，见「每任务一套用户页」）；
4. exec 的"等"是 `while (!job.done) { task_reclaim(); cpu_hlt(); }`。

第 4 步那个 `task_reclaim()` 不是可有可无的：任务死掉之后 `schedule()` 已经把
`active` 归 0，下一次 tick 进来直接返回，**永远不会**再走到 `release_pending()`，
于是 dtor 一辈子不会被调用、`job.done` 永远是 0。这一句是"抢占式阻塞"的
全部秘密：hlt 让出 CPU，醒来时手动把僵尸栈与 dtor 交接掉。

证据对比很干净——上一版和这一版的输出只差末尾两格：

```
上一版：exec uprog rc=0 mag=0x45584543 val=36 ex=36 sy=2/0 n=1/0
这一版：exec uprog rc=0 mag=0x45584543 val=36 ex=36 sy=2/0 t=1 d=0 n=1/0
```

`t=1` 是"跑在槽位 1 的任务里"，`d=0` 是那个任务的死因 `TASK_DEATH_RETURN`
（入口函数正常 return），也就是 **dtor 真的被交接了**——不是 exec 在场时顺手
把页还掉的。负向程序那趟同理（`t=1 d=0`、`rc=E5`、`sy=2/1`）。

`tools/exec.sh` 现在把这两格当判据：`t` 必须 ≥1，`d` 必须是 0。


新增 `sys` 命令把 syscall 层的账摊开（`exec uprog` + `exec bad` 之后实测）：

```
c=9 d=4 b=15 n=2 x=7      ← 9 次过门、4 次被拒、15 字节写出去、最近一号=2、退出码 7
```

`d=4` 里有两笔来自自检（白名单拒绝、越界指针），两笔来自 `bad` 的越界指针那趟
（门拒绝 + write 返回 0 被计为拒绝）——这恰好说明 `denied` 不是"程序不听话"的
计数，而是"内核拒绝服务"的计数。

## 内存分两半：`page.c` 管页框，`paging.c` 管页表

`mm/page.c` 原来是 359 行，混着两件事：**哪一页是空的**（位图、分配、释放、用量）
和**这页怎么出现在地址空间**（补 PDE/PTE、撤/恢复恒等映射、收 U/S）。
两者的交界只有一条：`paging.c` 需要页时调 `page_alloc()`，撤映射后调 `page_free()`。

| 文件 | 行数 | 拥有什么 | 不许做什么 |
|---|---|---|---|
| `mm/page.c` | 132 | `bitmap[]` + `used_frames`、`page_init()`（`__kernel_end` 边界检查）、`page_alloc/pair/free`、`page_frames_*` | 不写页表 |
| `mm/paging.c` | 237 | `page_demand_window`、`page_fault_map`、`page_unmap`、`page_identity_unmap/restore`、`page_mark_user/kernel`、`page_identity_flags`、`page_lock_kernel` | 不碰位图，只通过 `page_alloc/page_free` 要页 |
| `include/linux/pageint.h` | 17 | `PAGE_SIZE`、`PTE_FLAGS`、`PTE_KERNEL`、`DEMAND_BASE/SIZE` | 不是对外接口（`include/linux/page.h` 才是） |

好处是两条最容易互相踩的规则现在各归一处：**分配器的边界**（不让内核映像长进
`PAGES_BASE`，由 `link.ld` 符号 + `kernel-fit.py` + `page_init()` 三层守）只写在
`page.c`；**TLB 规则**（改页表必须紧跟 `invlpg`，否则改页表等于没改）只写在
`paging.c`。代价为零——这次是纯搬运，kernel 仍是 56672 字节、`__kernel_end`
仍是 `0x1141C`，`verify.sh -n 3` 三轮 PASS，`ring3-io` 仍 PASS。

## 物理页分配器

`mm/page.c` 管理 `0x25000–0x90000` 的 428KB 低端空闲内存，共 107 页，用一张
110 位的位图（4 个 `unsigned int`，实际只有 3.35 个字有定义）记录占用：

> 起点最初是 `0x10000`，后来抬到 `0x20000`；块设备 / MBR 分区 / 块缓存三件
> 落地后抬到 `0x21000`，多级目录那 330 行落地后再抬到 `0x22000`，`file_dev` 那层抽象再抬到 `0x23000`，管道那 8 个环再抬到 `0x25000`——每次抬页
> 都同步改 `tools/verify.py` 的 `frames=…/0xNN` 与 `tests/expect/boot-screen.txt`。

| 接口 | 语义 |
|---|---|
| `page_init()` | 先用 `__kernel_end` 验一遍“映像没长进分配区”（越界即 `[mem] FATAL` + `kernel_halt()`），再清零位图与计数。位图在 `.bss`，链接出来的二进制不带 `.bss`，QEMU 恰好给零内存，但必须显式初始化才站得住 |
| `page_alloc()` | 找第一个空闲位、置位、计数 +1、清页，返回物理地址；耗尽返回 0 |
| `page_free(phys)` | 只接受页对齐且落在区间内的地址，非本分配器给出的页或重复释放直接忽略 |
| `page_alloc_pair()` | 找**相邻两页**都为空的位置，两页一起清零、返回低地址那页；给任务栈凑“guard + 栈”用 |
| `page_identity_unmap(phys)` | 清掉恒等映射里对应的那一格 PTE 并 `invlpg`，返回原 PTE（页本身仍归分配器） |
| `page_identity_restore(phys, pte)` | 把原 PTE 写回去；归还页之前必须调用，否则下次 `page_alloc` 的清页会自己缺页 |
| `page_frames_used/free/total()` | 用量统计，供自检打印 |

单个字节的记录量是 1 bit/页（112 页只要 14 字节），比空闲链表省内存，代价是分配
要**扫位**——但扫描不必每次从 0 开始：

`page.c` 里维护一个 `next_free`，它只承担一个含义：**这之前的页全都已占用**。
不变式靠两处维持——分配成功把起点推到下一页之后，释放把起点拉回更小的那一页。
于是连续分配 k 页一共只走 k+1 步（摊还 O(1)），而不是每次都从 0 重扫。
`page_alloc_pair` 不动它：相邻性必须把低地址也看一遍，而它只标“已占用”，
不会破坏那条不变式。

实测（临时在 `page_init` 之后把 112 页全部分完再全部还掉，gdb 在 `kernel_idle`
读计数器）：**从 0 扫 = 6375 次位测试，带游标 = 124 次**（其中 5 次是正常启动的
真实分配），51 倍差距。测完把计数器和压力段一起删掉——留在内核里的只有
`next_free` 和三行注释。分配顺序一位未变，所以 24 行自检屏原样通过
（`verify.sh -n 3` 三轮 PASS，kernel 56672 → 56736 字节）。

## 按需分页

缺页处理不再只是打印现场，而是会补页后重试：

1. 数据段界限先在 `head.asm` 的 GDT 里从 16MB 放到 4GB（`0x00CF92000000FFFF`）。
   否则访问 16MB 以上的地址会先撞段界限触发 `#GP`，根本走不到 `#PF`；
2. 按需分页窗口定在 `0x02000000`（32MB）：页目录项 8 初始为 0，第一次访问必然缺页；
3. 处理顺序是“缺 PDE 就分配一张页表页 → 缺 PTE 就分配一页物理内存 → `invlpg` 刷 TLB
   → 返回 → 桩 `iret` 重试那条指令”。指令是真的重跑一遍，不是伪造返回值；
4. 越界判定分两层：地址不在窗口内，或页已存在（`err` 的 P 位为 1，属于权限问题，
   补页解决不了），或分配器已耗尽，三种情况都返回 0，交回上层 panic。

`page_unmap(addr)` 是反向操作：清 PTE → `invlpg` → `page_free` 归还物理页。之后
再访问同一地址会重新缺页，演示“释放 → 重新补页”这条闭环。

映射刻意做成**非恒等**：虚拟 32MB 落到分配器给出的低端物理页（`0x27000`、`0x28000`…），
这样页表翻译确实在工作，而不是地址碰巧相同。

**这条窗口只属于内核表**：每任务页目录的 `dir[4..1023]` 一律 0，所以用户任务
碰到 32MB 不会触发"内核替它补一页"——在它自己的表里那里根本不存在，
`#PF` 直接按越权处理。换句话说，"按需分页"是内核态的能力，不是给用户的
一条增长通道（见本章最后一节「每任务页目录」）。

## 已验证结论（Linux QEMU 6.2 + gdb 12.1 实测）

`make debug` 停在 `kernel_halt`（异常处理与停机共用这个入口）时抓到的现场：

```
row  0| mini-os 0.2  boot OK  boot -> setup -> head -> main
row  1| [cpu] esp=0x0000E470 ss=0x00000010 mem_k=0x00003B80
row  2| [idt] 00 div | 0D gp | 0E pf | 20 timer | 21 kbd | 30 yield | 31 tune
row  3| [pit] 0x00000064Hz div=0x00002E9C cnt=0x00002DC7
row  4| [rtc] dsec=0x00000001 dtick=0x00000048 | tick=0x00000076 ms=0x0000049C
row  5| [vga] hw=0x00000190 shape=0x00000E0F | [kbd] mk=0xFC irq=0x1 q=0x3 sh=0x1
row  6| [mem] demand base=0x02000000 size=0x00400000
row  7| [mem] PF 0x02000000 -> 0x00011000 err=0x00000002 resume
row  8| [mem] PF 0x02001000 -> 0x00012000 err=0x00000002 resume
row  9| [mem] read 0xA5A5C0DE , 0x12345678 | unmap 0x02000000 pa=0x00011000
row 10| [mem] PF 0x02000000 -> 0x00011000 err=0x00000000 resume
row 11| [mem] touch 0x00000000 frames=0x00000003/0x00000080
row 12| [task] p1 U=0x06 V=0x05 s1=0x07 | p2 H=0x09 L=0x02 sw=0x04
row 13| [gate] dpl=0x3 deny=0x00 old=0x1E | own dtor=0x01 mag=0x00 seen=0x1 why=0x0
row 14| [ring3] tr=0x30 cpl=0x3 ret=0x1E rej=00/00 set=0x28 cap=0x100 knl=5505 ls=1/3/1
row 15| [task] A ret, C 50ms, B 100ms spawn D(0xDEAD0001) null=0x00000000
row 16| [task] wake C=0x3 gc=0x5 B=0x2 gb=0xA seq=0x12 arg=0xDEAD0001
row 17| [task] D slot=0x00000001 arg=0xDEAD0001 lt=0x1/0x4
row 18| [task] Y x3: Y=0x00000004/0x00000005/0x00000002 sw=0x000244D4
row 19| [task] sq order: 0x00003142
row 20| [sched] decay prio 0x2 -> 0x0 ticks=0x00000008 rest=0x1/0x0/0x1 deep=0x2/0x1
row 21| [sched] guard: slot=0x1 va=0x00015FFC stack overflow -> task killed why=0x2
row 22| [sched] sw=0x00000001 frames=0x00000005/0x00000080 rq=0 sq=0 |tune 1E/00/3C
row 23| [mem] PF 0x04000000 err=0x00000002 (not-present) write -> unhandled
```

**row 24 默认是空的**：它专用于真按键回显（见「键盘」一章），没有按键时整行空白，
所以基准文件里不含这一行，`vga-screen.py` 也会跳过全空行。注键后同一份 dump 会多出
`row 24| hello`。

24 行，正好塞满 25 行文本屏的非滚动区（写第 25 行就会立即上滚，开机那行
banner 会被顶掉——实测过一次）。屏幕预算的取舍过程：为了让阶段四/五/六/七各有一行，先把阶段一/二/四的标签与账单合并成同一条线，再把 `[vga]`
与 `[kbd]` 两段合成一行（73 列）。

**行首主题标签**：每行前 6 列是固定的方括号主题，眼睛可以直接按主题跳读，不再靠
`phase3/phase4/...` 这种流程序号找位置。顺序固定为
`[cpu] → [idt] → [pit] → [rtc] → [vga]/[kbd] → [mem] → [task] → [gate] → [ring3] → [sched]`，
即“先在哪个特权级跑起来，再有哪些设施，最后是调度账目”；`[mem]` 一行会在按需分页那段
重复出现三次，`[task]/[sched]` 也会随阶段推进重复，标签重复是刻意的（同一主题的账目挨着看）。

字段名同时做了压缩（`ext_mem_k`→`mem_k`、`divisor`→`div`、`counter`→`cnt`、
`timer: ticks=`→`tick=`、`unmap va=`→`unmap`、`readback:`→`read`、`touch again:`→`touch`），
腾出的宽度留给标签。压缩是**有代价的**：`tools/verify.py` 的抖动字段白名单和逐行区间断言
都认这些字面串（`cnt=`、`| [kbd] mk=`、`Y x3:`、`[sched] guard:`），改名字必须同步改断言。

**列宽硬约束：一行最多 79 列。** 正好 80 列时行尾那个 `'\n'` 会把光标顶到下一行，
凭空多出一行，第 25 行触发上滚，banner 被挤出屏幕（这次整理时又踩了一遍：
`[sched] sw=...` 那行 80 列，屏幕于是少了 banner、多了一行空白）。当前最紧的两行是
`row  5`（73 列）和 `row 14`（79 列，`[ring3]`），后者已经贴着上限，再往里加字段
必须先挪走别的东西。

`cursor: hw=0x190` = 5×80 = 400，即第 5 行行首；行号一变这个数就跟着变——它是
“光标跟着输出走”的活证据，不是装饰。

通用保护路径另用临时副本单独验过（把触发语句换成 `mov ax,0x28; mov ds,ax`，
0x28 指向 GDT 的空项）：

```
row ?| EXCEPTION: general protection err=0x00000028 -> halt
```

错误码恰好是出错的选择子 `0x28`。

对应结论：

- 引导扇区被 BIOS 加载并成功读盘（此前 `read_track` 的 `jnc` 取反已修正）；
- `setup` 搬移内核、开 A20、进保护模式，`CR0 = 0x80000013`（PE|PG|ET|MP）；
- `head` 建好 4 张页表（`pg0..pg3` 各占 4KB，恒等映射前 16MB），跳进 C `main`；
- `stack_start` 被正确装载：`esp = 0xA4C0` 在 4KB 内核栈内（`kernel_stack=0x94C0`,
  `+0x1000` 即栈顶），`ss = 0x10` 为一个数据段选择子；
- `boot_params.ext_mem_k = 0x3B80`（15232 KB），说明 0x90000 的 BIOS 参数区被正确读取；
- **时钟中断在跑且频率被改过**：一行 `pit:` 给出 `hz=0x64`、`divisor=0x2E9C`、读回的
  `counter`（小于分频值，说明重载生效）；`rtc:` 一行显示一个 RTC 整秒里来了
  `dtick=0x4A`（74）个 tick——对照组（不重编程）是 18，所以频率确实提上去了。
  达不到 100 的原因在"定时器"一节写清了，是 QEMU/TCG 的服务速率上限；
- `timer: ticks=0x75 ms=0x492`：117 tick 换算出 1170ms = 0x492，tick→ms 换算正确
  （这一行现在接在 `rtc:` 后面，同一行两段读数）；
- **硬件光标与软件位置一致**：第 6 行（行号 5）行首读回 `cursor=0x190`，
  而 5×80 = 400 = 0x190——先读后打印，数字对上了，说明前面 5 行的换行/滚屏之后
  CRTC 光标一直被正确同步；`shape=0x0E0F` 说明扫描线 14..15 的下划线光标已生效。
  另外开头还做了一次“搬到 1234 再读回”的往返测试，失配会多打一行报警，实测没有出现；
- **按需分页真的补页并重试**：两次写 `0x02000000` / `0x02001000` 各触发一次缺页，
  分别映射到物理 `0x11000` / `0x12000`（`0x10000` 那张页表页先被分配，故 `used=3`）；
  随后读回 `0xA5A5C0DE` / `0x12345678`，说明重试的那条指令执行成功、数据落在真实内存里；
- **释放后再访问会重新缺页**：`page_unmap` 撤掉第一页的 PTE 并归还物理页，`used` 短暂回到 2；
  再次读取 `0x02000000` 又触发一次缺页（这次 `err=0x00000000`，即内核态读而非写，
  与前面写访问的 `0x02` 形成对照），补页后拿到**同一物理页 `0x11000`**，因为位图分配器
  总是优先给出最低的空闲页；读到 `0x00000000` 说明新分配时执行了清页，旧数据已被清掉；
- **时间片按 tick 递减**：阶段一两个同等优先级的 CPU 型任务，slice 取默认 2、忙 12
  个 tick，账单是 `U=6 / V=5`，切换 7 次——12 个 tick 基本对半分，切换次数 ≈ 12/2。
  若 slice 被忽略（每 tick 都换），切换次数会是 11~12；若只看优先级不轮转，就只会
  有 1 次切换、账单 11:0。三个数字合起来才把“slice 生效”钉死；
- **调用门是“能被 ring3 call”的那一种门**：描述符里 DPL=3 是读回来验的（`dpl=0x3`），
  `lcall $0x18` 能真的走进去并返回旧值（`old=0x1E`），而白名单外的字段被拒
  （`deny=0x00`）且 `age_ticks` 保持不变。三条缺一条都不算“对外入口可用”；
- **参数所有权有交接点**：带 `dtor` 的任务死后，`dtor` 恰好被调用一次
  （`dtor=0x01`）、载荷里的 `magic` 被清零（`mag=0x00`）、而任务活着时确实读到过
  载荷（`seen=0x1`）。这三条把“谁负责释放”从口头约定变成可判等的证据；
- **死因是从真路径里带出来的，而且横死不会冒充正常收工**：`row 13` 的 `why=0x0`
  来自入口 `return`，`row 21` 的 `why=0x2` 来自一个被 guard 页杀掉的任务——两条完全
  不同的退出路径，析构收到的 reason 不同。判等断言的好处是：如果有人把 guard 那条
  路径改回普通 `task_exit`，`why` 会变成 `1`（就是我做过的那次负向验证）；
- **回血是按倍数折算的，不是“够阈值就还一级”**：`deep=0x2/0x1` 两段各从最低级
  出发，睡 2 个单价还两级、睡 1 个单价还一级。老实现两段都是 `1`，所以第一个数
  就是线性与阶梯的分界线；
- **老化把饿死变成了保底**：阶段二 `H`(prio2) 与 `L`(prio0) 忙同样 12 个 tick，
  无老化时是 `H=0xB / L=0`（上一版实测），加老化后是 `H=9 / L=2`。L 拿到的是
  2 个 tick 的短片——升到最高级后按 prio2 的时间片跑完就降级，没有反过来霸占 CPU；
- **时间片必须跟着当前级别走**：第一版把 slice 在创建时按 `base_prio` 定死，
  升到 prio2 后仍拿着 8 tick 的长片，实测变成 `H=5 / L=6`（低优先级反而更占便宜）。
  改成 `slice_for()` 按当前 `prio` 查表后回到 `9:2`；
- **衰减把贪心压下去，而且独跑也压得住**：阶段六一个 prio2、`slice=1` 的任务独跑，
  没人跟它抢，8 个 tick 后从 prio2 落到 prio0（`prio 0x2 -> 0x0 ticks=0x8`）。
  这与老化的方向正好相反：老化是“等太久了临时借一级”，衰减是“烧太狠持久降一级”，
  两者一个抬 `prio`、一个压 `base_prio`，互不覆盖；
- **队里只剩自己时也必须重新装填时间片**：这一条是阶段六逼出来的——原来
  `left = slice_for()` 只在“换人”分支做，独跑时 `left` 恒为 0，每个 tick 都被记成
  “跑满一片”，衰减按 tick 而不是按片算，同样的降级只用了 3 个 tick。补齐这条分支后
  才恢复成 8 tick；
- **ring3 真的进来了，而且是自己走门回来的**（`row 14`）：`tr=0x30` 说明 `ltr` 装载成功；
  `cpl=0x3` 说明门处理体看到的调用者确实是 ring3（自检里内核自己也走同一条门，
  那一次记的是 0——这两个值必须一起看，否则“门能跑”会被误读成“ring3 能跑”）；
  `ret=0x1E` 是**用户态代码自己**写进栈页的返回值，说明 `retf` 回来了、`eax` 带回了值；
  `set=0x28` 说明这次写真的落到了 `restore_ticks`；`rej=0x00/0x00` 是两道
  拒绝（超上限、落在未映射地址的野指针），后者若放行，内核会替用户 deref
  未映射地址并直接 `#PF` —— 负向把窗口校验短路之后，屏幕只剩 15 行就停了；
  `knl=5505` 是后四次进 ring3 留下的证据：`page_lock_kernel(0, 16MB)` 清掉整张恒等
  映射的 U/S 之后，1MB 内的内核页与 10MB 处的空白页都被 CPU 拦成 `P|U|读` 的缺页，
  而被 `page_mark_user` 放行的用户栈页仍读得到（中间那位是 0），
  再被 `page_mark_kernel` 关上之后又变回 5（末位）——见上文那一小节；
- **睡眠时长是准的**：合并行里 `C` 的 `gc` 是 `0x4`~`0x7`（50ms）、`B` 的 `gb=0xA`
  （100ms），与 `task_sleep()` 的参数一致。偶尔多 1 个 tick 是 print 到 `task_sleep`
  之间挤进一个 tick 的正常抖动；
- **睡眠堆真的按键排序，不是先来先醒**：阶段三 `B` 先创建、睡 100ms，`C` 后创建、
  睡 50ms，出场顺序却是 `C` 在前 `B` 在后；阶段五把这件事做成了**可判等**的证据——
  4 个任务睡 100/200/50/150ms（创建顺序 slot 1/2/3/4），醒来顺序打出 `0x3142`，
  也就是 3→1→4→2。若堆忘记上浮（退化成 FIFO），这行会变成 `0x1342`；
  若按“先睡先醒”，会是 `0x1234`——三种排列分得开，才说明这个结构真的在排序；
- **槽位复用与传参都生效**：`A` 一进来就 `return`，返回地址是 `task_return_stub`，
  于是它自动退出、`slot 1` 立刻变 `UNUSED`；`B` 之后带 `0xDEAD0001` 创建 `D`，
  `D` 打出的正是 `slot=0x1 arg=0xDEAD0001`。`task_create` 扫的是“第一个 `UNUSED` 槽”
  而不是硬编码槽号，换任一任务先退出都能被复用；
- **参数是两跳传过去的，而且能断**：`main → B → D` 这一链上两处 `arg` 都打在屏幕上
  （`row 16` 是 B 收到的、`row 17` 是 D 收到的），两处都等于 `0xDEAD0001` 才算链路成立。
  负向验证把 `frame[12] = arg` 改成 `= 0`，这两行立刻变 `arg=0x00000000`；顺带把
  `phase5` 的醒来顺序从 `0x3142` 打乱成 `0x1432`——因为 4 个 sleeper 的睡眠毫秒数
  也是走这个参数传进去的（`task_sleep((unsigned long)arg)`）；
- **空入口在建任务之前就被拒**：`task_spawn(0, …)` 返回 0 且不占槽位/栈页（行上
  `null=0x00000000`，后面 A 拿到 slot 2 也印证了没白占）。短路这条判断的负向补丁会让
  iret 跳到 `eip=0`、整机三重故障——`verify` 连一行屏幕都抓不到，只能报
  “没有解析到任何 row 行”；
- **入口函数用返回桩收尾**：`A` 什么都没写，只是从 `worker_a` 里 `return`，程序没有
  跑飞也没有卡死——`iret` 落到 `[esp]` 的 `task_return_stub`、`[esp+4]` 恰好是 arg，
  说明 cdecl 帧摆放是对的；
- **退出后栈页真的归还**：所有 worker（含动态创建的 `D`）都退出后，`frames` 是
  `0x00000005/0x00000080` = 3（内核自己 + 一个栈页 + 一个 guard 页的基线）
  **+ 2 张常驻的 ring3 页**（用户代码页 + 用户栈页）。它们没有回收，因为这个内核
  还没有“进程销毁”这个概念：ring3 的 demo 跑完就停在原地。这条数字因此同时钉住
  两件事——“worker 的栈页都还了”，以及“ring3 只占两页、没有泄漏更多”；
- **两个队列最终排空**：`rq=0 sq=0` 说明没有任务残留在队里（否则 `task_active()`
  会一直为真、`main` 永远醒不过来）；
- **yield 真的会换人，且时间片必须一起交出去**：阶段四三个 prio=2 的任务每趟
  `task_yield()` 一次，12 个 tick 里切了 `0x2EEF`（12015）次——对比阶段一同样的
  12 个 tick 只有 `0x7` 次。把 `left = 0` 删掉做对照，`sw` 立刻掉回 `0x8`，
  yield 成了空操作（被“时间片没用完就继续跑”的短路整个吞掉）；
- `sw`（阶段三）：`A→B→C→main` 4 次、`C↔main` 2 次、`B→D→main` 2 次；
  多次复跑数字一致；
- **栈溢出被隔离而不是拖垮内核**：阶段七故意往 `bottom - 4` 写一个字，缺页落在
  guard 页 → 那个任务被改道 `task_guard_exit`（槽位归还、栈页与 guard 页延后回收，
  死因记为 2 并传给 `dtor`），`sched: done … rq=0 sq=0 frames=5/0x80` 说明它被当成
  一次正常的任务死亡处理了，内核继续跑；
  同一份屏幕里仍保留“不在窗口内的缺页 -> 停机”，证明隔离只针对 guard 页，不是把
  所有缺页都放生；
- **越界缺页仍然被拦下**：`0x04000000` 不在窗口内，页目录项也没建，打印
  `err=0x00000002 (not-present) write -> unhandled` 后停机；
- **带错误码的桩收尾正确**：GP 的错误码 `0x28` 被正确读出，说明 `pusha` 之后按
  `[esp+32]` 取错误码、返回前 `add esp, 4` 丢掉它的逻辑对 13/14 都成立。
- `esp=0xCC70`：main 的 4KB 内核栈顶（`kernel_stack` `0xBC70` + `0x1000`），
  仍低于 `0x10000` 的页分配器起始地址，两者不会踩踏。

**符号怎么找**：kernel 现在 47212 字节，地址**每次加代码都会整体漂移**，所以这份文档
不再逐条抄地址（抄了必然过期）——一律在调试机上现取：

```
nm -n build/kernel.elf | grep -w <符号名>
```

要盯的符号按模块列在这里，`tools/` 里的脚本也全是先 `nm` 取地址再 `xp` 读值：

| 模块 | 符号 |
|---|---|
| 中断桩/门 | `irq0_timer`、`irq1_keyboard`、`isr_page_fault`、`isr_tune`、`tune_gate_entry`、`ignore_int` |
| ring3 | `user_enter`、`user_return_stub`、`user_stub`、`user_stub_end`、`user_probe`、`user_probe_end`、`worker_lease` |
| 收尾/idle | `kernel_halt`、`kernel_idle`、`exception_handler_pf` |
| 页与租借 | `page_mark_user`、`page_mark_kernel`、`page_lock_kernel`、`page_lease_open`、`page_lease_close`、`page_lease_release_owner`、`page_lease_tick`、`page_lease_bind`、`page_lease_notified`、`release_pending`、`binds`、`notified_total`、`leases`、`lease_peak`、`dtor_runs` |
| 调度 | `tasks`、`task_switches`、`task_spawn_owned`、`task_kbd_tag_of`、`task_lease_note`、`task_lease_forget`、`task_lease_at`、`task_lease_peak` |
| 键盘-解码 | `kbd_feed`、`irq1_keyboard_handler`、`keyboard_init`、`kbd_pic_mask`、`kbd_irq_count`、`kbd_dropped`、`shift_on`、`ext_state`、`last_char`、`last_cmd` |
| 键盘-输入行 | `kbd_echo_count`、`kbd_echo_erase`、`kbd_echo_newline`、`kbd_echo_wrap`、`kbd_echo_move`、`echo_len`、`echo_col`、`line_buf`、`hist_buf`、`hist_len`、`hist_count`、`hist_pos`（**在 .data**：初值 -1 不是 0） |
| 键盘-提交队列 | `kbd_line_push`、`kbd_line_take`、`kbd_line_pending`、`kbd_line_capacity`、`kbd_line_submit_count`、`kbd_line_drop`、`submit_q`、`submit_qlen`、`submit_pending`、`submit_count`、`submit_drop`、`line_taken_buf`、`line_taken_len`、`line_taken_count` |
| 键盘-归属/深度 | `kbd_owner_attach`、`kbd_owner_detach`、`kbd_owner_select`、`kbd_owner_force`、`kbd_owner_active`、`kbd_owner_switches`、`kbd_owner_seen`、`kbd_owner_last`、`owner_mask`、`kbd_getchar`、`kbd_getchar_t`、`kbd_queue_count`、`kbd_queue_count_t`、`kbd_queue_total`、`kbd_lane_drop`、`kbd_lane_peak`、`kbd_lane_depth`、`kbd_lane_set_depth`、`lane_depth`、`kbd_line_set_capacity`、`submit_depth`、`q_head`、`q_tail`、`q_count`、`queue`、`lane_drop`、`lane_peak` |
| 自检证据（非 static，专供 `xp`） | `own_slot`、`own_got`、`own_iso`、`own_after`、`lane_peak1`、`lane_drop1`、`lane_q1`、`lane_q0`、`subq_peak`、`subq_order`、`subq_full`、`subq_drop`、`dep_lane_depth`、`dep_lane_peak`、`dep_lane_drop`、`dep_lane_restore`、`dep_sub_cap`、`dep_sub_full`、`dep_sub_restore` |
| 栈 | `kernel_stack`（顶 = 它 + 0x1000）、`main` |

## 已知限制

- `printk` 只支持纯字符串，无 `%d/%s` 格式化；
- 中断/异常只做了七个向量：0（除零）、13（通用保护）、14（缺页）、0x20（IRQ0 时钟）、
  0x21（IRQ1 键盘）、0x30（主动让出）、0x31（调度参数），其余仍是 `ignore_int` 兜底；
  双故障（8）没有专门处理；
- 键盘解了扫描码集 1 的 `0x00..0x39`，扩展码有前缀状态机但**只认七个键**
  （Left/Right/Home/End/Delete/Up/Down）；`Pause`（`0xE1` 六字节序列）整段丢弃，
  其余扩展键（小键盘、右 Ctrl/Alt、多媒体键）整码丢弃；没有 Ctrl/Alt 组合键、
  没有 LED 控制、没有重复率设置；
- 键盘输入队列虽然能按 owner 隔离了，但**只有 3 个可 attach 的槽**（`KBD_OWNERS=4`，
  槽 0 保留给共享）：第 4 个任务领不到槽会退回共享，它打的字和内核混在一起；
- 归属随调度器走，但**只在换人那一刻搬一次**：任务睡着时归属还留在它身上，
  直到下一次真换人才搬走——期间按的键仍记在睡着的任务名下；
- 隔离已经做到**每条环各自算容量**（深可配，默认 4、上限 8）：一个 owner 连续
  快过消费者的字符超过自己这条环的深度就丢（丢的是它自己的，别人的不受影响，
  `lane_drop` 记一笔）。深度是运行时可改的，但**谁改、按什么阈值改**还没有策略——
  “一整段粘贴”这种量级应该由消费者（未来的 shell）在拿到慢的信号后自己调；
- `kbd_owner_detach()` 会顺着标签把它的字符丢掉，但**不会叫醒**正阻塞在
  “等自己字符”的消费者（现在没有阻塞接口，所以暂无影响）；
- `int 0x21` 被自检拿来软触发（IF=0 下执行），这假设了 PIC 当前没有正在服务的
  IRQ1——因为 EOI 是对主片一刀切的，若真有键盘中断在服务中会被提前清掉；
- 第 24 行的输入行是**单行、单缓冲区**：有左右移动/Home/End/Delete 和 4 条历史
  （Up/Down 可翻），但**没有多行滚动**、写满 79 列是“清行重来”而不是换行滚屏；
  历史是纯追加（**不去重**、不记时间戳、不落盘，重启即空）；
  回车会把整行交出去（`kbd_line_take()`），提交队列**深 4 条**：满了再回车就丢新的那条
  （`submit_drop` 记一笔），旧行不会被覆盖；4 条这个量级是按“连按几次回车”定的，
  真要做“粘贴一大段多行”还得消费者自己流控。
  现在唯一的消费者是空闲循环，所以这条通路只在“自检跑完、系统空闲”之后才成立；
  它也**没有自己的软光标变量**——光标是 `vga_cursor_hw_set()` 直接写 CRTC 的，
  `printk` 的软件 cursor 完全不知道这一行的存在；
- 调用门已经真能跨特权级了（TSS + `ltr` + 每个任务自己的用户栈，见 `row 14`），
  `exec` 起出来的任务也真的在 ring3 跑（`uprog*/argvp` 都是在用户态跑完再
  `sys_exit` 交回）；**每任务页目录**也上线了（`mm/pgdir.c`，slot 1..4 各一张
  私有表、进 ring3 才装 CR3，见本章最后一节），**写保护**也跟上了：ELF 里没有
  `PF_W` 的页只给 `P|US`，ring3 写它就是 #PF → 越权（见「用户页写保护」那节，
  证据 `tools/wx.sh`）；但没有 fork/COW；
- 门的 ABI 仍是“一个指针 + 一个 `{field, value}` 结构体”，只能一次改一个字段；
  系统调用已经另走一条路（`int 0x30`，四个号：`TUNE/WRITE/EXIT/TICKS`），
  请求体在 `[esp0-16]`，窗口 `win_lo/win_hi` 已经是每任务一份；
- `0x31` 那条路仍然不做白名单（`TUNE_MASK_ALL`）：任何内核代码都能改全部字段。
  这是有意的——内核自己应当能改自己，白名单只针对对外那条路；
- 除零与通用保护仍是 panic 语义（没有恢复路径），缺页才有补页重试；
- 物理页分配器只管 `0x10000–0x90000` 这 512KB、128 页，且不做回收后的碎片整理；
- 按需分页只有一个 4MB 窗口（页目录项 8），窗口外缺页一律停机；窗口的 PDE/PTE
  现在用 `PTE_KERNEL 0x03` 建，对 ring3 一律不可见（原来也是 `0x07`）；
- `page_lock_kernel` 是“从 lo 扫到 hi”的线性扫描，锁 16MB 要过 4096 页，只适合
  启动时一次性收口；真要做“按需开关”，得先有页表项的位图索引，否则每次都是一趟线性扫；
- 锁定范围之外（16MB 以上、32MB 窗口以外）的分页项没有任何权限策略，
  只能靠“ring3 手上没有指针”这一层纪律；
- 缺页处理的判据已经收紧成"**用户态发起就一律当越权**"（只看 `err` 的 `U` 位，
  不再问地址范围——见文末「用户态缺页：一律当越权」）。于是 `page_fault_map`
  事实上只服务内核态；但函数本身仍不看发起者，那是靠"调用方判据 + 私有表里
  16MB 以上不建"两层兜着的，没有内建防线。写时复制仍然没有；
- **100Hz 只在代码与分频值层面成立**：这台 VM 没有 `/dev/kvm`，QEMU/TCG 的定时器服务
  速率有限，实测 100Hz 只到 70 上下（50Hz 能精确到 49、18.2Hz 到 18），所以
  `timer_uptime_ms()` 在模拟器里会比真实时间长；真实硬件/KVM 下才是标称频率；
- `timer_uptime_ms()` 是 32 位毫秒，100Hz 下约 49.7 天回绕；
- 8254 只用了通道 0，通道 1（DRAM 刷新）与通道 2（扬声器）没有动；
- 键盘走 IRQ1 + 环形队列 + 第 24 行单行编辑器（`kbdline`），但只认 PC/AT 扫描码集 1、
  没有大小写锁状态机（Shift 只对字母/少量符号生效）、没有键盘 LED 控制；
  鼠标、其余 IRQ 仍屏蔽；
- 段界限只放开了一半：数据段已改成 4GB 平坦，代码段 CS 仍是 setup 留下的
  8MB 段（head 没有重载 CS），内核体积远小于 8MB，暂不受影响；
- VGA 光标只在 80x25 文本模式成立，且 `vga_cursor_show(0)` 的“关光标”目前只有
  代码路径、没有自动化验证（CRTC 是 I/O 端口，gdb 读不到，只能靠人看屏）；
- 光标每次字符都 flush 一遍（4 条 `outb`），没有按需批量写入；
- **衰减是“按片计数、硬阈值”的简单版**：只有“连续跑满 4 片”这一种触发条件，
  没有按运行历史加权（真正的 MLFQ 会用指数衰减的权重算优先级）；
- 衰减只看 `hogs` 一个计数，`task_yield`/`task_sleep` 会把它清零，所以
  **“跑了 3 片然后睡一觉”能把“正在累加”的惩罚清零**——但已经降下去的 `base_prio`
  不会自己回来，要靠长睡回血。回血现在是“每睡满一个 `restore_ticks` 还一级”的
  倍数折算（睡 k 个单价还 k 级），但仍**没有“醒着也会慢慢恢复”的时间衰减**，
  也没有每级一个单价（比如高级回血慢、低级回血快）；
- 回血的单价是全局一个数，判的是**单次睡眠**的 `wake_tick` 差值：连续几次 200ms
  的短睡合计再久也不还（单价 300ms，单次没够）。这一条是有意的，但它也意味着
  “被反复打断的睡眠”永远拿不到回血；
- `sched_tune` 的校验到“范围 + 片长次序”为止：`slice[0] >= slice[1] >= slice[2]` 会被
  复查并回滚，但没有“改回默认值”的专用命令（只能自己再传一次默认值），也没有对
  多字段的组合约束（比如整体把片子都调成 1 让所有任务疯抢，这是合法的）；
- `arg` 的析构约定只管“谁调用 `dtor`”，不管 `dtor` 里能做什么：它在 `task_reclaim()` 的
  调用点上执行（`schedule()` 里是关中断的），不能假设自己跑在任务上下文、不能睡眠，
  也不该释放任务自己的栈页（那些已经在它之前还掉了）；
- 参数改动对**已经在跑**的任务是“下一次判定才生效”：已经装填好的 `left` 不会因为
  `slice` 改小而变短，`hogs` 累加到一半也不会因 `decay_quanta` 改小而立即降级；
- `verify.py` 对阶段四三个 `Y` 曾用 `1..8` 的单值下界，偶尔其中一个任务在两轮里一次
  都没轮到（`Y=0`）就误报；已改成“单个 ≤ 8 且三者之和 ∈ 5..32”，仍抓不住“两个任务
  长期饿死、第三个独吞”这种偏斜；
- 只有 3 个优先级，从最高级压到最低级只需要 8 片，惩罚幅度其实很粗；
- 老化用的是“排队 tick 计数”（每 tick 扫一遍全部槽位），槽位多了就该换成按入队
  时间戳排序；
- 时间片默认值 `{8,4,2}` 是很粗的拍脑袋值，没跟实际负载标定过；
- 时间片用尽就排回自己那一级的队尾，所以同一优先级内部是纯轮转，没有“最久未运行
  优先”之类的补偿；
- 主动让出有两个入口、语义不同：`task_sleep(ms)` 会真的离队（进睡眠队列），
  `task_yield()` 仍在就绪态、但把剩余时间片清零；两者都会把优先级降回
  `base_prio`，所以**老化提到的高级会被一次 yield 立刻清掉**——这一条是有意的：
  老化是“等太久了”的救济，主动让出本身就说明它没有等，借来的级别不该留着。
  降回动作收在 `schedule()` 一处（`task_yield` 里那句 `prio = base_prio` 只是让
  语义在让出点立刻可见），不再有两处各改一半的窗口；
- yield 循环会让切换频率变得很高（阶段四 12 个 tick 切了一万多次），当前没有
  “连续 yield 让出后再加一点惩罚”的机制；
- 就绪队列是每个优先级一条容量 5 的定长环形数组，`rq_push` 查重 / `rq_remove` 换队
  都是 O(N) 线性扫描；
- 睡眠队列是小顶堆：插入/出堆 O(log N)，但**没有 decrease-key，也没有任意删除**——
  只有“堆顶到期”这一种出堆方式。真要做超时取消（`task_wakeup(slot)`），得给每个
  槽位记它在堆里的下标，或者干脆加一层“惰性删除”标记；
- 堆的键是 `tasks[slot].wake_tick`，所以 `task_sleep` 必须在**入堆之前**把
  `wake_tick` 写好；顺序反了堆就按旧键排序。这类隐性约定目前只靠注释，没有断言
  守着（`sq order` 那条断言算是间接看守）；
- 任务槽固定 5 个（含 main），`task_spawn` 满了或没页时返回 0，没有等待/排队；
- 入口函数只能有一个 `void *` 参数，**没有返回值通道**；返回值想回传给创建者
  还得再加共享内存或消息队列；
- 任务之间没有父子关系：创建者退出后子任务照跑（本例 B 建 D、B 先退出），也没有
  `wait`/回收子任务的概念；
- 只回收任务栈页，任务控制块本身是静态数组，不随槽位增删；
- 最后一个退出任务的栈页要等 `main` 调 `task_reclaim()` 才归还（不能在自己还在用的
  栈上 `page_free`），这是主动让出机制的固有延迟；
- guard 页只守住“栈往下溢出”一种：往上（越过 `stack` 顶部）越界仍会静默写邻页；
  一个任务只有一页 guard，栈溢出超过 4KB 的落差才会被抓住（正常 `esp` 只往下走，
  一次最多溢出一页）；
- 栈与 guard 必须**物理连续**（`page_alloc_pair` 从低到高扫相邻两位），碎片一多就
  可能分配失败 → `task_spawn` 返回 0。这是“连续两页”方案的固有代价；
- guard 命中后只打一行、不带现场（`eip`/`err`/被写坏的栈内容），调试时仍需 gdb；
- `page_identity_unmap/restore` 依赖 head 建的恒等映射（0–16MB），一旦将来改成
  非恒等的高半区映射，这两条要跟着重写；
- 页目录已经**每任务一张**了（`mm/pgdir.c`：slot 1..4 各一套 PDE + 私有 PT0，
  `user_enter` 才装 CR3、`user_return_stub` 就切回内核表），所以“两个任务互相
  读不到对方的用户页”现在靠**表不同**成立。越权判据也已收紧成“用户态发起就
  交回内核”（`exception.c`，证据 `tools/pghigh.sh`）。写保护也已跟上：ELF 里没有
  `PF_W` 的页只给 `P|US`（清 `RW`），证据 `tools/wx.sh`；还剩一处：私有表页不做回收；
- 恒等映射的每条 PTE 原本都带 U/S=1；现在整张表（0–16MB）已被
  `page_lock_kernel(0, KERNEL_LOCK_END)` 一次性清掉 U，`knl=5505` 的前两位就是
  这条的证据（1MB 内 5、10MB 处也是 5），1MB–16MB 不再是绕过口；页目录私有化之后
  又加了一层：**任务自己的表里根本没有 16MB 以上**（`dir[4..1023]=0`）；
- 租借表只有 8 个静态槽、一句 `tick` 只递减一格：`ttl` 的单位是时钟 tick，
  所以租期最细到 10ms；槽满即 `open` 失败（计 `denied`），没有增长机制；
- 每任务借用表是**定长 4 格**、且不紧凑（`task_lease_forget` 只把一格清零、
  不搬动其它条目，为的是让回收循环按下标遍历时不会被搬动打乱）：借/还反复来几次，
  表里会出现“洞”，`task_lease_at` 必须能读懂 0 = 空；
- 借用表只记 `phys`，不记 ttl、不记 refs：同一 owner 同一页借两次仍然只有一条表项
  （refs 在租约槽里），所以“表满”是按页数算的，不是按借用次数算的；
- 回收 = 解约，但**解约不通知**“表里有、租约槽里已经没有”的那种脏条目，只静默划掉
  （正常情况下不会出现：`lease_drop` 已经先 `task_lease_forget` 了）；
- 租约记了 `owner`（内核 `0xFF` / 任务槽位号）和撤销回调，但**不记“借来干什么”**：
  出问题能看到“哪个 owner 还开着哪页”，看不到用途；
- 撤销通知是**同步回调**，在 `task_reclaim`（也就可能在时钟中断收尾的路径上）执行：
  回调里不能睡、不能 `spawn`、不能做会重启调度的动作，否则就是在调度器自己的
  收尾段里再切一次人。现在只有 demo 用它置一个标志，真要用得先定“回调允许做什么”；
- `PAGE_LEASE_BINDS` 只有 4 条、且是**覆盖式**登记（同一 owner 第二次 `bind` 直接替换）：
  一个任务只能有一个撤销回调，多路订阅必须自己分派；
- `page_lease_release_owner()` 只会收“已登记主人”的租约：一个任务若在死前把页
  `close` 掉，回收那一步就收不到东西——这是对的（页已关），但也意味着 `revoked`
  恒等于“忘了还的页数”，不能当成“所有借过页的页数”；
- `page_lease_tick()` 已挂进 `sched_tick`（真时钟中断），因此租约会**在运行期间
  自然老化**：这既是兜底，也意味着任何“我想精确控制它在第几 tick 过期”的演示都
  必须把 ttl 抬到远大于自检时长（当前 200），否则证据会被真中断提前改写；
- ring3 期间 IF=0（见 `user_enter` 的注释）：这段时间时钟中断进不来，是“先跑通”
  的取舍，不是最终形态；
- `exec` 的 ELF 支持是**最小**的：认 `PT_LOAD` + 搬段 + 清 `.bss`，没有重定位、
  没有动态链接、没有段权限（代码页和栈页都是 `U|W`，程序可以改自己的代码）、
  也没有“文件偏移 ≠ 虚拟地址”的重叠段支持（原地搬迁要求 `p_offset == p_vaddr`）；
- `exec` 一次最多 4 页（`EXEC_MAX_BYTES = 16KB`），装不下就返回 `-2`；
  多个段之间要求"vaddr 不重叠、程序头表落在文件第一页内"；
- `exec` 用户程序的出口现在有两条：`nr = 2`（sys_exit，带退出码）与 0xFF（老旁路，不带）；
- 系统调用**没有返回值通道**给"失败原因"：被拒一律返回 0，`0` 在四条契约里都不是合法值，
  所以"被拒"与"结果是 0"（比如 tick 真的是 0）只能靠约定区分；
- write 一次最多 63 字节、只放行可打印 ASCII 与 `\n`/`\t`；超长或含控制码**整条拒掉**，
  不截断也不替换——想打长文本得自己分多次；
- 用户窗口是一个**数字区间**，不是"哪几页"：exec 把 `[esp0-128, esp0)` 开出去，
  程序只要把指针挪到这一段里，就能读写自己的栈——它本来就能，窗口只是决定"内核愿不愿意看"；
- 窗口关得太晚（`cpu_sti()` 之后才关）：这一瞬间若有中断进来并切走，窗口仍是开的；
- `exec` 自己不再下潜，但**它仍然在等**：`while (!job.done) { task_reclaim(); cpu_hlt(); }`，
  所以调用者（shell 在主上下文里）是阻塞的；没有 `wait`/`waitpid`，也拿不到"别的任务"的退出码；
- 一次只有一趟 exec：`struct exec_job` 是**静态单槽**，窗口也是全局一个。并发 exec 需要
  每趟一个 job（堆分配）+ 每任务一个窗口；
- 跑 exec 的任务优先级固定 2（最高），时间片取默认；没有"按文件选择优先级"这回事；
- TSS 的 `esp0` 现在**跟着任务走**（每任务一页 `esp0_stack`，`schedule` 换人时写进 TSS），
  但 ring3 期间仍然 `IF=0`，所以"两个用户行程真正并发"还做不到：先要解决
  `boot/intr.asm` 的全局 `user_kernel_esp`、`kernel/tune.c` 的全局窗口、
  `exec_job` 静态单槽这三处全局单值；
- `exec` 失败只报 `E1..E5`，不带现场（哪个字节、哪一页），和 `cat` 的 `no such file`
  一样需要人再 `ls` 一遍。
- 调度点在时钟中断与 `int 0x30` 两处，都由中断门压现场，故只能在内核态切换。

## 下一步建议

1. **给回调定边界**：租约的账已经收齐了——`(owner, phys)` 双键、ttl 不互相续命、
   `sched_tick` 里老化、`task_reclaim` 里按**每任务借用表**逐页收回、撤销通知
   晚于 `dtor`（负向 `neg.sh -1 lease_owner` → `ls=1/0/0`、`neg.sh -1 notify_order` →
   `ls=1/1/0`、`neg.sh -1 lease_table` → `ls=4/0/0` + `lt=0x0/0x0`）。剩下的是回调的
   **执行语境**：它现在同步跑在 `task_reclaim` 里（也就是可能在时钟中断收尾段），
   不能睡、不能 `spawn`。下一步改成“回调只投递一个事件位，由 main 在安全点取走并处理”，
   让通知回归“只报告事实”；顺带把借用表从 4 格定长换成“按需追加”（现在 5 页就开始拒）；
2. **把 ring3 变可抢占**：`esp0`、用户页、`user_kernel_esp`、用户窗口、`exec_job` 五样
   现在都已按槽位分开（见「三处全局单值」「job 分槽」两节），剩下的只是 `main`
   （`cur==0`）那条路径刻意留着 `IF=0`；把 `schedule()` 的“死光就返回 `tasks[0].esp`”
   改成“返回本次 `ctx`”之后，两个用户行程才算真并发；
3. 死因已经能传到 `dtor` 了，但 `dtor` 还不知道“哪个槽位、跑了多久”：把回收交接
   扩成一个小结构（`{slot, reason, ticks, since}`），顺带让 `dtor` 能返回“要不再
   交还一次栈页”，并把回血单价做成每级一档（高级回血慢、低级回血快）；
3. guard 只守住了“栈往下溢出”这一种；栈页与 guard 页一旦被分配器拆散（现在靠
   `page_alloc_pair` 从低到高扫，碎片多了会分配失败），还得做页框连续性的回收整理；
4. 睡眠堆补 `task_wakeup(slot)` 与 decrease-key：现在只有“到期出堆”一条路，
   做不了“提前叫醒/取消睡眠”；
5. 补齐双故障（8）等其余异常，把 256 个门全部换成真实入口；键盘补 Ctrl/Alt 组合键
   （扩展码前缀状态机已经落地：`0xE0` 之后的一码按扩展码解释；输入队列已按消费者分环，
   见键盘章）
6. 分页细化：按 4KB 页属性区分内核只读/可写，替换“全部属性 7”的粗粒度映射；
7. 重载 CS 到 head 的 4GB 代码段，彻底解开段界限；
8. `printk` 支持最小格式化（`%s %d %x`），摆脱 `vga_print_hex` 手工拼接，
   顺便把光标 flush 改成 `printk` 收尾一次；
9. 产出带 DWARF 的 ELF，让 gdb 支持 `break main`、源码行级单步；
10. 键盘三条通路都收口了：**按键按消费者分环**（证据 `lane_*` + `neg.sh -1 kbd_lane`）、
    **提交是 FIFO**（证据 `subq_*` + `neg.sh -1 kbd_subq`）、**两者的深度都可配**
    （证据 `dep_*` + `neg.sh -1 kbd_depth`）；提交行的消费者也接上了——`shell_run()`
    有行协议/命令表/滚动输出，证据 `sh_*` + `kbd-shell.sh` + `neg.sh -1 kbd_shell`；
    看得见的出口也有了——**VGA 双页**（证据页 0xB8000 不动，用户页 0xB9000 给 shell，
    CRTC 切起始地址），证据 `console_start`/`con_chars` + `shell-console.sh` + `neg.sh -1 console`。
    下一步是让它有**状态**：提示符随行滚动、`clear` 之外的屏控（换页、翻历史），
    再配一个按 `runs`/`unknown` 自己调 `lane_depth` 的**自动策略**；
11. `kernel_idle()` 里定时器还在按 100Hz 空转（`sched_tick` 照跑）：空闲时应降频
    或干脆不记账，别让等事件的循环白烧 tick；
12. shell 已有 7 个内建命令 + `!!`、命令历史、变量表，持久态收进 `struct shell_state`
    并按 owner 分 4 槽（证据 `sh_rc_set`/`sh_var_val` 等 + `shell-vars.sh`）。
    下一步可以：① 把 `states[]` 从“按 tag 索引”换成“挂到 `task_t` 指针”（ring3 任务各自
    带一份、任务回收时一起释放）；② 加 `cd`/路径（要有“当前目录”这种真正的状态）；
    ③ 变量值里支持递归展开（现在故意不展开，避免死循环）。

至此调度器的“骨架”已经齐了：抢占、优先级、时间片、睡眠、动态创建、栈回收、
老化保底、主动让出、栈溢出隔离，每一条都有屏幕上的实测数字支撑；`make verify` 把这些数字
锁成了可重复的断言。再往下的分叉是**结构**（队列数据结构、MLFQ 衰减）与
**地址空间**（TSS/ring 3），前者是打磨，后者是换一代内核。

> 这张清单是当时写的。其中第 2 条后半（真并发）、第 6 条（页属性）后来都换过一刀：
> ring3 已经能进、`esp0` 与用户页已按槽位分开，页目录也做成了每任务一张
> （见本章最后一节「每任务页目录」）。第 6 条的**写保护**也已补上（`PF_W` 决定
> 页的 `RW`，见「用户页写保护」那节）——剩下的对称面是不可执行（NX），那要 PAE。

## 基础设施之后：下一步

串口、`%t` + 日志环、内核堆、ATA PIO、mosfs（含写/删/间接块）、`exec`（含 ELF 与
多页）、系统调用表都落地了，`make check` **18/18**。内核现在
能"看见自己"、能"读盘"、能"按名字读文件"、能"把读到的代码交给一个任务、在 ring3 跑完再收回来"，
也能让那个程序**开口说话、算数、自己退出**（`write` / `exit` / `ticks` 三个号）。

| 顺序 | 做什么 | 状态 | 已有地基 |
|---|---|---|---|
| 1 | **块设备**：ATA PIO 读扇区（`0x1F0`，LBA28） | **已完成**：`ata.c` + `disk` + `disk.sh`（认盘 0x800 扇区，LBA0/LBA1 与文件逐位一致） | 端口 I/O、串口 dump、`sec_buf` |
| 2 | **文件系统**：mosfs v1（超级块 + inode + 直接块 + 根目录缓存） | **已完成**：`mosfs.c` + `fs/ls/cat` + `fs.sh`（52/169/20 三个文件全对） | `ata_read`、`kalloc`、日志环 |
| 3 | **`exec`**：把盘上的裸二进制装进用户页，跑完回收 | **已完成**：`exec.c` + `exec` 命令 + `exec.sh`（magic 自证 `0x45584543`、val=36、两页收回、`E1` 分支不崩） | ring3 + 用户页租借 + `fs_read` + 调用门 |
| 4 | **写路径**：mosfs 的 `create/write`（数据块 → inode → 位图） | **已完成**：`fs_create`/`fs_write` + `write` 命令 + `fs2.sh`（关掉 QEMU 后从盘字节读回 `hi`/`big`，`528` 字节跨两块、位图两 bit 都置位） | 位图块 LBA 4、`ata_write` 0x30 |
| 5 | 分页细化：4KB 粒度区分内核只读/可写，替换"全部属性 7" | 排队中 | `paging.c` 的 `page_mark_user/kernel` |
| 6 | `kernel_idle()` 空闲降频（不再 100Hz 空烧 tick） | 排队中 | `sched_tick` 已是唯一入口 |

顺带的小账：日志环 67 列会把第 23 行截掉 42 个字符（`dropped=42`），要么把环放宽到
79 列（多占 32×12 = 384 字节），要么接受截断——目前选择接受，因为串口里有全量。

`exec` 之后下一个瓶颈已经不是"能不能跑"，而是"跑起来的程序能问内核要什么"：门口径
只有一个 `0xFF`，参数只有一个结构体指针，程序不能打印、不能要页、不能创建任务。
所以下一步的第一件事是**把调用门换成按号分派的系统调用表**（`eax` = 号，
`0x01 write` / `0x02 exit` / `0x03 ...`），第二件是**让 exec 走 `task_spawn`**：

| 顺序 | 做什么 | 为什么现在能做 |
|---|---|---|
| 1 | **系统调用表**：门从"一个 `0xFF`"扩成"号 + 指针参数"，指针校验按号细化 | **已完成**：`kernel/syscall.c` + `sys` 命令 + `exec.sh` 的 B/E 两段（`sy=2/0` 与 `sy=2/1`） |
| 2 | **exec 走 `task_spawn`**：用户代码页/栈页挂进 `task_t`，退出时随任务一起回收 | **已完成**：`exec_task_body` / `exec_task_dtor` + `t=1 d=0` 两格证据（入口 return、dtor 真被交接） |
| 3 | **写路径**：mosfs 的 `create/write` | **已完成**：`fs_create`/`fs_write`/`write` + `fs2.sh`（含宿主机读盘校验）；顺序是数据块→inode→位图 |
| 4 | **ELF 而不是裸二进制**：读 `e_entry` / `PT_LOAD`，不再靠"位置无关 + 从 0 开始" | **已完成**：`fs/elf.c` + `tools/uprog.ld` + `elf.sh`（`ep=0x0060` 等于 ELF 里的 `e_entry`，裸二进制仍 `ep=0x0000`） | `exec.c` 的装载点只有一处，换格式不影响上层 |
| 5 | **mosfs 间接块**：inode 第 5 槽 + 一块 128 个块号，单文件 2KB → 66KB | **已完成**：`blk_lookup`/`blk_ensure` + `fs4.sh`（3000 字节读得全、满 4 块后写第 5 块自动建间接块、`rm` 连间接块一起还） |
| 6 | **exec 多页**：一段连续代码页（1..4）+ `elf_span` 先算要几页 | **已完成**：`page_alloc_run`/`page_free_run` + `uprog2` + `pg.sh`（`exec pg=2`、`val=1834`、`pf=` 前后一致、20KB 走 `E2`） |
| 7 | **每任务一套用户页 + TSS.esp0 跟着任务走** | **已完成**：`task_t` 增 `esp0_stack/esp0/ucode/upages/ustack`、`task_user_offer` 交接、`release_pending` 统一回收 + `userpg.sh`（ring3 时 `CS=0x23`、`tss.esp0=0x26000` 且等于某槽位、收尾回 `0x16870`、`pf=` 不变） |
| 8 | 分页细化 + `kernel_idle()` 空闲降频 | 与 exec 无关的两笔老账 |




## 让 ring3 真能被时钟打断：把“全局单值”拆成“每槽位一份”

上一节把“每任务一套用户页 + `TSS.esp0` 跟着任务走”做完了，但任务里的 ring3 还
**不可抢占**：进用户态时 `eflags` 只留 bit1（`IF=0`），时钟进不来。要开 `IF=1`，
先得把“进 ring3 的现场”从一张全局格子里拆出来——否则任务 A 在 ring3 被时钟打断、
换到任务 B，B 也从 ring3 回来时，`user_return_stub` 换回的是 **A 的内核栈**。

三处全局单值，这轮拆了第一处：

| 单值 | 现在 | 为什么 |
|---|---|---|
| `user_kernel_esp`（`boot/intr.asm` 的 `.data`） | **`user_kret[TASK_SLOTS]`**，`current` 索引 | 一槽一份，换谁都不会串 |
| `tune_gate_user_window`（`kernel/tune.c` 转给 `sys_user_window`） | **`win_lo/win_hi[TASK_SLOTS]`**，按 `task_self()` 定位 | 两个任务同时下潜时，单值窗口会给错任务开门 |
| `struct exec_job job`（`fs/exec.c` 的静态作业块） | **`jobs[TASK_SLOTS]` + `busy`**，一趟占一格 | 两趟 exec 并发会互相覆盖 done 与结果区 |

改法（`boot/intr.asm`）：

```
user_enter:
     mov ecx, [cur]
     mov [user_kret + ecx * 4], esp   ; 现场归这个槽位
     ...                              ; cur==0 → eflags=0x002（`main` 的老行为）
                                      ; 否则   → eflags=0x202（任务里的 ring3 可被打断）
user_return_stub:
     mov eax, [cur]
     mov esp, [user_kret + eax * 4]   ; 换回**这个槽位**的内核栈
     pop ebp
     ret
```

### 一次 ABI 事故：`user_enter` 偷改 `%ebx`，把 `exec` 的证据写进了 IVT

改完 `user_enter` 后，**每一次 `exec` 都变成 `rc=E5 mag=0x00000000`**——包括最短的
`exec uprog`，而屏幕上的 `uprog:ring3 ok` 又说明 ring3 确实跑通了。

定位顺序（这套夹逼法值得留着）：

1. **先分清“写错了”还是“被后面清零了”**。在 `exec_task_body` 的三处写
   （`j->magic/value/exit`）**紧后面**加一个检查点，打到 `0x0` —— 说明不是“后来被谁
   清零”，而是**写的时候地址就是错的**。这一条把后面所有“谁覆盖了 `job`”的猜想
   （页表别名、`klog` 越界、`memset` 零页……）全部作废。
2. **gdb 硬件观察点这条路不划算**：`-S` + `watch *(unsigned int*)&job.magic` 只能在
   BIOS 阶段命中两次（`eip=0x93` 那两条是 BIOS 自己写这段内存），之后 gdb 的
   `continue` 一直报 `Cannot execute this command while the target is running`，
   等到 `timeout` 也没再命中；反过来“先注键再 attach”又因为 gdb 停在 boot 中途
   而抓不到。**结论：先用 serial + 检查点把窗口夹到几条指令，再只反汇编那一段。**
3. **读反汇编一眼看穿**：`objdump -d build/kernel.elf` 里
   `exec_task_body` 末尾是 `mov -0x18(%esi),%eax; mov %eax,0x14(%ebx)` —— `%ebx`
   从函数开头 `mov 0x14(%esp),%ebx`（= `&job`）之后**再没刷新过**，而 `user_enter`
   中间写了 `mov ebx, 0x00000202`。按 cdecl，`%ebx` 是**被调用者保存**寄存器：
   汇编函数偷改它，编译器不会为此做任何补救。

于是 ring3 回来后 `%ebx` 变成 `0x002`（`main`）或 `0x202`（任务），
`j->magic/value/exit` 被写到 `0x16 / 0x1A / 0x1E`（实模式 IVT 那一带），
真正的 `job` 一直是 0 → `magic != 0x45584543` → `rc=E5`。

修法：`user_enter` 只用**调用者保存**的 `eax/ecx/edx`，EFLAGS 先算在 `edx` 里再
`push`；`user_return_stub` 只动 `eax`。文件里也写上了这条约定——**asm 入口不许改
callee-saved 寄存器**，要改就先 `push`，并在 `user_return_stub` 里按相反顺序 `pop`。

### 实测

```
exec uprog rc=0 mag=0x45584543 val=36 ex=36 sy=2/0 t=1 d=0 n=1/0
exec u=2 lh=2 jb=0 ep=0x0060
```

`make check` **21/21**，`verify.sh` 两轮 `PASS`。新增两条：

- `tools/ring3pre.sh`——**ring3 可被时钟打断**的专项：`exec uprog3` 在用户态忙等时，
  一边采样 `CS` 一边读 `tasks[1].ticks`，实测 ring3 采样 7 次、槽位 1 的 tick
  从 43 涨到 60（17 个 tick ≈ 0.17s），程序仍按约定 `exit 9`。这是"`IF=1` 真的
  放行、且中断被记在**这个任务**账上"的唯一直接证据。
- `tools/probe-job.sh`（开机 → 注键 → 抓屏 → 按符号名 `xp` 一段内存），排查“某个
  静态量被写到什么值”时不必每次手拼 qemu 命令行。**造盘的参数顺序是**
  `img / uprog / bad / elf / big / paged / huge / uprog3`——慢程序在第 8 格，
  错位会变成“exec 一个不存在的名字”，症状是 `rc=E5 mag=0xDEAD0000`。

剩下的一笔账记在这里：`main`（`cur==0`）路径的 ring3 仍刻意保持 `IF=0`——它要可抢占，得先把
`schedule()` 里“任务死光就返回 `tasks[0].esp`”那条改成“返回本次 `ctx`”，
否则 `main` 的内核栈会有两张。


---

## 页归任务之后，租约也得归任务（否则 ttl 会在别人正跑着的页上收 U/S）

上一节把"全局单值"拆成"每槽位一份"，顺手放开了任务里 ring3 的 `IF`。这带来一个
当时没露头的隐患：`page_lease_open` 给内核替别人借的页定了一个 **ttl=500**，
语义是"我忘了关，你就替我收"。内核态不涨 tick 的时候它是无害的兜底；可 ring3
一放开 `IF`，**用户态跑得久一点，这个兜底就会在程序正跑着的页上把 U/S 收走**——
症状是"页还开着"和"页已经关了"混在同一个瞬间，从外面看就是随机的 `#PF`。

### 修法：交接（handoff）

页在 spawn 之前就已经归了任务（`task_user_offer`），账当时还挂在内核名下。
所以 `exec_run` 在拿到槽位之后补一步交接：

```c
/* 账也跟着页走：改成永不到期，回收的唯一触发点变成"槽位死了" */
for (i = 0; i < pages; i++)
    handed += page_lease_handoff(PAGE_LEASE_OWNER_KERNEL, slot,
                                 code + i * PAGE_SIZE_BYTES);
handed += page_lease_handoff(PAGE_LEASE_OWNER_KERNEL, slot, stack);
```

`page_lease_handoff(from, to, phys)` 只做三件事：先让接收方在**自己的借用表**里
记下这页（记不下就整笔留在原地，内核名下的 ttl 照旧兜底），再把 `owner` 改成槽位、
把 `ttl` 改成 `PAGE_LEASE_NO_TTL`（`0xFFFFFFFF`）。`page_lease_tick()` 见到这个值
直接 `continue`：**主人死了才有理由收，时间不是理由**。

收的路径没变，还是 `release_pending()` → `page_lease_release_owner(slot)`：
任务一死，U/S 跟着槽位一起收回。所以"页归谁"和"账归谁"现在是同一件事。

顺带加了两个可数量：`exec_last_handed()`（这一趟有几笔真的改记成功，shell 第二行
`lh=`）和 `page_lease_handed()`（累计笔数）。

### 证据

`tools/lease.sh`（新，已进 `check-all`，现共 **21 项**）：让 `uprog3` 在 ring3 忙等，
`tools/lease-watch.py` 在 `CS&3==3` 的每一次采样里一次性 dump 8 个租约槽的
`owner/phys/refs/ttl`，同时读 `tasks[1].ticks`：

```
RING 6
SLOT owner=1 phys=0x26000 ttl=0xffffffff samples=6 tick=0..8
VERDICT ok
PASS  租约跟着页一起归任务（ring3 期间 ttl 不动、任务死后 8 槽全空）
```

读法：代码页（`0x26000`）那笔账的主人已经是**槽位 1**，`ttl` 是 `0xFFFFFFFF`
且在 6 次 ring3 采样里一分没动，而同一段时间里这个槽位的 tick 从 0 涨到 8——
时钟确实在走，它就是不碰这页。跑完之后 8 个槽位全空（32 个字段零非零）。

回归：`exec.sh` / `pg.sh` 全绿，多页那一趟的屏幕第二行变成
`exec pg=2 u=3 lh=3 ep=0x0060`——三页都是交接成功的（`lh` = 交接笔数）。


---

## 布局契约：结构体偏移只有一个来源（`kernel/layout.c` + `build/layout.env`）

前面几轮加功能时，回归脚本一直靠「`nm` 找到符号 → 按**硬编码偏移**读字段」：
`task_t` 112 字节、`ticks` 在 +24、`esp0` 在 +96，这四个数字散在
`shell-tasks.sh` / `userpg.sh` / `ring3pre.sh` / `lease.sh` 里；`elf.sh` 还要
按整行正则去抠 `exec u=.. lh=.. ep=0x..`。（108 / +92 是加 `chan` 字段之前的旧值，
`layout.env` 现在自己会跟上，脚本不用手改。）

这套写法的代价是**静默失败**：结构体加一个字段，脚本读到的还是那个地址，
于是它测的是别人的内存，回归照样打印 PASS；打印格式加一个字段，正则匹配不上，
脚本报的却是"程序没跑对"——上一轮加 `lh=` 就把 `elf.sh` 撞坏过一次，就是这个味道。

### 做法

`kernel/layout.c` 只做一件事：把 `sizeof` 与 `offsetof` 编成一个只读数组，
放进**独立的 `.layout` 段**（链接脚本里单列一条规则，不受孤儿段摆放影响）：

```c
const unsigned int layout_words[] __attribute__((section(".layout"), used)) = {
    LAY_MAGIC,            /* 0x4C415941："LAYA"，段被挪走就报错 */
    sizeof(task_t),
    __builtin_offsetof(task_t, ticks),
    __builtin_offsetof(task_t, esp0),
    ...                   /* arg / dtor / state / death / prio / slice / ... */
    TASK_SLOTS, TASK_LEASES, TASK_PRIO_LEVELS,
};
```

构建期两步，都不执行任何代码、也不需要 debug 信息：

```
objcopy -O binary --only-section=.layout build/kernel.elf build/layout.bin
python3 tools/layout.py build/layout.bin > build/layout.env
```

`build/layout.env` 长这样（`make` 会自动生成并打印前三个键）：

```
TASK_SIZE=112
TASK_OFF_TICKS=24
TASK_OFF_ESP0=96
TASK_OFF_ESP0_STACK=92
TASK_OFF_ARG=48
TASK_OFF_KBD_TAG=88
TASK_SLOTS=5
TASK_LEASES=4
```

脚本顶部改成两行：

```bash
. build/layout.env 2>/dev/null || { echo "  FAIL  build/layout.env 不存在，先 make"; exit 1; }
off=$(printf '0x%x' $((0x$tasks_sym + TASK_SIZE * $1 + TASK_OFF_TICKS)))
```

`make image` / `make check` 都把 `layout.env` 列进依赖，`check-all.sh` 开头也
先确认它存在——**仪器没校准就直接失败**，而不是拿错位的地址跑出绿。

### 顺带修掉的第二处：屏幕断言不再按整行正则

`elf.sh` 取 `ep` 的方式改成「按空格切字段、再按 `k=v` 取值」：

```bash
exec_field() { printf '%s\n' "$1" | grep -E '^row +[0-9]+\| exec ' \
                 | tr ' ' '\n' | grep -m1 "^$2=" | sed "s/^$2=//"; }
ep=${...}          # 以后 exec 那一行再加字段，也不会撞坏
```

### 证据

`make` 之后 `layout.env` 的值与旧的手写数字完全一致（108 / 24 / 92），
四个改过偏移的脚本单跑全绿：

```
PASS  每任务 esp0：ring3 落在自己的任务页（0x26000）
PASS  ring3 可被时钟打断：忙等期间槽位 1 的 tick 涨了 18
PASS  租约跟着页一起归任务（ring3 期间 ttl 不动、任务死后 8 槽全空）
PASS  ELF 装载：e_entry 起跑（ep=0x0060）/ 指纹与结果自证 / 裸二进制老路径不变
```

内核因此多了 96 字节（79468 字节，`__kernel_end=0x18284`）——`layout_words`
本身进了映像，但它是只读数据，不参与任何运行逻辑。

### 附带修掉的一个"仪器 bug"：`nm | awk ... exit` 在 `pipefail` 下的 SIGPIPE

把布局契约接进来之后，`make check` 出现过一次 `FAIL: verify.sh rc=1`，现象是
`qemu-debug.sh` 的日志**0 字节**、退出码 **141**（128+13 = SIGPIPE）。

根因不在 QEMU，也不在 gdb，而是这两行：

```bash
ADDR=$(nm -n "$SYMS" | awk '$3=="kernel_idle"{print "0x"$1; exit}')
```

`awk` 里的 `exit` 会立刻关掉读端，此时 `nm` 往往还没写完——它拿到 SIGPIPE；
脚本开着 `set -o pipefail`，于是整条赋值的状态变成 141，`set -e` 直接把脚本带走，
**连第一行 "symbols : ..." 都没输出**（所以日志是空的）。

它为什么突然从"偶发"变成"一半"：`nm -n` 要写的行数跟内核体积正相关，这一轮
内核从 78956 涨到 79468，写完之前被打断的概率就跟着涨了。

修法两处：

- `awk` 不再 `exit`，改用标志位读完整条流（`&& !f {...; f=1}`）；
- 顺便把"探端口"从**真的连一下**改成看 listen 状态（`ss -ltnn`）。QEMU 的 gdbstub
  一次只服务一个客户端，`/dev/tcp` 探针自己就是一次连接，断开后 stub 还在收尾，
  紧接着 gdb 再连会被拒——这也是 `verify.sh` 历史上"偶发 qemu/gdb 运行失败"的一半原因。

改完连跑 8 次：**成功 8 / 失败 0**（改之前 6 次里错 2 次）。


---

## job 分槽：一趟 exec 占一格，用户窗口按任务分

上一节之后还留着两处"全局单值"，它们不会在单进程下出错，只在**两趟并发**下出错：

| 单值 | 出错的样子 |
|---|---|
| `static struct exec_job job`（`fs/exec.c`） | 两趟 exec 同时跑：后一趟把前一趟的 `done` 先置 1，前一趟提前醒来读到的 `magic/value` 是**别人写的** |
| `static unsigned long user_lo/user_hi`（`kernel/syscall.c`） | 两个任务同时下潜：A 注册窗口、B 注册窗口，A 的门解指针时用的是 **B 的区间**——等于给 A 开了后门 |

### 做法

**1. job 表按槽位开格，一趟占一格。** `struct exec_job` 尾部加 `int busy`，
`static struct exec_job jobs[TASK_SLOTS]`，`exec_run()` 线性找空格：

```
找不到空格 → n_fail++; return -7      /* 明确失败，不排队 */
找到     → last_job = ji; busy = 1;
           rc = exec_run_job(j, ...);  /* 原来的函数体整体改名搬家 */
           busy = 0; return rc;        /* busy 的清理只写这一处 */
```

`busy` 只在**一处**清，是因为真身里有十几条失败 `return`（页要不到、ELF 头不对、
指纹不匹配…）。每条都记得清一次，就等于"总有一天会漏一条"——漏掉一格以后所有
exec 都返回 `-7`，很难查。

证据接口（`exec_exit_code / exec_last_slot / exec_last_death / exec_last_entry`）
统一经 `last_job_ptr()` 取"最近一趟那一格"，没跑过就返回 `0 / TASK_DEATH_EXIT`，
不假装跑过。`exec_slots_used()` 数 busy 格，是"跑完没有残留"的可数证据。

**2. 用户窗口每槽位一份，按 `task_self()` 定位。** `win_lo[TASK_SLOTS] / win_hi[TASK_SLOTS]`，
`sys_user_window()` 写当前任务的格，`sys_ptr_ok()` 只认当前任务的区间。
`tune_gate_user_window()` 不用改——它本来就是转手给 `sys_user_window` 的。

**3. 加一个"关窗"出口。** 窗口是**扣在任务头上**的权限，任务一死就必须清，
否则槽位被复用时新任务会继承前任的窗口——那是一条永久后门。
所以新增 `sys_user_window_reset(slot)`，接在两处：

- `task_reset()` 的槽位清零循环里（每次换一组任务）；
- `schedule()` 的 `TASK_DEAD` 分支，紧挨着 `kbd_owner_detach()`（兜住
  guard 页杀和 `task_exit` 这两条走不到"正常关窗"的路径）。

### 顺手搬走一个宏

`TASK_SLOTS` 原来只在内部头 `include/linux/taskint.h` 里（"只给 task.c 与 queue.c 用"），
可 `fs/exec.c` 要给数组定长度。解法不是让 `fs/` 去 include `kernel/` 的内部头
——那是层级污染；而是把这个"公开容量"搬到 `include/linux/task.h`，和已经在那里的
`TASK_LEASES / TASK_DEATH_*` 放一起。`TASK_PRIO_LEVELS` 留在 `taskint.h`（它确实只给调度器看）。

### 证据

`shell` 第二行多了 `jb=`（`exec_slots_used()`），跑完必须回到 0：

```
exec uprog rc=0 mag=0x45584543 val=36 ex=36 sy=2/0 t=1 d=0 n=1/0
exec u=2 lh=2 jb=0 ep=0x0000
```

`jb=0` 是"这一趟把格子还回去了"的直接证据；`u=2 lh=2` 说明两页照旧全交接给任务。
`TASK_SIZE` 仍是 **108**（这轮没动 `task_t`，布局契约的数字一个没变），
`fs/exec.c` 的 job 块从 52 字节 ×1 变成 52 ×5 = 260 字节。

`make` 通过：kernel **79888 字节**、`__kernel_end=0x18518`、分配区余量 **31464**；
`bash tools/check-all.sh` **PASS 21 / FAIL 0**（含 `exec.sh`、`elf.sh`、`pg.sh`、
`userpg.sh`、`ring3pre.sh`、`lease.sh` 六项与 exec/窗口相关的专项）。

### 还没做的

`exec_run()` 仍然是"发起即阻塞等到 done"：分槽只解决了**同时只能跑一趟**的
数据串写，没解决"发起方得站着等"。要真并发得把它拆成 `exec_begin() / exec_reap()`
两段，让调用方拿着 job 号回去继续干别的——那一步的页归属已经收成了地址空间对象
（见下一节），剩下的只是"任务持那个指针"。


---

## 地址空间对象：五个散字段收成一格指针

上一节把 job 分了格，但 job 里还躺着五个**同生共死**的数字：

```
code / pages / stack / entry / esp0
```

它们描述的不是"这一趟的状态"，而是"这一趟的那套页"。散着放的代价在**交接**上：

- 页已经归任务了，job 里 `code` 还留着——外面看着像还归 exec；
- dtor 要逐个清四个字段，漏一个就是悬空指针；
- 要"发起后不等"时，调用方得背 5 个数字才能去 reap。

### 做法

**1. 新对象 `struct aspace`（`include/linux/aspace.h` + `mm/aspace.c`，共 100 行上下）。**

```
code / pages / stack / entry / esp0   这一套页
slot                                  交给哪个槽位（0 = 还没交）
live                                  池里这一格被占着
```

池子和 job 表同构：静态 `TASK_SLOTS` 格、线性找空格、用满明确失败
（`aspace_new()` 返回 0，不排队）。理由也一样——内核堆还没有"可回收对象"这一类，
而这类对象的最大数量本来就由槽位数封顶。

**2. job 只持一个指针。** `struct exec_job` 从 13 个字段缩到 9 个，
页相关的五样全部换成 `struct aspace *as`；`exec_stage()` 里写 `as->code = code;`，
`exec_task_body()` 里读 `as->esp0 / as->entry / as->stack`。

**3. 所有权只有两处，且互斥。**

| 结局 | 谁 free | 在哪 |
|---|---|---|
| 装配失败（读不到、页不够、ELF 装不下、建不起任务…） | `exec_run_job` | 唯一一处 `if (rc != 0) aspace_free(as);` |
| 成功（任务跑完 / 被 guard 杀掉） | `exec_task_dtor` | 任务死后由调度器在换好的栈上调用 |

这和 `busy` 的清理是同一个思路：失败返回点有十几条，就不会让每条自己记着还。
**只要有一处漏了，池子就少一格**，跑够 `TASK_SLOTS` 次之后所有 exec 都返回
`-3`，而且看不出是谁漏的——所以"可数"比"记得清"重要。

### 证据

`shell` 的 `exec` 第二行多了 `asu=`（池里还占着几格），和 `jb=`（job 表还占着几格）
是一对：两个都该回到 0。

```
exec uprog rc=0 mag=0x45584543 val=36 ex=36 sy=2/0 t=1 d=0 n=1/0
exec u=2 lh=2 jb=0 asu=0 ep=0x0000        ← 成功：对象已随任务死掉归还
```

失败路径同样不留东西（`exec nope` 在装配段就返回 `-1`，对象由包装层还回去）：

```
exec nope rc=E1 mag=0x00000000 val=0 ex=- sy=0/0 t=0 d=1 n=0/1
exec u=0 lh=0 jb=0 asu=0 ep=0x0000
```

- `make` 通过：kernel **80240 字节**、`__kernel_end=0x186C8`、分配区余量 **31016**（比上一步的 79888 多 352 字节：对象池 5×28 + 一层包装，句柄靠删掉一版没用的 getter 抵掉一部分）
- `TASK_SIZE` 仍是 **108**：`task_t` 这轮没动，布局契约的数字一个没变
- `bash tools/check-all.sh` → **PASS 21 / FAIL 0**

### 还没做的

- `task_t` 还不持 `aspace *`——现在是"exec 造、job 持、dtor 还"，任务自己只知道
  `ucode/upages/ustack` 三个数。要让任务真正"拥有"地址空间，得给 `task_t` 加一
  个指针字段，并把 `task_user_offer` 的三参数交接改成"交一个对象"。加字段会改
  `sizeof(task_t)`，但布局契约已经就位：`layout.c` 加一行、`layout.py` 加一行，
  四个回归脚本自动跟着走（它们用的都是 `TASK_SIZE * slot + TASK_OFF_*`）。
- 租约还没进对象：现在仍记在 `task_t` 的 `leases[4]` 里，而 `aspace.slot` 足以
  从对象找到那个槽位。等 `task_t` 持指针之后，这份账可以彻底搬进对象。
- `exec_begin() / exec_reap()` 的拆分还没做——有了对象，这一步只是把"装配段"
  和"等 + 收"切成两个函数，对象指针就是那一趟的句柄。

## 最小 libc：内核第一次有自己的 `memset`

### 为什么"没写过 memset"也会突然需要它

内核一直是 `-nostdlib`、没有 libc，以前也没事——因为没人写过需要它的循环。
但 `-O2 -ffreestanding` 下 gcc 会把下面这种代码**自己改写成函数调用**：

```c
for (i = 0; i < n; i++) buf[i] = 0;         /* → call memset */
for (i = 0; i < n; i++) dst[i] = src[i];    /* → call memcpy */
```

这不是"顺手优化"，是 gcc 认定"这两个函数语义固定、调用更省代码"。于是加一个
结构体清零、一段拷贝，链接期就会冒出 `undefined reference to memset`——而且
报错位置在 `ld`，看不出是哪个 `.c` 引出来的。`fs/exec.c` 要按 argv 布局摆字符串，
正好撞上这一条，所以 libc 是 argv 的前置，不是可选的美化。

### 做法

`include/linux/string.h` + `lib/string.c`，九个函数，全部用 `unsigned int` 做长度
（这个内核里没有 `size_t`，也不需要）：`memcpy / memset / memcmp / strlen /
strcmp / strncmp / strcpy / atoi`。故意**不做** `printf/vsprintf/malloc`：格式化
输出已经有 `printk`，格式化到缓冲区是另一件事（要可变参数 + 环形缓冲），现在
没人需要。

Makefile 两处：`SRC_DIRS` 加 `lib`、`C_OBJS` 尾加 `string.o`。

### 一个必须写显式规则的坑

`string.c` 不能和其他 `.c` 用同一条模式规则编，否则**自己调自己**：

```make
$(BUILD)/string.o: lib/string.c | $(BUILD)     # 显式规则优先于模式规则
	$(CC) $(CFLAGS) -fno-builtin -c $< -o $@
```

- `-fno-builtin`：不然 `memset` 里那句"按字节清零"的循环又被 gcc 换成
  `call memset`——函数体第一条指令就是 `call memset`，进去就栈溢出，而且症状
  是"一 memset 就三重错误"，跟 libc 这个词毫无关系；
- 显式规则：Make 的**模式规则**和显式规则同时命中时，显式规则赢，但两者并存
  的时候容易误以为改了 `CFLAGS` 就生效——只改 `CFLAGS` 变量不管用，必须在
  这条规则的命令行里加。

### 证据

`make` 通过，`lib/string.c` 出现在链接行的最后一项：

```
gcc ... -fno-builtin -c lib/string.c -o build/string.o
ld -m elf_i386 -T kernel/link.ld -o build/kernel.elf ... build/syscall.o build/string.o
```

`exec-argv.sh`、`fs*.sh`、`heap.sh` 全套照旧通过——说明新函数没有改变任何既有
路径的行为（它们只是把 gcc 隐式调用换成了显式实现）。

## panic / assert：出错只剩一个出口

以前"停机"是散在各个文件里的：异常处理里 `printk(...)` 之后 `kernel_halt()`，
别处也可能直接 `cli; hlt`。散着放的代价不是"多写几行"，而是**每个出口的说法
都不一样**：有的打向量号、有的打错误码、有的什么都不打，抓屏时得先猜这次是
哪条路停的。

### 做法

**1. `include/linux/panic.h` + `kernel/panic.c`。**

```c
void panic(const char *why) __attribute__((noreturn));   /* eip 取自调用点 */
#define ASSERT(cond, why)  ((cond) ? (void)0 : panic(why))
```

`panic()` 打一行 `PANIC: <原因> eip=0x........`，把 CRTC 切回第 0 页，然后
`kernel_halt()`。`eip` 用 `__builtin_return_address(0)`——没有调试符号也能拿
`objdump -d` 夹逼到出错的那几条指令。

**2. `ASSERT` 写成宏，不写成函数。** 这是有意的：包成函数之后
`__builtin_return_address(0)` 拿到的是 `assert` 自己的返回地址，等于每条断言
都报同一个 eip，恰好把最有用的那个数字废掉。

**3. `kernel/exception.c` 的不可恢复路径改成"打原因 + panic"。**

```c
printk("exception %s err=0x%08X\n", exception_name(vector), error);
panic(exception_name(vector));
```

`exception_name()` 本来就有（除零/GP/PF 都在里面），以前只用于向量表打印。现在
它是唯一的"事故名"，向量号换成名字之后，串口日志里一眼能看出是哪一类错。

**4. `fs/exec.c` 的 `exec_task_body` 开头加一条不变量断言**
`ASSERT(j && j->as, "exec: task body without aspace")`——任务体是在任务栈上
跑的，那里出问题（`j` 空、对象空）只会表现为"莫名三重错误"，断言把它变成一句
可读的话。

**5. `shell` 的 `panic <原因>`（写这一步时是第 22 条，后面 `date` 插在它前面）。**
不为用户准备，是为**测试**准备：
`panic` 平时永远跑不到，想验证"这条出口真的打日志 + 真的停住"，只能故意按一下。

### 证据

`tools/panic.sh`（注入 23 项回归里的一项）走三个通道：

```
已到 kernel_idle（EIP=0x000066d9）
证据页（panic 后 CRTC 指向的那一页）：PANIC: probe eip=0x0000BF9F
串口：PANIC: probe eip=0x0000BF9F
停机：EIP 两次读 = 0x000066c9 / 0x000066c9
PASS  panic：屏幕与串口都留下 PANIC 行，EIP 停在同一处不再走动
```

- 屏幕：读的是 **`0xb8000`（第 0 页）**，不是平时 shell 用的 `0xb9000`；
- 串口：机器已经 `hlt` 之后，串口文件是唯一还拿得到的记录；
- 停住：连读两次 EIP 完全相同——`hlt` 死循环，不是"跑到别处去了"。

### 第一版为什么屏幕上是空的

第一版 `panic()` 只调 `printk`。`printk` 写的是**证据页**（第 0 页），而运行期
屏幕上显示的是**用户页**（第 1 页）——所以 panic 行写对了地方但人看不见。
测试立刻抓住了这一点（`屏幕上没有 PANIC 行`），修法不是"再往用户页打一遍"
（那要经过 shell 的滚动缓冲，panic 时不该依赖 `con_row/con_col` 这种状态），
而是 `vga_page_select(0)`：机器要停了，最后一句话落在人正看得见的那块地方上。

## argv：参数怎么递到 ring3（不改调用门协议）

`exec` 到现在只能"跑起来"，程序不知道自己叫什么、也不知道调用者想让它干什么。
加参数最省事的写法是"压一个标准栈帧"，但这个内核里压不了——原因是**窗口太小**：

```
sys_ptr_ok 只认 [esp0-128, esp0)    共 128 字节
```

参数指针要是指到窗口外，程序连自己的名字都 `write` 不出去（每次 `write` 都会
被内核的指针校验打回）。所以布局必须**整块塞在这 128 字节里**，并且不能动
`user_enter`（改它就要重算 ring3 入口的寄存器，收益远小于风险）。

### 布局契约（写进 `include/linux/exec.h` 的注释）

```
[esp0-128 + i*24, +24)   argv[i] 字符串（23 字节 + NUL，最多 3 个）
[esp0-56  + i*4)         argv[i] 的绝对指针
[esp0-44]                argc
[esp0-24] / [esp0-20]    magic / value      （老协议，一个字没动）
[esp0-16]                {nr, a0, a1, a2}   （系统调用请求体）
```

三个常量：

```c
#define EXEC_ARGV_MAX     3      /* 128 字节装不下更多 */
#define EXEC_ARG_LEN      23
#define EXEC_ARG_STRIDE   24
```

程序取参数的方式：**用自己进 ring3 时的初始 `esp`**（那时 `esp == esp0`），
`argc = *(unsigned long *)(esp - 44)`，`argv[1] = *(unsigned long *)(esp - 52)`。
不需要新的系统调用，也不需要内核再"回填"一次。

### 做法

**1. `exec_build_args()`（`fs/exec.c`）。** 把 `argc/argv` 写成上面那张表：
超过 23 字节截断、没用到的格子写 0（避免程序读到上一趟残留）。调用点在
`exec_stage()` 里、spawn **之前**——那时栈页还归 exec 自己，写坏只脏自己的页；
spawn 之后再写就要过租约和窗口校验，是另一套代价。

**2. 三层逐层传递。** `exec_stage / exec_run_job / exec_run` 都加
`int argc, const char *const *argv`，公共签名变成：

```c
int exec_run(const char *name, int argc, const char *const *argv,
             unsigned long *out_magic, unsigned long *out_value);
```

**3. `shell` 的 `cmd_exec` 构造局部数组。**

```c
const char *av[EXEC_ARGV_MAX];      /* exec uprog a b → argc=3 */
```

`av[0]` 就是程序名（和 POSIX 一致），后面的词来自 `shell_argv_buf`。多过 3 个词
的部分被忽略——**不是**报错，因为"参数上限"是内核的布局决定的，shell 说了不算。

**4. 探针 `boot/uprog4.asm`（mosfs 名 `argvp`，`datadisk.py` 第 9 槽）。**
读 `[esp-44]` 的 argc → 写 magic / value；`argc>=2` 就取 `[esp-52]`（argv[1]），
否则取 `[esp-56]`（argv[0]）；手算长度后 `sys_write` 打出来；最后 `sys_exit(argc)`。
**两条独立通道互证**：`value` 走证据行（内核侧），字符串走屏幕（程序侧）。
只信其中一条，就可能把"内核写对了但程序读错了"当成成功。

### 证据

`tools/exec-argv.sh`（回归第 14 项）：

```
A. exec argvp rc=0 mag=0x45584543 val=2 ex=2
A2. 程序自己打出来的参数：hello
A3. 第二行：exec u=2 lh=2 jb=0 asu=0
B. 不带参数：exec argvp rc=0 mag=0x45584543 val=1 ex=1
B2. 这时打的是 argv[0]：argvp
PASS  参数：argv[1] 递到 ring3（val=2/ex=2、打出 hello），
      不带参数时 argc=1、argv[0]=程序名，job/地址空间池都归零
```

两个细节值得一提：

- `jb=0 asu=0` 在**带参数**的这一趟也要成立——参数是写在栈页里的，不额外占
  池子；要是实现成了"另造一个参数对象"，这两个数就会漏。
- 测试第一版把 `grep -x 'hello'` 直接套在 `vga-screen.py` 的输出上，永远匹配
  不到——那个脚本每行带 `row  N| ` 前缀。修法是先 `sed` 掉前缀再整行匹配，
  **别把"前缀"当成内容的一部分去断言**。

### 规模与回归（第一档做完时）

- kernel **81328 字节**、`__kernel_end=0x18B08`、分配区余量 **29944**
  （比地址空间对象那一步多 1088 字节：libc 全文 + `exec_build_args` + 一层参数传递；
  `panic.o` 只有几十字节）
- `TASK_SIZE` 仍 **108**、`sizeof(struct exec_job)` 仍 **36**：参数没有新增字段，
  全在栈页里
- `bash tools/check-all.sh` → **PASS 23 / FAIL 0**（新增 `exec-argv.sh`、`panic.sh`）

### 还没做的

- **`envp` 不做。** 128 字节的窗口里，3 个 argv 字符串 + 指针已经用掉 72 字节，
  再加环境变量就得改窗口大小（`EXEC_WINDOW`）——那是"改协议"，要单独一轮。
- 参数**只能 3 个、每个 23 字节**。真要放宽，正路是让程序和内核约定一个
  "参数块指针"，块本体放在栈页的另一处（比如 `esp0-4096` 起的一个页内区域），
  窗口只留一个指针——但那要求 `sys_ptr_ok` 认"栈页整页"，而不是现在这个 128
  字节的固定窗口。
- `exec` 仍不认 `#!`、不认路径分隔、不认"参数里带空格"（shell 的引号只在
  `shell_argv_buf` 层生效，`exec` 拿到的是切好的词）。

## 日历时间：`struct tm` + 纪元秒 + RTC 三件套

到这一步为止，内核的"时间"只有两个：`timer_ticks()`（开机以来多少个 tick）和
`rtc_seconds()`（CMOS 里那个 0..59 的秒计数）。两个都**不是日期**——它们能证明
"PIT 的频率对不对"，但答不了"今天几号"。日历 API 补的就是这个缺口。

### 分两层：纯算术与硬件读数

| 层 | 文件 | 依赖 | 特征 |
|---|---|---|---|
| 算术 | `include/linux/time.h` + `lib/time.c`（304 行） | 无 | 常量时间、无循环、可复现 |
| 硬件 | `kernel/chr_drv/rtc.c` 的 `rtc_now()` | CMOS 端口 | 会撕裂、会读到垃圾、可能失败 |

这样分是因为两层的失败模式完全不同：算术的错是**确定**的（错一天就是错一天，
测一遍就抓得住）；硬件的错是**概率**的（跨天那一下读到 `00:00:59`），只能靠
协议（双读比对）和显式失败返回值。

### `struct tm` 的两个故意不同

```c
struct tm {
    int sec, min, hour, mday, mon, year, wday, yday;
};
```

- `year` 是**完整年份**（2026），不是 C 库那种"距 1900 的年数"。那个约定唯一的
  效果是让人写错 `t->year + 1900`；
- 没有 `isdst`：没有时区就没有夏令时，留这个字段等于留一个永远填 0 的谎。

`mon` 仍是 0..11（和 C 库一致，和"人话"不一致）——这一点在头文件里写死，
打印时才 +1。

### 换算用一个 400 年周期，不用"按年累加"

`time_mktime()` / `time_gmtime()` 用的是 Howard Hinnant 的
`days_from_civil` / `civil_from_days`：

```
把 1、2 月算进上一年  →  以 3 月为年首的"日序"  →  400 年一个 era 的整数除法
```

好处是闰年规则被吸收进除法里，**没有一处 `if (is_leap)`**，2100 年这种"整百
不闰"的边界也不会漏。换算全程是乘除加减，没有循环。

### 错误只有一个通道：返回 -1

`int time_mktime(const struct tm *t, long *out)`：

- 月份越界 / 2 月 30 日 / 24 点 / 12 月 → -1；
- 超出 `long` 的表示范围 → -1。

为什么不用"返回 -1 当纪元秒"（Unix 的老做法）：纪元秒 -1 本身是合法时刻
（1969-12-31 23:59:59），用它当错误码，调用方就永远分不清"失败"和"差一秒"。

### 2038 边界踩到一个真坑

`2038-01-19 03:14:08` 的天数仍然是 24855（和 03:14:07 同一天），原来的
`if (days > 24855) return -1` 拦不住它，接着

```c
secs = days * 86400L + hour * 3600L + ...;   /* 有符号溢出 = UB */
```

在 i386 上直接回绕成 `-2147483648`，然后 `mktime` 返回 0（"成功"）。自检
第 8 条就是这样抓到的（`n=20 ok=19`）。修法：

```c
unsigned long secs = (unsigned long)days * 86400UL + ...;
if (secs > 2147483647UL) return -1;
```

先在有符号安全的地方（unsigned）算完，再判一次上限。**"天数合法"不等于
"秒数合法"**，这两层边界必须分开判。

### RTC：为什么要整块双读

`rtc_now()` 读七个寄存器（秒/分/时/日/月/年/世纪）。撕裂的后果比"秒计数"
严重：23:59:59 之后读到 `00:00:59`（秒换了、分没换）能拼出一个合法但错误的
时刻，**事后没有任何检查能发现**。所以：

1. 等 UIP 清零 → 整块读；
2. 再等 UIP 清零 → 再整块读；
3. 七个字节逐个相同才认，连续 4 次不一致返回 -1（宁可不给时间，也不给错时间）。

三个换算在建驱动里就做掉，上层不知道寄存器长什么样：

- **BCD/二进制**：看 reg B 的 bit2；
- **12/24 小时**：看 reg B 的 bit1，12 小时制下 hour 的 bit7 是"下午"，
  12 点这个分界点单独处理（上午 12 点 = 0 点）；
- **世纪**：读 0x32。很多实现没有这个寄存器、返回 0xFF，那就按 `20xx` 解释——
  这是明确的取舍，注释里写了"这里是猜的"。

最后把 `struct tm` 交给 `time_mktime()` 验合法性（2 月 30 日、25 点这种由
仿真器/固件填出来的垃圾在这里被挡掉），再由 `time_gmtime()` 回填 `wday/yday`：
保证"同一个时刻只有一种表示"。

### shell 的三个入口

```
date              读 RTC：date 2026-11-22 07:30:39 Sun yd=325 ep=1795332639
date <纪元秒>      纯算术：utc 2024-02-29 12:34:56 Thu yd=059 ep=1709210096
date chk          自检：  date chk n=20 ok=20
```

`date chk` 做成命令而不是"启动时打一行"，理由和 `panic` 那条命令一样：
自检的价值在于**能单独按一次**，而启动屏 24 行是按行算死的。

自检的 20 条分十组，其中最有价值的不是"手算期望值"，而是第 6 组：

```c
for (i = 0; i < 200; i++) {                       /* 200 个时刻，间隔 13337 秒 */
    time_gmtime(probe, &back) → time_mktime(&back) == probe ?
}
```

往返一致性能在**不看代码**的前提下抓出取整错误（`%` 对负数、`/` 的借位、
`yday` 的 0/1 起）。剩下的组覆盖：闰年三态（2000/2024 闰、2100/1900 不闰）、
2 月月长、两个锚点（0 与 946684800）、2038 最后一秒与越界、非法字段三连、
ISO 定宽补零、名字表越界。

### 证据

`tools/date.sh`（回归第 23 项）：

```
A. date chk n=20 ok=20
B. ep=0            内核：1970-01-01 00:00:00 Thu  yd=000   宿主：同       yd=000
B. ep=1709210096   内核：2024-02-29 12:34:56 Thu  yd=059   宿主：同       yd=059
B. ep=2147483647   内核：2038-01-19 03:14:07 Tue  yd=018   宿主：同       yd=018
C. 越界：date bad epoch (0..2147483647)
D. date 2026-11-22 07:30:39 Sun yd=325 ep=1795332639
D2. 与宿主机 UTC 相差 3s（允许 ±180s）
D3. 内核换算：2026-11-22 07:30:39 Sun  用 GNU date 复核同一 epoch：一致
```

- B 段**不和写死的字符串比**，而是和宿主机 `date -u -d @ep` 的实时输出比：
  期望值写错时，测试会跟着一起错，这种自证是假的；
- D 段是唯一依赖硬件的：QEMU 的 RTC 就是宿主时钟，所以"相差 3s"是起机这几秒
  的真实开销；再用同一 epoch 让 GNU date 复核内核自己算的年月日，是**自洽性**
  证据（内核给的数字能不能互相解释），不是"和外部一致"的证据——两件事分开说。
- 测试第一版用 `date -u ... +%a` 和内核比星期，结果宿主机 locale 打的是
  `四/二/日`，四条全红。修法是 `LC_ALL=C`：**断言要和 locale 解耦**。

### 规模与回归

- kernel **87536 字节**、`__kernel_end=0x1A348`、分配区余量 **23736**
  （比 `argv` 那一步多 6208 字节：自检 20 条 + 换算两条 + shell 的 `date`；
  `time.c` 单独编进来是 304 行）
- `TASK_SIZE` 仍 **108**
- `bash tools/check-all.sh` → **PASS 24 / FAIL 0**（新增 `date.sh`）

### 还没做的

- **时区与夏令时**：现在一律 UTC。要做本地时间，需要一张"偏移表 + 切换规则"，
  而规则本身（哪年哪月改点）是要维护的数据，不是几行代码；
- **闰秒**：`sec` 只收 0..59，60 会被 `mktime` 拒绝。真需要的话得先决定
  "纪元秒要不要表示闰秒"——Unix 的答案是"不表示，用表抹平"；
- **时间不是单调的**：RTC 只给"现在"，NTP 式校准（多次采样、平滑修正、
  与 `timer_ticks()` 的漂移对齐）没有做。现在两个时间源各走各的：
  `date` 读 RTC，`stat` 读 tick，两者之间没有换算关系；
- **格式化器还不认 `%02u`**：`printk` 的宽度只用于十六进制。`date` 的输出是
  shell 手拼的（`u3` 这类小函数），要把它推广到 `printk` 得改格式化器语义，
  牵动所有既有输出——不值得在"加一个 API"这一轮里做；
- **没有 `ctime`/`strftime`**：只有 `time_iso()` 一种固定格式。要可配置格式，
  得先有"格式化器 + 模式串"这一层，属于独立的一轮。

## 两路时间对齐：tick 负责走时，RTC 负责纠正

上一节给了"现在几点"，但内核里其实是两套时间，各说各话：

| 来源 | 有绝对意义 | 单调 | 代价 |
|---|---|---|---|
| `timer_ticks()` | 否（只是"开机后第几个 tick"） | 是 | 几乎为零（读一个变量） |
| `rtc_now()` | 是（年月日时分秒） | 否（可能撕裂） | 毫秒级忙等 + 逻辑分析仪级别的运气 |

`date` 走 RTC、`stat` 走 tick，两个数字之间没有任何换算关系——于是答不了
"这台机器跑了多久"、"两条日志谁先谁后"这类问题。`kernel/clock.c` 就是把它们绑成
一个东西：**开机用 RTC 定一次锚，之后靠 tick 走时，再定期用 RTC 把走时拉回来**。

### 结构：一个锚点 + 一条修正规则

```
base_epoch / base_tick        开机那一刻的"绝对时间 + tick"
clock_epoch()   = base_epoch + (timer_ticks() - base_tick) / TIMER_HZ
clock_resync()  读一次 RTC，差多少就把 base_epoch 挪多少（封顶 60s）
clock_poll()    idle 循环里每 30000 tick（300s）自动校准一次
```

三个细节是这轮真正花时间的部分：

**1. 走时用无符号减法。** `timer_ticks()` 是 32 位、约 497 天回绕一次。
`now - base_tick` 用 `unsigned long` 算，回绕后自动得到"正确的间隔"；
要是用有符号减，回绕那一刻时间会往回跳 497 天。

**2. 两条性质分开承诺，不混着说。**

```
timer_uptime_ms()  单调不减，与"几点"无关  → 日志排序、超时判断
clock_epoch()      绝对、可被校准（允许回拨）→ "现在几点"
```

把"绝对时间"和"单调性"塞进同一个数，是所有时间 bug 的祖宗：要么为了单调
永远不校准（走时越来越偏），要么为了校准把单调性弄丢（`sleep(10)` 变成
`sleep(-3)`）。这里干脆不合并——要用哪个就问哪个。

**3. 校准放在 idle，不放在时钟中断里。** 读 CMOS 要等 UIP 清零（最长一个
更新周期），那是毫秒级忙等；在 IRQ0 里做，会把 10ms 的片长吃掉一截，
调度器的证据（tick 差值）当场就变。放在空闲循环里，代价只是"这次唤醒多花了 2ms"。

### 封顶：拒绝比照抄安全

`CLOCK_MAX_STEP = 60` 秒。差值超过 60 秒不采纳，只记 `clock_rejects()`。
理由：RTC 读数一旦是垃圾（世纪寄存器缺失、年份错位），差值是几亿秒；
照抄就等于把系统时间扔到 1970 或 2100 年，而且**之后每次校准都会"修"成那个年代**。
拒绝的代价只是"继续用 tick 走时"，而它至少还在正确的量级上。

### shell 的两个口子

```
clock              clk ep=1795334330 ms=950 t=0x000BA7 d=+0 s=0 r=0 x=0
clock sync         clk sync rc=0 d=+21 s=2 r=0
clock skew <秒>    测试口：把误差设成 <秒> 秒（`clock skew 0` 清零）
```

- `ms` 是这一秒已经走了多少毫秒（0..990，10ms 一档）——100Hz 的量化在这里
  看得见，也是"走时是按 tick 算的"的直接证据；
- `d` 是最近一次 `RTC - 预测`：正的说明 tick 走慢了（TCG 下常见），
  负的说明走快了；
- `s / r / x` 分别是成功校准、越界拒绝、历史最大差值的计数。

`clock skew` 是**故意的测试钩子**，和 `panic` 那条命令同一类：校准路径平时
多半差值为 0（这台机器上 tick 常常正好跟得上真时间），"它到底会不会把错的
基准拉回来、会不会拒绝离谱的差值"就没有证据。用 skew 把误差做成确定的 20s / 90s，
两条分支才都可测——**否则测试就成了"漂移恰好发生时才有意义"的碰运气**。

### 证据

`tools/clock.sh`（回归第 24 项）：

```
A. date 2026-11-22 07:58:29 Sun  ep=1795334309  宿主=1795334311  差=-2s
B. 12s 之后：ep 涨了 17s，宿主涨了 17s
C. clk ep=1795334330 ms=950 t=0x000BA7 d=+0 s=0 r=0 x=0
D. clk sync rc=0 d=+0 s=1 r=0        （欠账 0s，因为这次 tick 正好跟得上）
G. clk skew rc=0 err=20；实测差 23s → sync d=+21 rc=0 → 拉回后差 3s
H. clk sync rc=E2 d=+90 r=1；拒绝后仍差 93s → clock skew 0 → sync rc=0 → 差 3s
PASS  两路时间：锚点差 ≤180s、走时由 tick 驱动、skew20 被拉回、skew90 被拒、年月日自洽
```

G/H 是这轮最值钱的两段：它们不依赖"漂移刚好发生"，而是**把误差做成输入**，
于是"校准真能修"与"封顶真的拦得住"各有一次确定性证据。

### 踩的坑

**测试输入里的 `-90` 根本没打进去。** 第一版用 `clock skew -90` 收尾，结果
误差从 90 变 180——因为 QEMU `sendkey` 的减号叫 `minus`，脚本把 `-` 当键名
传过去被静默丢弃，实际打进的是 `90`。两个修法一起用了：

1. 把 `clock skew` 的语义从"挪 N 秒"改成"**把误差设成 N 秒**"，收尾用
   `clock skew 0`，测试里不再需要输入负数；
2. 测试脚本里显式做字符→键名映射（空格 `spc`、`-` `minus`、`.` `dot`），
   别指望"字符就是键名"这件事永远成立。

顺带两个断言上的小坑：`sed` 的 BRE 里分组要写 `\(...\)`（不然 `\1` 是非法引用，
而报错只出现在 stderr，脚本看起来只是"没读到"）；取 `s=` 字段要带前面的空格
（否则 `ms=950` 会被当成 `s=950`）。

### 规模与回归

- kernel **90288 字节**、`__kernel_end=0x1AE28`、分配区余量 **20952**
  （比日历那一步多 2752 字节：`clock.c` 177 行 + shell 的 `clock` 命令）
- `TASK_SIZE` 仍 **108**
- `bash tools/check-all.sh` → **PASS 25 / FAIL 0**（那一轮是 25 项；加 `fd.sh` 之后共 26 项）

### 还没做的

- **平滑校准（slew）**：现在是"步进"——差值一次补上去。真正的 NTP 会把修正
  摊在几秒里慢慢加，避免时间出现跳变；这套机制要一个"每秒该补多少毫秒"的
  调节器，跟调度器的 tick 挂钩，属于独立一轮；
- **自动校准周期是固定的 300s**：漂移率变化时它不会自适应。真做起来要看
  "上次差值的斜率"（`d` 的历史），而不是只数 tick；
- **`date` 与 `stat` 的口径还没统一**：`stat` 仍然是 tick 口径（`up=` 是
  `timer_uptime_ms()`），而 `date` 是校准后的绝对时间。要做到"日志里一行内
  同时有 uptime 和墙钟"，得让 `printk` 的 `%t` 一起切到 `clock` 口径——那是
  一次会影响所有既有证据行的改动，需要单独回归；
- **时间源只有一个 RTC**：没有 NTP、没有"用 tick 反推 RTC 是否可信"的交叉
  判断。现在 RTC 说什么就是什么（除了封顶那一条）。

## fd 层：把 (ino, offset) 收成一个整数

在 fd 之前，读文件只有一种姿势：调用方自己拿着 `ino` 和 `offset`，每读一段
自己加偏移。shell 里那没问题（一个 `for` 循环够了），但对 ring3 说不通：

- 用户程序不该知道 inode 号——那是盘上的内部编号，知道了也只能干坏事；
- "读到哪儿了"这个状态不能放在最不可信的地址空间里。用户程序随时可以把它
 改成任意值，而内核每一趟都要重新校验（校验成本比状态本身还高）。

`fs/fd.c` 就是这个状态的家：**每槽位一张表**（和 `tasks[]` 一一对应），
每个格子放 `{ 名字, ino, size, offset, 读/写标志 }`。用户程序只拿得到下标
（0..3），所有真实动作仍由 `fs_*` 完成。这一层不碰盘，只记账、判边界。

### 三条不变量

```
1. 一个 fd 只归一个槽位          拿别人的 fd 号 → FD_E_BADFD，那一格根本不解引用
2. 任务死时整表清空             fd_close_all(slot) 接在 sys_user_window_reset 旁边
3. 读 size 是 open 的快照；写每写一次刷新
```

第 1 条是"fd 也是访问控制对象"的那半条：如果 `fd_get` 只按下标查表，那么
槽位 1 的程序传一个 2 就能读到槽位 0 打开的文件（以及槽位 0 读到的位置），
而没有任何一处会报错。所以查表必须同时用 `slot` 和 `fd` 两个键。

第 3 条两条语义不同是**故意的**：读端看到的边界在 `open` 时就定了（一致视图，
POSIX 的 `fstat` 语义），写端则必须跟着 inode 走——不刷的话下一次 `read` 会
以为还在旧末尾，读回 0 字节，现象是"写进去了但读不出来"。

### 返回值的约定

`open/read/write/close` 的成功值不同，失败值统一：门返回 `SYS_ERR`
（`0xFFFFFFFF`）。为什么不返回具体负码——用户程序拿不到 `-3` 和 `-5` 的语义
差别，能做的判断只有"成没成"。具体错在哪看内核侧：`fd_denied()` 的计数、
`fd` 命令打出来的表，以及失败那一格是空着的。

### 窗口从 128 扩到 256

`sys_ptr_ok` 只认窗口内的指针。fd 的 `read/write` 要把数据落在**用户自己的
内存**里，而原来的 128 字节已经被 argv 占满（`[esp0-128, esp0-56)` 字符串、
`[esp0-56, esp0-44)` 指针、`[esp0-44]` argc、`[esp0-24]` magic、`[esp0-16]` 请求
结构体），没有一段 64 字节以上的连续空位。

于是窗口扩到 `[esp0-256, esp0)`，新多出来的 128 字节
（`[esp0-256, esp0-128)`）就是 I/O 缓冲，常量写在 `include/linux/exec.h`：

```
EXEC_WINDOW   256
EXEC_IO_OFF   256    /* 相对 esp0 的负偏移 */
EXEC_IO_BYTES 128
```

**扩窗不等于放宽信任边界**：多出来的那 128 字节仍在同一个用户栈页
（页基 = `esp0 - 4096`），窗口变大只是"允许传的范围"从半页变成半页多一点，
没有一个字节落到别处。

### 限制属于哪个边界

第一版把"一次最多 64 字节"写进了 fd 层（`FD_IO_MAX 64`），结果 `fs.sh` 当场红：

```
G. fs ok=0x1 b=2048 i=32 rd=18 er=0
FAIL  rd=18（挂载+读三个文件应该只有十几次读块）
```

原因：内核的 `cat` 一个 169 字节的文件原本读 1 趟，被 fd 层切成 3 趟，
而 `fs_read` 每趟都要重读一遍 inode 块——单块缓存省下的读全被这次拆分吃回去了。

修法不是把断言放宽到 18，而是把上限搬回它该在的边界上：

```
fd 层        上限 = FS_MAX_BYTES（一个文件的全部）
syscall 边界  上限 = SYS_FD_IO_MAX 64（用户缓冲只有窗口里那 128 字节）
```

内核的 `cat` 现在传 256，读块次数回到 14。**限制放错边界，代价不是"慢一点"，
而是把一条已经成立的不变量（单块缓存真的在省读）改坏了还不知道。**

### 生命周期

`fd_close_all(slot)` 挂在与 `sys_user_window_reset(slot)` 相同的两个位置：

- `task_reset()`（整张任务表重来）；
- `schedule()` 的 `TASK_DEAD` 分支（任务死、槽位要还给池子）。

这两个位置是"槽位复用时必须把私有资源收干净"的唯一执行点。正常路径上程序
自己 `close` 过，但"被 guard 杀掉 / 被 exit 带走"两条走不到那句——不收的话
下一个任务拿到的 fd 号带着**上一个任务的读进度**，那不是泄漏，是把别人的
状态当成自己的起点。

### 证据

`tools/fd.sh`（回归第 12 项，插在 `fs.sh` 之后）：

```
A. 初始表：fd s0=0 s1=0 s2=0 s3=0 s4=0 o=0 on=0 rd=0 wr=0 dn=0
B. cat 走 fd 读出的第一行：hello from mosfs v1
C. fd open rc=0 fd=0 ino=2 sz=52 / fd read n=16 off=16 / fd close rc=0
D. exec fdprobe rc=0 mag=0x45584543 val=16 ex=16
D2. ring3 read 回来的内容：hello from mosfs
E. cat 用户程序写的 fdout：fdwr
F. 收工后的表：fd s0=0 s1=0 s2=0 s3=0 s4=0 o=5 on=0 rd=89 wr=5 dn=0
PASS  fd 层：内核与 ring3 共用一条路径（open 得 fd=0、read 后 off=16、
      ring3 读回 16 字节并写出 fdout、任务死后整表归零）
```

几个数字的含义值得单独说：

- **C 的 `off=16`**：同一趟里 open 之后 off 从 0 走到 16。这是"fd 真有状态"
  的直接证据——没有它，fd 就只是 inode 的别名；
- **D 的 `val=16 ex=16`**：`boot/uprog5.asm` 在 ring3 里 `open("hello.txt")` →
  读 16 字节 → 用**控制台 write** 打出来 → `exit(16)`。屏幕上那行
  `hello from mosfs` 是真内容，不是内核代打的；
- **E**：同一趟程序还 `open("fdout", 1)` + `fwrite("fdwr\n")`，
  之后内核的 `cat fdout` 读回 `fdwr`——**两个方向都通了**，而且落盘的是真字节；
- **F 的 `on=0`**：任务死后整表归零。`o=5` 是这一趟累计开过的次数
  （cat 1 + fd 1 + 探针 2 + cat fdout 1），`rd=89` = 52 + 16 + 16 + 5，
  `wr=5` 就是探针那 5 个字节；`dn=0` 说明这一趟没有一次拒绝
  （负向分支另有 `exec bad` 那条在 `exec.sh` 里）。

### 规模与回归

- kernel **94000 字节**、`__kernel_end=0x1BFA8`、分配区余量 **16472**
  （比上一版多 3712 字节：`fd.c` 272 行 + shell 的 `fd` 命令 + syscall 四条分支）
- `TASK_SIZE` 仍 **108**（fd 表是静态 `.bss`，没进任务结构）
- shell 命令 **25** 条（加 `fd`）
- `bash tools/check-all.sh` → **PASS 26 / FAIL 0**

### 还没做的

- **没有目录**：mosfs v1 只有根目录，`fs_lookup` 只认一级名字。fd 层的
  `open` 因此也只认一级——要做路径就得先有目录项的类型语义与 `..`；
- **没有 `dup`/`dup2`/继承**：`fork` 不存在，所以"子进程继承父进程的 fd"
  这个问题还没有被提出来；`exec` 也不继承（每次都是新任务、空表）；
- **删除打开的文件**：`rm` 只清目录项 + inode + 位图，不管有没有 fd 指着它。
  现在不会出错（`fs_read` 会因 inode 归零而失败），但语义是"下次读报 EIO"
  而不是 POSIX 的"最后一个 fd 关掉才真正释放"；
- **没有截断/覆盖写**：mosfs v1 只有追加，`fd_write` 的 `off` 因此是"写到尾部"
  的同义词。真做覆盖写要动块号表（那是下一版的事）；
- **fd 表没有可回收性**：`FD_MAX 4` 是编译期常量，静态表落 `.bss`。
  动态表要 `kmalloc`，而"对象的生存期"这个问题现在只在页和 aspace 上回答过。



## 每任务页目录：隔离的单位从"页"变成"表"

在这一版之前，隔离的单位是**单个页的 U/S 位**：`page_mark_user` 把某一页
对 ring3 开，`lease` 管"谁在用、什么时候收"。看起来够用，其实是假隔离。
`boot/head.asm:211` 只设过一次 `mov cr3, eax`，此后**五个槽位、所有任务共用
同一张页目录**。于是：

```
槽位 1 的程序正在跑   → hello.txt 的数据页被 page_mark_user 打开
槽位 3 同时进入内核   → exec 给它装代码页，又把另一页打开
槽位 1 还没退出     → 它现在已经能读槽位 3 的代码页了
```

`page_lock_kernel(0, KERNEL_LOCK_END)` 在启动末尾把 16MB 恒等映射的 US 全清，
所以"用户程序读内核内存"这条**本来就没成立**。真正的窟窿是**任务之间**：
一张表上的 US 位是全局的，"给 A 开"必然"对 B 也开"。

修法的方向因此不是"再加几道检查"，而是**让每个任务有自己的表**。

### 交换什么

```
每槽位私有          页目录（PDE 那张，1 页）
                    私有 PT0（0..4MB 那张，1 页）
共享（抄内核 PDE）    dir[1..3] = 内核表 dir[1..3]（4MB..16MB 恒等映射）
不映射              dir[4..1023] = 0
```

`dir[0]` 指向私有 PT0，里面 0..4MB 的 identity 映射全在，US 清 0。用户页
按需在 `pgdir_set_user(slot, va, 1)` 里打开——写的是**这个槽位自己的 PT0**，
`invlpg` 刷掉一条 TLB 项。

`dir[4..1023] = 0` 是刻意的取舍：不抄内核表的高半部分，就**没有**
"用户碰一下 32MB 以上的地址、`page_fault_map` 顺手给内核补一页"这条路。
代价是这些任务的地址空间只有 0..16MB（够用：用户页全在 0..4MB、
页表在 0x20000..0x90000），换来的是内核高半区**压根不在用户表里**。
按需分页那条 4MB 窗口（PDE 8）仍然在**内核表**上，只有内核态看得见。

### slot 0 为什么不建表

槽位 0 是主任务（跑 `main` 里那段演示 + 落到 shell 的那个 idle 循环）。
它从不 `user_enter`，也从来没有用户页。给它一张表等于：

- 每次调度到 shell 都要切一次 CR3（白付 TLB 全刷的代价）；
- 它的表要和内核表**永远保持同步**（内核改一条 PDE 得改两处），
  这是最容易漏的地方。

于是定下契约：`pgd_tab[0] = 0` 表示**不切**。asm 侧看到 0 就跳过 `mov cr3`，
`pgdir_set_user(0, ...)` 退化成老的 `page_mark_user`——`main` 的老路径
一个字节都没变。

### CR3 只在这四处改

```
user_enter        进 ring3 之前   装 pgd_tab[slot]，同时记 cr3_tab[slot] = 装入值
user_return_stub  回内核那一刻   把 cr3_tab[slot] 清 0，切回内核表
task_die          死因记录之后    pgdir_kernel()，先回内核表再让出
schedule          改 cur 之前    pgdir_switch(cur, next)
```

两条容易漏的点：

1. **保存切出者的 CR3 时要读 CR3，不能查表。** `pgdir_switch(from, to)` 里
   如果写 `cr3_tab[from] = pgd_tab[from]`，那记的是"我们以为它装了什么"；
   而 `user_enter` 可能被 gate 重入过、`cr3_tab` 被清过 0。读 `CR3`
   才是"CPU 正用的那一张"，这也是唯一可信的来源。
2. **`task_die` 里 `pgdir_kernel()` 必须放在最前面。** 之后是调度器在换栈、
   跑 dtor、挑下一任——这些都在内核表上跑；晚一步，私有表就带给了下一任。

### `invlpg` 与整表刷

- 打开一个用户页：只改一条 PTE → `invlpg`。几十个页的 exec 加起来也没有
  一次整表刷贵；
- 收工（`pgdir_clear_user`）：要清几十条 US，逐条 `invlpg` 不如**换一次表**。
  现在写成"先清全部 US，末尾 `mov cr3` 一次"，代价是一条指令，好处是幂等
  （重复调用不会重复刷）。

### 那 8 页从哪来

`pgdir_init()` 在 `tss_init()` 之后、`page_lock_kernel(0, 16MB)` 之前跑，
给 slot 1..4 各要 2 页（页目录 + PT0），共 **8 页常驻**。页分配器是位图式的，
这 8 页从此不再属于任何人——不回收。理由：每个槽位本来就常驻（`tasks[]`
也是静态的），表和槽位同寿命；回收要处理"表还在 CR3 里就被还回去"的窗口，
那是一整类新的 bug，没有功能收益。

所以 `verify` 的 `frames` 从 `0x05` 变成 `0x0D`（5 + 8）。这是**有意改变**，
不是回归。

### 一个真 bug：`user_enter` 不知道自己是 ABI 边界

接上页目录之后 `exec-argv` 突然红了：

```
exec argvp rc=E5 mag=0x00000000 val=0 ex=- sy=3/0 t=1 d=0 n=0/1
```

程序自己打出的 `hello`、`exec u=2 lh=2 jb=0 asu=0` 全正常——只有 job
结构的 magic 读回 0。排查顺序（每一步都是实测，没有一步靠猜）：

1. 注释掉 `pgdir_clear_user` → 仍红；
2. 把 `pgd_tab[slot] = pgd` 改成 `= 0`（干脆不切表）→ **仍红**
   → 与页目录/CR3 无关；
3. 加 `.bss` 影子量对比：body 侧 `bs=0x45584543`（对的），
   dtor 侧 `sh=0` → 坏在 body 之后、dtor 之前；
4. 用**单参数** `printk` 打栈上参数槽 `[b1] j=0x0002FF98`，
   而局部变量 `j` 读到同一个值 —— 参数槽是对的，局部变量被冲了；
5. 在 `user_enter` 前后用 `__asm__ "=m"` 各存一份 `ebx/esi/edi`：
   `si=0x1C900` → `si1=0x2FF98`。

**结论：** `user_enter` 跳进 ring3 时，`ebx/esi/edi` 是 callee-saved，
但 asm 桩没有保存它们。`uprog4.asm` 用 `esi` 当字符串指针，返回内核时
`esi = argv[1]` 的地址；而 exec body 里的 `j` 正好住在 `esi`，
`j->magic = res[-6]` 实际写到了 `0x2FF98 + 0x50 = 0x2FFE8`（用户栈指纹区），
job 结构里的 magic 一个字节都没动过。

```nasm
user_enter:
    mov  ebp, esp
    push ebx                 ; ← 这三条和下面的 pop 是这次真正的修法
    push esi
    push edi
    ...
user_return_stub:
    mov  cr3, eax
    pop  edi
    pop  esi
    pop  ebx
    pop  ebp
    ret
```

顺序是自洽的：记录 `esp` 指向 edi 槽，iret 帧 push 在它下面，不覆盖。
修后 `si1=0x1C900`、`m=0x45584543`，`exec-argv` 回 PASS。

**这条要当成硬约束记住：** 任何"进 ring3 / 从 ring3 回"的桩，都是 ABI 边界，
必须把 callee-saved 原样还回来。之前在这里踩过一次（把 exec 证据写进 IVT、
`rc=E5 mag=0`），当时以为是"别改寄存器"；现在知道**只"不改"不够，
调用者和被调用者用的是同一套寄存器，必须显式保存/恢复**。

### `pgd` 命令与证据

`shell/cmd_prog.c` 的 `cmd_pgd` 打一行（守 79 列，只列 s1..s4）：

```
pgd s1=0x023000 s2=0x025000 s3=0x027000 s4=0x029000 sw=103127 us=0 mp=2 fl=0
```

- `s1..s4`：四个槽位的私有页目录地址，`0x000000` 就是没建起来；
- `sw`：`pgdir_switch` 被调用的次数。它只在**真的要换表**时涨——
  slot 0 到 slot 0 的调度不切，所以 idle 到 idle 一动不动；
- `us`：此刻所有槽位合计有几格 US=1（空闲时必须为 0）；
- `mp`：`pgdir_set_user` 成功放行的累计次数；
- `fl`：放行失败的累计次数。**`mp` 不动时靠它区分"没调"和"调了但失败"**
  ——这一格这轮就是靠它才排掉的假问题（见下）。

`tools/pgdir.sh`（回归第 18 项，插在 `pg.sh` 之后）就按这几样判：

```
A. s1..s4 都非 0
B. 空闲时 us=0
C. 跑一趟 exec uprog 之后：mp 变大、sw 变大、fl 不变、us 回 0
D. 两趟之间 s1 的地址不变（表是槽位常驻资源，不是每次新建）
```

判 C 的逻辑值得单说：只有"跑的时候放行、跑完收掉"两头都动，才说明
闭环真的走了一遍。`mp` 只增不减、`us` 是净数——两个一起看才排除
"只放不收"（`us` 会大于 0）和"根本没放"（`mp` 不动）。

### 连"取证"本身也踩了一脚

第一版 `pgdir.sh` 判 C 失败，输出是 `mp 没有变大：0 -> 0`、`sw` 也一模一样。
差点去怀疑 `pgdir_set_user` 没被调用（甚至去翻了反汇编确认 `call` 在）。
真相更简单：**跑完 `exec` 之后没有重新敲一次 `pgd`，dump 读到的是第一屏
那句旧话**。屏幕上的字不会自己消失，`pmemsave` 搬的是"现在屏幕上有什么"。

直接读计数器内存（`pmemsave 0x19690`）才看清：`n_maps=2`（代码页 + 栈页）、
`n_switch` 涨了 2（0→1、1→0 各一次）。功能一直是对的。

所以脚本里那句注释不是废话：

```sh
type_cmd e x e c spc u p r o g; sleep 2
# 跑完之后必须**再敲一次 pgd**：直接 dump 读到的是上一次的输出
type_cmd p g d; dump
```

**测试也要证伪自己**：一条判据红了，先确认"取证这一步本身有没有问题"，
再怀疑被测对象。这次如果不读内存，很可能去改一段本来正确的代码。

### 规模与回归

- kernel **95792 字节**（+1792：`pgdir.c` 270 行 + `intr.asm` 装/卸 CR3
  + 三处 push/pop + `pgd` 命令的 `fl` 字段）
- `TASK_SIZE` 仍 **108**（页目录表是 `.bss` 静态数组，没进任务结构）
- shell 命令 **26** 条（加 `pgd`），help 第三行变成
  `sys date clock pgd panic`
- `frames` 基线 `0x05 → 0x0D`，`boot-screen.txt` 已更新（guard va 也随
  分配顺序从 `0x25FFC` 变成 `0x2DFFC`）
- `bash tools/check-all.sh` → **PASS 27 / FAIL 0**

### 还没做的

- **写保护**（这一条后来补上了，见文末「用户页写保护」）：这一轮里私有表的用户页
  还是 `P|RW|US`，用户程序能写自己的代码页。补上的做法是"ELF 的 `PF_W` 决定页的
  `RW`"，证据 `tools/wx.sh`；局限仍在**页内混排**——一页里同时有 `.text` 和 `.data`
  时只能整页可写，要真正 W^X 得让链接脚本把段按页对齐；
- **没有地址空间隔离的验证**：现在两个任务不能互相读，是因为**表不同**，
  但还没有一条测试去证明"槽位 1 读槽位 2 的页会 #PF"。`pgdir.sh` 只测了
  计划的那四件事；
- **`exception.c` 的越权判据已经跟上了**：改成“用户态发起就一律当越权”，
  不再问地址落在哪；`tss.cr3` 也改成跟着 `pgdir_load` 走。两条都写在文末
  「用户态缺页：一律当越权」那节，证据 `tools/pghigh.sh`；
- **页目录页不回收**：槽位死一次、活一次都还是那 8 页。
- **没有 `fork`**：地址空间对象（aspace）现在是 exec 独占造的，共享/写时复制
  都要等 fork 这个调用存在之后才有意义。


## 用户态缺页：一律当越权（判据从"地址"改回"发起者"）

每任务页目录落地之后，`exception.c` 里那条判据就成了最后一块没跟上的补丁：

```c
if ((error & 0x04) && cr2 < KERNEL_LOCK_END) {   /* 旧 */
```

它的后半句是**单张页目录时代的产物**。那时所有任务和内核共用一张表，只有
16MB 恒等映射被 `page_lock_kernel` 锁掉了 U 位，所以"用户态 + 落在 16MB 内"
才等价于"越权"。16MB 以上没有锁、也没有私有表，只能靠"地址不在按需窗口"
兜底——兜不住的结果是：请求被放给 `page_fault_map`，补不上就打印
`[mem] PF ... unhandled` 然后 `kernel_idle`。

也就是说，**一个 ring3 程序只要去摸 16MB 以上的地址，就能把整个系统停住。**
这不是"越权被拦住"，这是"用户能把内核拖进不该它走的路径"。

现在的判据只有一条：

```c
if (error & 0x04) {                              /* 新：只看发起者 */
```

理由很硬：私有页目录里 `dir[4..1023] = 0`，16MB 以上在用户表里**根本不存在**；
0..16MB 里真正对 ring3 开着的，只有 lease / 私有 PT0 放过的那几页。
所以"用户态发起"已经蕴含"这一页不该它碰"，不需要再问地址。

### 三种缺页，谁走哪条路

```
内核态缺页      → page_fault_map 补页（按需窗口 PDE 8 那条），iret 重试
用户态缺页      → 越权：返回帧改成 user_return_stub，交回内核（不补页、不停机）
用户态栈溢出    → 更早一步被 task_guard_victim 认出，走 task_guard_exit 被杀
```

顺序不能反：guard 判断在越权判断**之前**。否则栈溢出的地址（在任务栈下方
的 guard 页）也会落进"越权"分支，任务不会被杀、只是被交回内核，死因也就
传不到 dtor——那是 `neg_*` 那批负向用例专门钉住的一条。

### 后果：补页只服务内核态

这条改动把"按需分页"的受众收窄成**只有内核**。以前 `page_fault_map` 不看
发起者，ring3 在 32MB 窗口缺页也能拿到一页；现在这条通路彻底断掉。

- `page_fault_map` 本身没改（它仍不看 U 位），因为它的调用者只剩内核态；
- 那条 4MB 窗口仍然只建在**内核页目录**上（私有表的 `dir[8]` 是 0），
  所以就算哪天有人误把用户态请求放进去，也补不出来——两道锁，一道在
  调用方（判据）、一道在表里（不存在）。

### TSS.cr3：一格没有读者的字段，也不能乱写

`kernel/tss.c` 里原来是 `tss.cr3 = 0`。这一格只在**硬件任务切换**（task gate /
`ljmp` 到 TSS 描述符）时被 CPU 装载；本内核不用 task gate，所以它没有读者，
"写 0 也没事"。

但这正是埋雷的写法：别处已经写着"每个任务有自己的页目录"，而 TSS 里那一格
停在 0（= 内核表）。将来谁补上 task gate，切进来的用户任务会拿着内核表跑，
隔离当场失效，而且症状会非常难查（"明明每任务一张表，怎么又能读内核了"）。

修法是把这一格变成**跟随量**：在 `mm/pgdir.c` 唯一换表的地方写一次。

```c
static void pgdir_load(unsigned int cr3)
{
    __asm__ volatile ("mov %0, %%cr3" : : "r"(cr3) : "memory");
    tss_set_cr3(cr3);        /* 硬件任务切换会读的那一格，永远等于"现在在用的表" */
}
```

少一个调用点，就少一处可漂移的账。空闲时它自然是 `0x000000`（内核表），
进 ring3 前后跟着变成那张私有表、再回来——`pgd` 命令第二行的 `tc=` 就是它。

### 证据

`tools/pghigh.sh`（回归第 19 项，插在 `pgdir.sh` 之后），探针是
`boot/uprog6.asm`：在 ring3 里 `mov eax, [0x01100000]`（16MB+1MB），
刻意**不写指纹**，这样"被拦下"和"跑完"能区分开。

```
  盘上的文件：uprog 143  high 11
  A. exec high rc=E5 mag=0xDEAD0000
  C. 越权之后还能敲命令：irq gp=0x000001
  B. 屏幕上的 unhandled 行数：0
PASS
```

三条判据各自钉一件事：

- **A**：`rc=E5` + `mag=0xDEAD0000` = 哨兵原样在。任务被交回内核、没跑完，
  也没有"补页成功继续执行"的痕迹（否则会读到一个值、把指纹写上）；
- **B**：屏幕没有 `unhandled` = 这一趟**根本没进** `page_fault_map` 的失败分支。
  这条是"判据真的把它们分开了"的直接证据；
- **C**：越权之后再敲 `irq` 还有回显 = 系统没被停进 `kernel_idle`。
  用 `irq` 而不是 `ver`，因为 `ver` 的输出和屏幕顶上的横幅一模一样，
  系统停住了也能"读到"它。

### 这一轮踩的两个坑（都在测试侧，不在内核侧）

1. **`FS_V1_MAX_FILES 8`**：第一次造盘按老习惯塞了 10 个文件，`exec high`
   直接给 `rc=E1`（`fs_lookup` 失败）。根目录缓存只装 8 项，第 9 个文件
   在内核眼里不存在。改成"基础三件套 + uprog + high"五个才对。
2. **位置参数里的空串会被折叠**：`datadisk.py` 第 11 个位置是 high，前面
   一串 `''` 在中间层（SSH 助手）被吃掉了，于是 `uprog6.bin` 落到了第 10 位、
   变成 `fdprobe`——盘上有个 11 字节的"fdprobe"，而 `high` 不存在。
   修法是给 `datadisk.py` 加 `名字=值` 的具名参数，`high=` 这样传；
   判据也顺手改成"盘上真能看到 high"。

两条都不是内核 bug，但都能让回归红得莫名其妙——和上一轮那条"没重敲 pgd"
是同一类：**先怀疑取证，再怀疑被测对象**。

### 规模与回归

- kernel **96048 字节**（+256：判据与注释、`tss_set_cr3`、`pgd` 的第二行）
- shell 命令仍是 **26** 条（`pgd` 只是多打一行）
- `bash tools/check-all.sh` → **PASS 28 / FAIL 0**

### 还没做的

- **写保护**（后来补上了）：这一轮的用户页仍是 `P|RW|US`，程序能改自己的代码页。
  见下一节「用户页写保护」——`PF_W` 决定页的 `RW`，证据 `tools/wx.sh`；
- **没有跨任务隔离的负向测试**：现在"槽位 1 读槽位 2 的页"靠表不同成立，
  但还没有一条测试真的去撞墙（`pghigh.sh` 撞的是"16MB 以上不存在"；
  `wx.sh` 撞的是"自己的代码页只读"，也不是跨任务）；
- **`page_fault_map` 仍不看 U 位**：靠调用方判据 + 表里不建两层兜着，
  没有"函数自己就拒绝用户态"这条内建防线；
- **TSS 仍然只当 `ss0:esp0` 用**：`tss.cr3` 现在是对的，但 task gate 本身没做，
  所以它依然是一格"准备好了、还没启用"的字段。

## 用户页写保护：代码页只读

W^X 的第一步。在这之前，ring3 能进的每一页都是 `P|RW|US`——"能读"和"能写"
一起给。于是一个用户程序（bug 或恶意改写）可以改自己的代码页，而内核看不出
任何异常：PTE 说"这一页对用户开着"，当然也就"可写"。

### 权限从哪来：ELF 的 `p_flags`

`fs/elf.c` 的装载本体改成了 `elf_load_map()`：搬字节的同时产出一张"哪几页可写"
的图（`wmap`，每页一个字节）。规则只有一条：

- 只有 `p_flags` 带 `PF_W` 的 `PT_LOAD` 段**覆盖到的页**才算可写，其余整页按只读；
- 初始整段清 0（保守方向）。漏标一页可写 → 程序写自己的数据时被杀，现象明显、
  一次就能发现；多标一页可写 → 写保护形同虚设，现象隐蔽。所以宁可严。

老接口 `elf_load()` 保留，只是转调 `elf_load_map(..., wmap=NULL)`——不产生权限图，
行为一个字节都不变。

### 放行那一侧：`pgdir_set_user_page(..., writable)`

`pgdir_set_user()` 原来只会 `pt[pti] |= PTE_US`。现在拆成两层：

```c
pt[pti] = writable ? (v | PTE_US | PTE_RW) : ((v | PTE_US) & ~PTE_RW);
```

`pgdir_set_user()` 变成 `writable = 1` 的薄包装，所以所有老调用点语义不变。
栈页永远可写，代码页按 `wmap` 走。

**slot 0 那条兼容路（`page_mark_user`）做不到只读**，这条按"没这个能力"拒绝并记
`fl++`，而不是悄悄按可写放行——静默降级比失败更难查。实际上 slot 0 是 main，
不会跑 exec 的用户页。

### 收回时要把 RW 还回去

`pgdir_clear_user()` 原来只清 `PTE_US`。写保护上线之后这就不够了：写保护页的
`RW` 也是清的，只清 `US` 会在私有表里留下一个"只读的内核格"。语义上那页已经
还给池了，池里的页不该带着上一任主人的权限；而且将来一旦打开 `CR0.WP`，
这个只读格就是一颗雷。改成：

```c
pt[i] = (pt[i] & ~PTE_US) | PTE_RW;
```

（当前 `CR0.WP = 0`，所以就算漏了这一步，ring0 照样能写只读页——这正是"现在没
炸、但埋着"的那种坑。）

### 判据：`fe=0x7` 才是"写保护"，不是"页不存在"

三类缺页的错误码不一样，这是唯一能把它们分开的地方：

| 现场 | 错误码 | 含义 |
| --- | --- | --- |
| 用户态写只读页 | `0x7` = P\|W\|U | 页**在**、想写、用户态 |
| 用户态读不存在的页 | `0x5` = P\|U（16MB 以上是 `0x0`） | 页不在 |
| 内核态缺页 | 无 U 位 | 走补页或 panic |

`pghigh.sh` 那轮只判"任务被拦下"，因为读不存在的页和写只读页在结果区里都是
"指纹没写"。这一轮必须看错误码：`exec ro` 给 `rc=E5 mag=0xDEAD0000` 也可能是
"程序自己跑完 return、只是没写指纹"，两者从 `exec` 的输出里**长得一模一样**。

所以 `cmd_pgd` 的第二行加了一格 `fe=`（`ring3_fault_err`，最近一次 ring3 缺页）：

```
pgd s1=0x023000 s2=0x025000 s3=0x027000 s4=0x029000
pgdc sw=97767 us=0 mp=0 fl=0 fe=0x000007 tc=0x000000
```

实现踩的坑和上一轮 `tc` 一模一样：**`hex6()` 自带 `0x`，拼字符串时不要再写一遍**
（先写成 `" fe=0x"` + `hex6` 会打出 `fe=0x0x000007`）。

### 证据：`tools/wx.sh`（当时是第 20 项；`kv.sh` 插进来后变成第 21 项）

探针 `boot/uprog7.asm` + `tools/uprog7.ld`（`PHDRS ... FLAGS(5)`，也就是 **R E**，
`readelf -l` 里没有 `W`）。程序第一条指令就 `mov byte [uprog7_ro], 0xAA`——写
自己所在的代码页。

```
  A. 裸二进制（可写）：exec uprog rc=0 mag=0x45584543 val=36
  B. 只读代码页：exec ro rc=E5 mag=0xDEAD0000
  C. 最近一次 ring3 缺页的错误码：fe=0x000007
  D. 越权之后还能敲命令：irq gp=0x000001
  D2. 屏幕上的 unhandled 行数：0
PASS
```

- **A** 是回归对照：裸二进制整段可写（`wpage` 全 1），程序在栈上写指纹/字符串
  照常跑完——写保护没有误伤正常路径；
- **B** 哨兵还在 = 没跑完；
- **C** `fe=0x7` = P|W|U，这一条才把"写保护拦住"和"自己跑完 return"分开；
- **D/D2** 系统没停、也没进补页的失败分支。

顺带一个发现：**`tools/uprog.ld` 和 `tools/uprog7.ld` 的产物都是 `R E`**——ld 默认
不给纯 text 段 `PF_W`（`uprog2.ld` 因为有 `.bss` 才是 `RWE`）。也就是说 `exec uprogel`
这趟的代码页现在也是只读的，而它照样 PASS：`uprog.asm` 只写栈，不写代码页。
这条不是设计出来的，是 `readelf` 顺手看见的——也说明"代码页只读"对现有程序
没有副作用。

### 规模与回归

- kernel **96592 字节**（+544：权限图、`pgdir_set_user_page`、`cmd_pgd` 的 `fe`）
- shell 命令仍是 **26** 条（`pgd` 只多了半行字段）
- `bash tools/check-all.sh` → **PASS 29 / FAIL 0**（新增 `wx.sh`）
- 同一趟全量里 `clock.sh` 红了一次：`d=+13 与欠账 6 相差 7s`。**不是内核回归**——
  这台调试机没有 `/dev/kvm`，QEMU 跑 TCG，宿主 `sleep 12` 实测过了 17s，而内核
  tick 只走了 11s。凡把"sleep 的标称时长"当基准的判据都会红（`clock.sh` 的
  D/G/E/F 四处阈值因此从 4/2/3/3 放宽成 10/19..32/≤5+相对改善/≤5，
  理由写在该脚本头注释里："校准真的生效"由 `left` 从 24s 降到 4s 这种前后对比
  证明，而不是硬比一个理想值）。

### 还没做的

- **数据段与代码段的页内混排**：现在按**页**判权限。一个页里同时有 `.text` 和
  `.data` 时（ld 不用 `FLAGS` 分开段就会这样），整页会被判成"可写"，写保护
  对那些页失效。要真正 W^X 得让链接脚本把段按页对齐、内核按段而不是按页判；
- **栈/数据页还缺 `NX`**：没有 PAE/NX 位，`P` 只管"在不在"，所以数据页照样
  可执行——写保护的对称面（不可执行）还没做；
- **没有装 `CR0.WP`**：ring0 写只读页不受限（`pgdir_clear_user` 也因此靠"恢复
  RW"兜住）。要开 WP 就得把"内核自己写用户页"的所有路径改成"先改 PTE 再写"；
- **跨任务隔离仍无负向测试**：见上一节末尾。

## 机器可读证据通道（KV）：不再把屏幕排版当接口

这一版不动任何内核语义，只换取证方式。理由是这一年踩过的坑有一半不在被测对象：

- `hex6` 自带 `0x` 而我拼字符串时又写了一遍，真实输出变成 `tc=0x0x000000`；
- `pgdir.sh` 的 `grep -oE 'tc=0x[0-9A-F]+'` 里字符类没有 `x`，抓到 `tc=0x0`
  就被截断，屏幕上看着像"值被压短了"；
- `clock.sh` 的 `fld()` 不加前导空格时，`ms=` 会被当成 `s=` 命中；
- `ver` 的输出和屏幕顶上的横幅一模一样，系统停住了也能"读到"它。

四次都是**排版即接口**的代价：改一个字符，回归就红，而被测行为完全正确。

### 契约

```
KV <tag> k1=v1 k2=v2 ...\n
```

- **只写串口**（`-serial file:...`），一个字节都不碰显存：24 行证据区、
  `tools/verify.py`、所有抓屏判据都不受影响；
- 值的三种写法：
  - 十进制无前导零（`sw=173187`）；
  - 十六进制 `0x` + **8 位**（`tc=0x00000000`）——**刻意与屏幕的 6 位不同**：
    屏幕上为省列只打 6 位，`mag=0x45584543` 这么打会变成 `0x584543`，
    机器通道的头号职责是不丢信息（这条是本轮实测翻车后改的，第一版我照抄了 6 位）；
  - 返回码 `0` 或 `E<n>`（与屏幕 `rc=E5` 完全同口径，迁移时判据不用重写）；
- 一行上限 240 字节，装不下就截断并在结尾补 ` ovf=1`——宁可让脚本看见"这行不全"，
  也不要让脚本拿半行去断言。

### 挂了谁

四个入口：两个"被测行为"、两个"系统还活着"——它们合起来是五个脚本的共同取证点：

| 命令 | 记录 | 字段 |
| --- | --- | --- |
| `pgd` | `KV pgdir` | `s1..s4 sw us mp fl fe tc` |
| `exec <名字>` | `KV exec` | `name rc mag val ex sy sd t d n nf pg u lh jb asu ep fe` |
| `irq` | `KV irq` | `gp pf tick kbd yield tune sum`（号不存在写 `-`） |
| `stat` | `KV stat` | `pf pf_fail alloc kill cmd io ovf` |

`sy`/`sd` 在 KV 里拆成两个键（屏幕上是 `sy=2/1` 这种"一对"，脚本还得再切一次 `/`）。
后两个是为了把"系统没被停住""没进 unhandled 分支"这两条判据从**排版**里拿出来：
以前它们是"`irq` 有没有回显""屏幕上 `unhandled` 行数是不是 0"，前者靠字符是否落在
屏上、后者数的是排版（证据区滚动之后，行数天然是 0）。现在前者是"`KV irq` 记录
存在且 `pf ≥ 1`"，后者是"`stat.pf_fail` 这一趟没涨"。

### 证据：`tools/kv.sh`（回归第 19 项）

**这条脚本刻意不用 `pmemsave`**，全部判据只看串口：

```
  A0. 起机到 idle 之间自发产生的 KV 行数：0（应为 0：KV 只由命令触发）
  A. KV pgdir：s1=0x00023000 s2=0x00025000 s3=0x00027000 s4=0x00029000
              sw=149142 us=0 mp=0 fl=0 fe=0x00000000 tc=0x00000000
  B. KV exec：name=uprog rc=0 mag=0x45584543 val=36 ex=36 sy=2 sd=0 t=1 d=0
              n=1 nf=0 pg=1 u=2 lh=2 jb=0 asu=0 ep=0x00000000
  C. KV pgdir（第 2 条）：s1..s4 同上，sw=149144 us=0 mp=2 fl=0
              fe=0x00000000 tc=0x00000000
  E. 串口里的 KV 记录：3 条，其中溢出 0 条（应为 3 与 0）
```

（`sw` 是页计数，跟着这版 kernel 的大小走，**脚本不写死**，只判"涨了 2"。）

- **A0** 证明这条通道只由命令触发（不是又一条日志洪流）；
- **B** 一条记录就能判完 exec：`rc/mag/val/ex` + `d=0`（死因是 return）+
  `jb=0 asu=0`（job 表与地址空间对象都还干净）；
- **C** 把 `pgdir.sh` 的核心判据换一条通道重做：`sw` 涨、`mp ≥ +2`、`us` 回 0、
  `tc` 回内核表、`s1` 地址不变；
- **D** 唯一一次 dump 只为证明屏幕那两行**仍然在**（两路并存，KV 不是替代人看）；
- **E** 记录数正好 3 条、无 `ovf=1`——少一条是漏发，多一条是重复发。

### 收益的证明：三条脚本已经迁过去

`pgdir.sh` 第一个迁：同一个结论，判据来源从"抓屏 + 正则"换成"读串口 + 切 KV"。

```
  A. 空闲时的 KV（那一趟实测）：... sw=138709 us=0 mp=0 fl=0 fe=0x00000000 tc=0x00000000
     屏幕对照：pgdc sw=138709 us=0 mp=0 fl=0 fe=0x000000 tc=0x000000
  C. 跑完 uprog 的 KV：... sw=138711 us=0 mp=2 ...
     屏幕对照：pgdc sw=138711 us=0 mp=2 fl=0 fe=0x000000 tc=0x000000
PASS  ...mp 0->2、sw 138709->138711、fl 不变、us 与 TSS.cr3 都回来、表地址不变
```

`wx.sh`（写保护）与 `pghigh.sh`（用户态 #PF 一律越权）这一版跟上来，判据从
"屏上正则 + 数 unhandled 行"换成"`KV exec` / `KV pgdir` / `KV irq` / `KV stat`"：

```
  wx.sh      A. KV exec#1：name=uprog rc=0 mag=0x45584543 val=36
             B. KV exec#2：name=ro rc=E5 mag=0xDEAD0000
             C. exec 的 fe=0x00000007；pgd 的 fe=0x00000007（两处同口径）
             D. KV irq：gp=1 pf=9 ...（记录存在 = 命令真的执行了）
             D2. stat：pf=3 pf_fail=1 —— 与基线那条比没涨
  pghigh.sh  A. KV exec：name=high rc=E5 mag=0xDEAD0000（fe=0x00000004）
             B. stat.pf_fail 1 -> 1（这一趟没进 unhandled）
             C. KV irq：记录存在、pf ≥ 1
PASS  两条都通过
```

这一迁才暴露出来一个**值得记下来的事实**：`pf_fail` 是**累计**数，起机自检那一步
（`main.c` 故意往窗口外写）早就把它 +1 了。所以旧脚本"屏幕上 unhandled 行数 = 0"
之所以恒真，不是因为这一趟干净，而是因为证据区被滚动刷掉了——它测的是排版，
不是行为。新判据取一次基线、只比**增量**，这才是那条判据本来想说的意思。

`exec` 记录顺带多了 `fe` 字段（最近一次 ring3 缺页错误码）：写保护那趟不必再等
后面一条 `pgd` 才知道错误码，`exec` 的现场与这个码是同一趟。

（`sw` 跟着 kernel 大小变，脚本只判"涨了 2"，不判绝对值；`fe`/`tc` 两种通道
一个是 6 位一个是 8 位，正好是"屏幕给人看、KV 给机器判"这条分工的活标本。）

屏幕那两行仍然打印、仍然进 `echo`（人看的对照），但**判据不再读它**。所以
以后想给 `pgd` 加字段、改列宽、把 `fe` 挪到第二行，都不会再把这条回归弄红。

### 规模与回归

- kernel **99792 字节**（+313：`irq`/`stat` 两处 KV 块、`exec` 的 `fe` 字段）
- `__kernel_end=0x0001D7BC`，分配区从 `0x00020000` 起，余量 **10308 字节**
- shell 命令仍是 **26** 条
- `bash tools/check-all.sh` → **PASS 30 / FAIL 0**（三条迁完的脚本各自单跑 PASS）

### 还没做的

- **值不能含空格**：`kv_str` 原样追加，写进空格就把 `k=v` 切坏了。当前只有
  `name`（文件名不允许空格）、`ex=-`、`irq` 的 `-` 用到字符串，等真需要时再加转义；
- **中断里不能发 KV**：一行是"清缓冲 → 追加 → 整行发"，被打断就会出现两条记录
  交织。现在只有 shell 命令发（IF 开、但中断体不调 kv），够用；
- **串口仍是轮询写**：一行 60 字节在 115200 下约 5ms，都花在 shell 命令里，
  没有感知；但真要高频发就该改成 FIFO + 中断；
- **还剩一批屏幕正则**：`exec.sh`／`exec-argv.sh`／`elf.sh`／`pg.sh`／`fd.sh`／
  `lease.sh`／`ring3pre.sh` 仍是"抓屏 + 正则"。`KV exec` 的字段已经够判其中
  大部分（`rc/mag/val/ex/d/jb/asu/pg/u/lh`），剩下的迁移是逐个脚本的机械活；
  真正还差记录的只有 `fd` 的 `off` 与 `lease` 的 `ttl` 这类"不在 exec 汇总里"的字段。

## 拆 `shell.c`：26 个命令按域搬出去（后来加到 27：`mkdir`）

上一节拆的是**取证方式**，这一节拆的是**代码体积**。`shell/shell.c` 在这一步之前
是 1631 行、占整个内核 text 的 17% 以上——只读一遍就要滚几十屏，而它其实混了四件事：
内建命令、文件系统命令、程序/地址空间命令、纯观测命令。任何一类加功能，都会让剩下
三类跟着变长。

### 做法：纯搬运，先切块再归位

没有改任何一个命令的逻辑，只做三件事：

1. **切块**：脚本按"顶格 `static` 函数定义 → 列 0 的 `}`"切，花括号计数跳过注释与
   字符串（注释里出现过 `{`，直接数会错位）；紧邻空行上方的**整块注释**跟着函数走
   （判据是块末以 `*/` 结尾、块首 `lstrip` 后以 `/*` 开头——这样 `#define ... /* x */`
   的行尾注释不会被误当成块首）。
2. **归位**：按域分成三个文件 + 一个 helper 文件（见目录结构）；`shell.c` 只留
   内建命令、解析、命令表、分派。
3. **补接口**：`include/linux/cmds.h`（搬走的 19 条 `int cmd_xxx(void)`）与
   `include/linux/fmt.h`（11 个格式化 helper）。`shell.c` 里那 7 条内建命令仍是
   `static`，不进 `cmds.h`。

```
shell.c     1631 行 →  381 行
cmd_fs.c              408 行（disk/fs/ls/cat/fd/write/rm）
cmd_prog.c            247 行（pgd/exec/sys）
cmd_info.c            285 行（stat/irq/tasks/log/heap/kstress/panic）
cmd_time.c            227 行（clock/date + parse_int）
fmt.c                 142 行（hex2b..sdec）
```

### 编译暴露的三处“跨文件才成立”的约束

纯 `static` 的东西一旦跨单元，就会被这三条绊住——都不是语法错，而是原来“整个
shell 是一个 TU”这个隐含前提：

1. **`SHELL_RC_*` 宏原本只写在 `shell.c` 里**。搬到 `cmd_*.c` 的命令全部用
   `return SHELL_RC_OK`，宏一撤就变成隐式声明/未定义。解法：宏搬进
   `include/linux/state.h`（这是 shell 内部共享头，返回码属于“命令契约”，正好在它这层）。
2. **`cmds.h` 不能声明那 7 条 `static` 内建命令**。第一版把 26 条全写进 `cmds.h`，
   立刻 `static declaration follows non-static declaration`。解法：`cmds.h` 只列
   搬出去的那 19 条。
3. **签名必须与定义逐字一致**。`hex2b`／`hex4` 的第二参数是 `unsigned int`，
   而 `hex3`／`hex6`／`hex8` 是 `unsigned long`——按后者的模板批量生成声明就撞上
   `conflicting types`。这三条都是“声明与定义分开后必然会遇到”的错，且编译器指得
   非常准（`note: previous declaration ...`）。

另外两个漏 include（`cmd_prog.c` 的 `FS_NAME_MAX` 要 `fs/mosfs.h`、`cmd_info.c` 的
`page_frames_used` 要 `mm/page.h`）说明：**原文件顶部那一大串 include 就是一张依赖图**，
拆的时候必须按每个新文件真正用到的符号重新取一遍，不能凭记忆抄。

### 结果

```
kernel text 99792 → 96603 字节（-3189，主要来自跨命令的重复格式化代码不再各自内联）
__kernel_end=0x0001CB9C，余量 13412 字节
shell 命令仍是 26 条，`bash tools/check-all.sh` → PASS 30 / FAIL 0
```

**一次要记下来的事故**：拆分后第一次全量是 `PASS 3 / FAIL 27`，尾巴上一堆
`FileNotFoundError: /tmp/*-console.bin`——不是命令坏了，是 `build/mini-os.img`
在检查过程中**整个 build 目录消失**，后面 27 项全部“起不来”。重建后同一条
`check-all.sh` 原序 30/30 全绿。没定位到具体删除者（脚本里没有 `rm -rf build`），
所以记在这里：**跑全量之前先确认 `build/mini-os.img` 在**，别把“仪器没了”读成“被测坏了”。

### 第二刀：`cmd_info` → `cmd_time`

第一刀切完，`cmd_info.c` 仍是 473 行，而且里面明显是两拨东西：`stat/irq/tasks/log/
heap/kstress` 是“读计数器”，`clock/date` 是“各自一整套参数分派 + 拼一行”，两者
不共享任何状态。于是照第一刀的做法再搬一次：

- `cmd_time.c` 227 行：`clock`、`date`，以及**只服务 `clock skew` 的 `parse_int`**；
- `cmd_info.c` 285 行：只剩六条观测 + `panic`；
- `fmt.c` 142 行：`parse_int` 搬走后回到“只格式化、不解析”，头注释也改成这么说。

`parse_int` 这一条值得单独记：它当初被放进 `fmt.c` 是因为“格式化/解析同属一层”，
但它唯一的调用者是 `clock skew`（上限 700 还是为“把钟弄歪”定的）。搬进
`cmd_time.c` 后改回 `static`——**不再有跨单元的解析 API**，`fmt.h` 里的声明也删了。
以后谁想加“解析参数”，必须先想清楚是否又造了一个隐性公共接口。

顺带三件事：

- `cmd_info.c` 卸掉 `kernel/clock.h`／`kernel/chr_drv/rtc.h`／`lib/time.h`／`kernel/chr_drv/timer.h`
  四个再也用不到的 include（做法是**先删函数、再让编译器报未声明**来确认真没人用，
  而不是凭记忆删）；
- `Makefile` 的 `C_OBJS` 在 `cmd_info.o` 后加 `cmd_time.o`；
- `cmds.h` 不用动：`cmd_clock`／`cmd_date` 的声明本来就在搬走的 19 条里，`shell.c`
  的命令表按函数名引用，**名字没变就不用碰分派**。

```
kernel text 96603 → 96624 字节（+21：parse_int 换 TU 带来的对齐变化）
__kernel_end=0x0001CB5C，余量 13476 字节
```

### 还没做的

- **`cmd_*.c` 的 include 清单仍是手抄的**：只有 `cmd_info.c` 这次按编译器报错清了四个，
  其余三个文件还没做“哪些 include 其实没用到”的清理；冗余 include 只是编译变慢，
  但会掩盖真正的依赖。
- **`cmd_time.c` 里 `date <纪元秒>` 自带一个数字解析循环**，和 `parse_int` 长得像，
  但不接受符号、上限是 2147483647。没合并是有意的：两者边界条件不同，合成一个函数
  得传两个上限；等出现第三个“解析整数”的调用者再统一。

## 换一台机器：Arch + gcc 16 + QEMU 11

原来的调试机（Ubuntu 22.04 / gcc 11 / QEMU 6.2 / NASM 2.15）下线后，同一份源码搬到
`192.168.201.100`（Arch Linux，gcc 16.2.1 / NASM 3.02 / QEMU 11.1.3 / gdb 18.1）。
源码与真源逐字一致（`md5sum` 对过 Makefile、shell.c、cmd_time.c、cmd_info.c），
但**“能编”这件事在新工具链上重来了一遍**，暴露三个只在“换环境”时才现形的问题。

### 1. gcc 16 把常量地址当“大小为 0 的对象”

```
mm/pgdir.c:76: error: array subscript 0 is outside array bounds of 'unsigned int[0]'
              [-Werror=array-bounds=]
```

`pgdir_fill()` 里 `kern = (unsigned int *)PGDIR_KERNEL; ... dir[i] = kern[i];`——
gcc 13+ 看到来源是常量地址，就推断这是个大小 0 的对象。先试过把 `kern` 改成
`volatile unsigned int *`，**无效**（gcc 16 照样报，只是把对象写成 `volatile ... [0]`）。
最后在文件内局部关掉这一个诊断：

```c
#if defined(__GNUC__) && (__GNUC__ >= 13)
#pragma GCC diagnostic ignored "-Warray-bounds"
#endif
```

选局部而不是全局 `-Wno-array-bounds`：这个文件的指针全部指向页表物理页/恒等映射内存，
没有可被推断大小的真实数组；全局关掉会连真越界一起放过。

### 2. `tools/fs.sh` 是 CRLF，bash 直接不认

```
tools/fs.sh: line 11: set: -: invalid option
tools/fs.sh: line 31: syntax error near unexpected token `$'\r''
```

全量里它报 `rc=2（无结论行）`，单跑才看见真相。29 个脚本里只有它是 CRLF（用字节扫描
`0x0D` 确认过），转成 LF 后 PASS。**这也解释了以前“put + sed”流程为什么没出事**：
那套流程每次都把文件 `\r$` 清一遍，而这次是整包解压，没有那一步。

### 3. `clock.sh` 的判据踩了 bash 八进制

```
FAIL  ms=010 不是 10ms 的整数倍
```

`ms` 是 3 位补零的十进制，`010` 在 bash 算术里是八进制 = 8，`8 % 10 != 0` → 假红。
改成 `10#${ms:-0}`。这条以前不发作，只是因为 gcc 11 那台机器上采样时刻从没正好落在
`010`；换机器后时序一变就撞上——**判据里凡是要算术的数字字段，都该显式 `10#`**。

### 结果与新数字

```
make：kernel 103088 字节 @ 0x0A00，img 1474560
bash tools/verify.sh  → PASS（24 行结构比对，2 轮）
bash tools/check-all.sh → PASS 30 / FAIL 0
```

kernel text 比 gcc 11 那台大 6464 字节（96624 → 103088），是 gcc 16 的优化/对齐差异，
不是代码变多；分配区仍从 `0x00020000` 起，装得下。

**关于 `build/` 消失**：这台机器上第一次跑全量时重现过一次（27 项起全部“起不来”），
但随后两次全量 + 一个专门盯目录的探针（每 0.3s 检查 `build` 是否存在）都没再复现。
所以结论仍是“未定位”，只是现在有了可复用的现场工具：`/tmp/probe_build_gone.sh`
（边跑全量边盯 `build`，并给每行输出打时间戳）。

## 负向组：26 个变异测试进 CI（`check-all.sh --neg`）

正向 prove「功能在」，负向 prove「测试会红」：改坏一处源码
（`tools/negpatch.py` 做单次精确替换），然后要求对应的正向测试返回非零。

这组原来是 26 个 `neg_*.sh`，每个 20～26 行，其中 20 行是同一套样板：
`cp -r` 整棵树 → 打补丁 → `make -s` → `neg_case` → 清理。26 份样板的结果是
`tools/` 里 101 个文件中有 26 个只差「锚点 + 替换文本 + 被测脚本」三行，
而新增一条用例的成本是「再抄一遍样板」。现在样板只留一份：

- `tools/neg.sh`：26 条 case，一条一个 `case_xxx` 函数，函数体只写「哪个文件、
  锚点是什么、换成什么、跑哪个正向脚本」；建树 `mk_tree` / 编译 `build_tree` /
  跑 `run_on` 三个辅助共用。`neg.sh -l` 列 case 名，`neg.sh -1 <name>` 跑单条
  （`check-neg.sh` 就是这么调的），`neg.sh <词…>` 按包含匹配挑一组。
- `tools/negcommon.sh`：`neg_case <tag> <命令…>` 只认被测脚本的退出码。
  rc≠0 记 `ok`；rc=0 记 `MISS` 并置 `NEG_BAD=1`；证据固定是被测脚本最后 8 行。
- `tools/negpatch.py`：单次精确替换；退出码 0=命中一次 / 2=NOT-FOUND /
  3=多命中；LF 与 CRLF 两种换行各试一次。
- `tools/check-neg.sh`：逐条调 `neg.sh -1 <case>`，每项前 `killall qemu` + 删
  `/tmp/qmp.sock`，末尾打 `负向合计：PASS n / FAIL m`，退出码即这一组结论。
- `tools/check-all.sh --neg`：正向 35 项跑完再接负向组，退出码两者合并。
  不带参数仍只跑正向（正向约 9–12 分钟，整组再挂上 20 分钟左右）。

三条硬判据（少一条这组就会假绿）：

1. 变异必须真做出来——`negpatch` NOT-FOUND 或 `make -s` 编不过直接 FAIL，
   这不算「没咬住」，算「用例本身失效」；
2. 被测脚本必须 rc≠0——这是「测试会红」的唯一客观证据，打印出来的行不算；
3. 每条 case 自己把 rc 传出去——`neg_end` 后面不能再跟 `killall` / `rm -rf`，
   后者的 0 会盖掉前者的 1。

三个坑（都是这一路踩出来的）：

- 老式「原地改源码 → 跑 → 还原」一旦中途 make 失败或按下 Ctrl-C，源码就停在
  坏的那一版（`console` / `kbd_shell` / `shell_hist` 三条）。现在统一改到
  `/tmp/neg-<case>` 副本上，真树一个字节都不动，中断也只是留一堆临时目录。
- 被测脚本的工程根有两种来源，混用会得到「变异做了、测试全绿」这种最像假绿的
  结果：绝大多数正向脚本读 `$MINI_OS_DIR`，把它指到变异树就行；而
  `verify.sh` / `mkimg.sh` / `qemu-debug.sh` 用 `dirname($0)/..` 自己算根，
  必须直接跑**变异树里那一份**（`bash /tmp/neg-x/tools/verify.sh -n 1`）。
  `neg.sh` 的 `run_on` 因此按脚本名分了两条分支。
- `tools/kbd-echo.sh` 原来只有 `&& echo PASS || echo FAIL`，末尾 `killall`
  把 rc 归 0。变异后 `row 24` 明明空着，负向组却报 MISS。补 `rc=0/1` +
  `exit "$rc"`。

### 结果

```
bash tools/check-neg.sh
  PASS  bcache_hit       PASS  负向：1 个变异全部被对应正向测试抓住
  …
  负向合计：PASS 26 / FAIL 0（共 26 条）

bash tools/neg.sh -l
  bcache_hit  bcache_wt  console  dev_hd  kbd_depth  …  vfs_wpath      # 26 个名字
```

26 条覆盖：块缓存命中与写穿、控制台 CRTC、`/dev/hd` 选槽、命令表、历史、
变量表、键盘门/解屏蔽、环深、回显、环容量、行消费、提交深度、队列归属、
接管跟随、内核页锁、U/S 放行、锁范围、租约 refs / ttl、随任务回收、
租约表不记账、通知顺序、分区 base、`dev` 与 VFS 三处（名字比较、写落到根、
非空 rmdir）。
单条 30–90 秒（一次全量 make + 一次 QEMU 起停），整组约 20 分钟；
合并前 26 个文件、合并后 1 个，`tools/` 从 101 个文件降到 76 个。

## 存储栈第一层：块设备 + 块缓存（blkdev / bcache）

`fs/mosfs.c` 原来直接调 `ata_read` / `ata_write`——整个内核只有这一处
硬耦合到 ATA。（现在这层耦合收在 `fs/blkio.c`，`mosfs.c` 只剩六个对外
动作的编排。）把它拆成「块设备层」+「块缓存层」，是分区表、VFS、
从磁盘 exec 三件事的共同前置。

### 块设备层：`kernel/blk_drv/blkdev.[ch]`

- 4 个槽，**槽号 = 设备号 = ATA 盘号**（0/1），`base` 字段留给将来的分区偏移。
- `blkdev_register(slot, name, drive, base, sectors)` 幂等，重复登记不叠加。
- `blkdev_init()` 按 `ata_present_mask()` 登记：只有探测到盘才占槽。这台机器
  上只有 IDE 从盘（`-drive if=ide,index=1`，2048 扇区），引导用的 `-fda` 是
  软盘，不归它管——所以 **`blkdev_count=1`**、扇区表是 `[0 2048 0 0]`：
  槽号锚在盘号上，不做紧凑排列。
- `blkdev_read/write` 先判越界再解引用，越界只计 `blkdev_bound_denied_n`、
  返回 `-1`，不碰硬件。

「登记数 = 2」这个断言一开始写错了，它假定主盘也从 IDE 来；实际判据应是
「登记数 ≥ 1，且扇区表里非零项数 == 登记数」，再加一条「mosfs 用的那块盘
扇区数 > 0」。

### 块缓存层：`fs/bcache.[ch]`

- 8 槽直接映射，`slot = (lba + dev * 7) & 7`——`dev` 参与哈希，
  两块盘同一个 LBA 不会互相冒充。
- **写穿**：`bcache_write` 先落盘，成功才把新内容装进槽；盘写失败就把槽
  置 `valid = 0`，宁可下次重读，也不让缓存比盘新。`bcache_sync()` 恒 0。
- 缓存池**不是**静态数组：`unsigned short w[256] * 8` 放 `.bss` 会把
  `__kernel_end` 顶到 `0x1FD60`，离 `PAGES_BASE = 0x20000` 只剩 `0x2A0`，
  `tools/kernel-fit.py` 的一页余量直接失守、make 挂。改成 `page_alloc()`
  取一页 4KB 当池（每槽 96 字节 metadata 放 `tags[]`），拿不到页就退化为直读。
  kernel 因此从 103088 涨到 105192 字节。

### 语义变化（要记住的两条）

1. `fs_reads()` 从「读块请求数」变成**「真落盘的未命中块数」**
   （即 `bcache_misses()`），新增 `fs_hits()`；`fs` 命令输出末尾多一个 `hit=`，
   现在长这样：
   `fs ok=0x1 b=2048 i=32 rd=3 er=0 wr=0 cr=0 un=0 hit=7`
2. 多一页常驻 → row 22 的 `frames` 从 `0x0000000D` 变 `0x0000000E`，
   `tools/verify.py` 的断言已同步改
   （14 = 3 基线 + 2 ring3 常驻 + 8 页目录 + 1 块缓存）。

### 回归：`tools/blkdev.sh` / `tools/bcache.sh`

两个脚本都**不再靠解析屏幕**：用户页只有 23 行，`fs` 行会被滚动吃掉，
第二轮 `fs` 的输出根本不在屏上——早期版本因此假红（数字一直停在上一次的值）。
现在 `nm` 取符号地址，QMP `xp /1dw` 直接读：

```
bash tools/blkdev.sh
  A. blkdev_count=1  扇区表 [0 2048 0 0]
  B. 挂载后 miss=3 hit=7
  C. cat 一次后 miss=4 hit=8
  D. cat 两次后 miss=4 hit=10      ← 第二次零落盘、命中 +2
  PASS

bash tools/bcache.sh
  A. 基线 miss=3 hit=7
  B. cat long.txt miss=4 hit=8
  C. cat long.txt miss=4 hit=10
  D. write t1 miss=7 hit=20
  E. cat t1 miss=8 hit=21（屏上 hello 行数=1）
  PASS
```

两个负向都实测咬住了：`neg_bcache_hit`（命中判断加 `&& 0`）报
「第二次 cat 仍落盘 12 → 14」；`neg_bcache_wt`（写穿后不更新缓存行）报
「写后读回没看到 hello」。正向 **33 项**、负向 **22 项**已并入
`check-all.sh` / `check-neg.sh`。

判据上踩过两个坑：

- **写后读回的内容判定必须排除命令回显**：`> write t1 hello` 这行本身就含
  `hello`，`grep -c hello` 会数成 1，变异就被放过。要剥掉 `row N| ` 前缀，
  再 `grep -v '^>'`，只数输出行。
- **8 槽直映下「写后读零落盘」不成立**：写一个新文件要碰目录 / inode / 数据
  三块，`cat` 回来时其中一块可能已被别的块挤出槽，`miss` 会 +1。
  断言放成「增量 ≤ 2」，真正的判据交给「读回来的内容对不对」。

## 分区层：MBR 主分区（`kernel/blk_drv/part.[ch]`）

块设备的 `base` 字段一直留空，因为盘上还没有「设备内块 0 不等于盘上块 0」
的东西。MBR 分区一加，这个字段才算真的被用上。

分工很清楚：

- `part_init()` 扫 blkdev 的**整盘**槽（0/1），读 LBA 0，验末尾 `0x55 0xAA`，
  再逐条查 `start != 0`、`start + count <= 盘扇区数`；
- 合格的登记成槽 2/3：`drive` 保持原盘号、`base = start`、`sectors = count`。
  之后 `blkdev_read(2, lba)` 落到盘上的 `start + lba`，文件系统完全不知道
  自己站在分区里；
- 只认 4 条主分区，槽位用满（`BLK_MAX_DEV=4`，只剩 2 格）就记
  `part_skipped_n`；不跟扩展分区链、不认 GPT。

`fs/mosfs.c` 的选盘也跟着改了：原来的 `if (present_mask & 2) dev = 1` 换成
一条 **probe 链**——先试分区设备（槽 2 起），再试整盘设备（槽 0/1），
第一块在「设备内块 1」处带着 `MFS1` 的赢。有分区表就按分区挂，
没有分区表的老盘行为一字不变，两种盘一个入口。

`tools/datadisk.py` 多了个具名参数 `part=64`：把整卷布局挪到 LBA 64，
并在盘首写一张只含一条 `0x83` 分区的 MBR。卷内块号（SB=1、位图=4、
数据=5…）一概不变——分区设备的块 0 就是分区起点。

```
bash tools/part.sh
  A. part: count=1 mbr=1 skipped=0 type[2]=131
  B. sectors: 分区设备=1984（整盘 2048）
  C. fs ok=0x1 b=1984                ← 挂的是分区卷，不是整盘
  D. cat hello.txt：输出行数=1
  PASS

bash tools/neg.sh -1 part_sig           ← 变异：base 从 start 换成 0
  C. fs ok=0x0 b=0
  ok   part-base-dropped 被抓（rc=1）
```

「按分区挂」这一条有独立证据：`fs` 报的 `b=1984` 是分区长度，
整盘是 2048；如果退回整盘挂载，盘上 LBA 1 是空的，magic 都对不上。

### 代价：PAGES_BASE 再抬一页

加完分区层，`__kernel_end` 到了 `0x1F220`，离 `PAGES_BASE = 0x20000`
只剩 `0xDE0`——不到 kernel-fit 要求的一页余量，make 直接失败。
所以 `PAGES_BASE` 抬到 `0x21000`，可用页 **112 → 111**。

连带改动（都是「有意改变行为」，一起提交）：

- `tests/expect/boot-screen.txt` 的 `frames=…/0x70` 全部变 `…/0x6F`；
- 分配器给出的物理页整体后移 `0x1000`（row 7/8/10 的 `0x00022000`
  变 `0x00023000`），基准用 `verify.sh --update` 重生成后逐条确认过；
- `tools/verify.py` 里那条额外断言同步改成 `14/0x6F`。

kernel 现在 105704 字节 @ `0x0A00`，余量 7648 字节——够下一次加模块。

## 多级目录：路径解析 / mkdir / 读写都走树

到上一章为止，mosfs 是**平坦的**：只有根目录、最多 8 项、名字 12 字节。
`cat hello.txt` 能读、`write t1 hi` 能写，但 `d1/hi.txt` 这种路径它不认识，
也建不出子目录。这一章把路径打通——不新增磁盘格式，只把盘上已有的
「目录 = mode 2 的 inode + 一块目录项」解释成树。

### 路径规则只有三条

写在 `include/linux/mosfs.h` 的注释里，实现严格按它走：

1. 空串、`//`、结尾多一个 `/` —— 一律 `-1`（格式非法）；
2. 中间某一段是普通文件（`mode != 2`）—— `-2`（拿文件当目录）；
3. 名字超过 `FS_NAME_MAX` 的段 —— 直接不认（v1 的目录项就是 12 字节）。

`fs_path_lookup(path, &ino, &mode, &size)` 是唯一的入口：

```
`/d1/d2/deep.txt`
  ├─ 先扫一遍路径：有没有 '/'
  ├─ 没有 → fs_lookup()：走根目录的 dent[] 内存缓存，零块读
  └─ 有   → cur = sb.root，逐段 dir_find()
```

**单段走缓存这条分支不是为了省事，是为了不破坏基准**：`tools/fs.sh` 里
`cat hello.txt` 的断言带着读块次数（`rd=`），如果连根下单个名字都去查盘，
那条数字就对不上。路径进来走盘、单名进来走内存，两条腿各自有回归。

`dir_find(dir_ino, name, &ino, &mode, &size)` 做的事：`inode_read` 验
`mode == 2`，按 `blk_lookup` 逐块读目录块，每块里按 16 字节一条的
目录项比对名字（`name_eq` 逐字节比到两边同时见 NUL），命中后**再回
inode 补 mode/size**——目录项只存「名字 → 号」，别的东西一概不存。

`fs_dir_count_at` / `fs_dir_entry_at` 是给 shell 用的按下标遍历：
前者 `size / 16`，后者每条一次 `blk_lookup + blk_read + inode_read`。
带参的 `ls <路径>` 走这一对；不带参的 `ls` 仍旧用 mount 时的内存缓存。

### mkdir：只动三处

`fs_mkdir(path)` 的步骤（v1 目录最多占一块，满了一块就只能另建目录）：

```
1. 切末段名 / 父路径（parent[64] 栈缓冲）
2. 父解析：没有 '/' 或父为空 → sb.root；否则 fs_path_lookup(parent)
3. dir_find 查同名 → 返回 1（已存在，不重复建）
4. psize + 16 > FS_BLOCK → -2（父目录满）
5. inode_alloc → bitmap_alloc 清 0 写盘
6. inode_write(新号, mode=2, size=0, blocks={新块})
7. 父目录块尾部写一条 dirent
8. inode_write(父号, mode=2, size=psize+16, 原 blocks)
9. 父是根 → 顺手 dent[ndent++] 同步，紧跟的 ls 才看得到
```

返回码：`0` 建成、`1` 已存在、`-1` 父不存在、`-2` 父不是目录/目录满、
`-3` 读写失败、`-5` inode 用完。shell 只把它拼成一行
`mkdir <path> rc=<0|1|En>`。

### 写路径也走树：`fs_create` / `fs_unlink` 内部切父路径

两个函数签名不变，内部先用 `path_split` 把 `d1/nf` 切成父 `d1` 与末段
`nf`：

- 父为空 → 老路径（根目录 + `dent[]` 内存缓存，`fs.sh`/`fs2.sh` 的
  读块数与行为一字不变）；
- 父非空 → `fs_path_lookup` 定位父（必须 `mode == 2`），重名交给
  `dir_find`，建项与删项分别走 `dir_add` / `dir_del`。

`dir_add` 只做一件事：读父目录块 → 在 `psize` 处写 16 字节（名字 + 号）
→ 回写 → 父 inode 的 `size + 16`。`dir_del` 反向：后面的往前挪 16 字节、
末尾清 0、父 inode `size - 16`。根目录这两条额外同步内存 `dent[]`，
子目录不用——`dent[]` 从来只是根目录的镜像。

删文件仍是"先账后名"（位图 → inode 标空 → 目录项），这套顺序抽进了
`free_inode`，根与子目录共用。写失败要回收号：`dir_add` 不顺就
`inode_zero(ino)`，不留孤儿 inode。

shell 这边只放开了缓冲：`write`/`rm` 的名字缓冲从 `FS_NAME_MAX+1` 换成
`FD_PATH_MAX+1`，`rm` 对带 `/` 的名字不再印根目录项数（那个数对子目录
没有意义，印 `n=0->0`）。

`fd_open` 写模式也直接交给 `fs_create`（它自己切路径），建完用
`fs_path_lookup` 刷新 size；`fd_write` 之后的 size 回填同样走路径。
`fd_ent.name` 从 `FS_NAME_MAX` 放宽到 `FD_PATH_MAX = 40`——
`cat` 早就走 fd 层，所以 `cat d1/d2/deep.txt` 每跑一次都在顺带回归 fd 层。

### 目录的 size 改成「项数」

同一个子目录，在根表里印 16（字节）、在 `ls <它>` 里印 1（项），
看着像两个东西。所以 `root_load` 与 `fs_dir_entry_at` 统一：
**mode 2 的项印 `size / 16`**，mode 1 印字节。`ls` 的列宽没动。

### 证据

```
bash tools/vfs.sh                    ← 盘上是 /d1/{hi.txt, d2/{deep.txt}}
  deep from d2                       ← cat d1/d2/deep.txt（两段路径）
  hi from d1                         ← cat d1/hi.txt
  hi.txt              11
  d2                  1             ← 目录印项数
  hello.txt           52
  long.txt            169
  blob.bin            20
  uprog               1
  mkdir d1/new rc=0
  write d1/nf rc=0 i=6 w=2 n=0      ← 子目录里新建，落到 d1/new 旁边
  hi2                                ← cat d1/nf 读到刚写的
  ls d1：hi.txt / d2 / new / nf      ← 4 项
  mkdir d1 rc=1                     ← 重名不建第二遍
  rm d1/nf rc=0 n=0->0              ← 子目录里删掉
  rm d1 rc=E2                       ← 目录不删
  write: create failed              ← 父不存在（write nope/x）
  write d1/d2/df rc=0 i=11 w=6 n=1  ← 两级父目录里建文件
  deep2                              ← cat d1/d2/df
  ls d1：hi.txt / d2 / new           ← 回 3 项
  mkdir d1/tp rc=0                  ← 空目录
  rm d1/tp rc=0                     ← rmdir 删得掉
  write d1/tq/z rc=0 i=13 w=6 n=1
  rm d1/tq rc=E2                    ← 非空：不删
  ls d1：hi.txt / d2 / new / tq
  rm d1/tq/z rc=0
  rm d1/tq rc=0                     ← 腾空之后删得掉
  ls d1：hi.txt / d2 / new           ← 回 3 项
  PASS

bash tools/neg.sh -1 vfs_name           ← 变异：name_eq 改成不比字节（if(0)）
  ok   vfs-name-eq-dropped 被抓（rc=1）

bash tools/neg.sh -1 vfs_wpath          ← 变异：dir_add 的父换成 sb.root
  FAIL  新建后 ls d1 项数=3（应为 4）
  FAIL  cat d1/nf 没读到刚写进去的 hi2
  ok   vfs-write-to-root 被抓（rc=1）

bash tools/neg.sh -1 vfs_rmdir          ← 变异：size != 0 的判据改成 if(0)
  FAIL  rm 非空目录应 rc=E2（实得：rc=E1）
  ok   vfs-rmdir-nonempty 被抓（rc=1）
```

正向 18 条断言。三条负向各钉住一条判据：「名字要逐字节比」、「项要
写进父目录、不是根」、「非空目录不许删」。

### rmdir：空目录才删得掉

删除分成两条：`fs_unlink` 管文件，`fs_rmdir` 管目录。shell 只留一个
`rm`，它先试 `fs_unlink`，拿到 `-2`（"这是个目录"）再转给 `fs_rmdir`——
所以命令表没变，还是 27 条。

`fs_rmdir` 的判据只有一句：**`size != 0` 就返回 `-2`，一个字节都不动**。
顺序和删文件一样是"先账后名"：还目录块（位图）→ inode 标空 → 从父目录
抽项。所以半途失败最坏是"目录还在、里面空了"，不会出现"名字没了、块
永远占着"。

顺带把重复的三段收成了两个 helper：`root_del(idx)`（根目录的盘上移位 +
内存 `dent[]` 跟着移，根比子目录多一份镜像）和 `locate_in_dir()`（
父路径 → 父号 + 子号 + 下标，删文件与删目录共用）。`fs_unlink` 因此比
原来短了一半。

目录只有一块（v1 一块到底），真有额外的块就判 `-2` 不删——来路不明的
块宁可留着让人去查。

### 输出层：第 9 行起屏幕不再刷新（已查清并修掉）

上一版留的那个"无参 `ls` 少渲染最后一行"，根因不在路径解析，在输出
缓冲的记账上：

```c
n = out_count > count0 ? out_count - count0 : 0;   /* 旧 */
```

`out_count` 是**缓冲里现有几行**，上限 `SHELL_OUT_LINES = 8`。写满 8 行
之后它恒等于 8，再算差值永远是 0——所以从第 9 行输出起，屏幕上什么
都不再出现，直到 `clear` 把缓冲抹掉。之前"漏最后一行"只是这个现象在
那条序列里的样子（前 8 行刚好填满，第 9 行正是最后一项）。

修法是把"写过多少行"单独记成单调计数器：

```c
static unsigned int out_total;    /* 只增不减，不封顶 */

n = out_total - total0;                       /* 这一趟新增了几行 */
if (n > SHELL_OUT_LINES) {                    /* 超了就只搬最近 8 行 */
    skip = n - SHELL_OUT_LINES;
    n = SHELL_OUT_LINES;
}
idx = (head0 + skip + j) % SHELL_OUT_LINES;   /* 跳过已被覆盖的那几行 */
```

`out_count` 保持 0..8 不变，`shell_out_count()` 给自检用的语义没动。
`shell_con_mark/report` 的第二个参数从"行数"改成"累计游标"，头文件里
的注释同步写清楚。缓冲区没有变大，`SHELL_OUT_LINES` 仍是 8——只改了
记账方式。

证据就在 `vfs.sh` 第四段：那一段连发 9 条命令、累计输出 13 行，超过
8 行之后每一行都照常上屏（`ls d1` 四项齐全、`rm d1/tq rc=E2` 这类都
在）。

### 代价：PAGES_BASE 又抬两页

多级目录 + 写路径这批改动把 `__kernel_end` 顶到 `0x201C0`，离
`PAGES_BASE = 0x21000` 不足一页，于是抬到 `0x22000`（111 → 110 页）。
后来 `file_dev` / `char_dev` / `block_dev` 这一层抽象又把它顶到
`0x218A0`，再抬到 **`0x23000`**（110 → 109 页，`frames=…/0x6D`）。
`tests/expect/boot-screen.txt` 每次都用 `verify.sh --update` 重生成，
`tools/verify.py` 的硬断言同步改（现在是 `14/0x6D`）；分配器给出的
物理页整体后移，row 11 的 `4/0x6D` 与 row 22 的 `14/0x6D` 已逐条核对。

kernel 现在 **114408** 字节 @ `0x0A00`，余量 **5984** 字节——不到两页，
下一次加模块前先看一眼 `kernel-fit.py`。

## 按 Linux 0.11 重排目录：`drivers` 分两半、`fs` 分六件

目录结构一直是个折中：源码按职责分目录，头文件同套分组，但每个目录里
的文件数一路长上来。到这一次，`drivers/` 有 10 个 .c、`fs/mosfs.c`
一个文件 900 多行——找东西要翻，改一处要看整篇。于是照 Linux 0.11 的
老约定重排一次，**只动位置与 `#include` 前缀，不动一行逻辑**。

### `drivers/` → `kernel/chr_drv/` + `kernel/blk_drv/`

0.11 的 `kernel/chr_drv/` 放字符设备（按字节、按事件），`kernel/blk_drv/`
放块设备（按扇区、要缓存）。照着分：

- `kernel/chr_drv/`：`vga.c`、`timer.c`、`rtc.c`、`serial.c`、`keyboard.c`、
  `kbd_queue.c`、`kbdline.c`
- `kernel/blk_drv/`：`ata.c`、`blkdev.c`、`part.c`

头文件跟着搬家（`include/linux/` 7 个、`include/linux/` 3 个）。
`Makefile` 的 `vpath` 列表从 `drivers` 换成 `drivers/char drivers/block`，
**目标文件仍然平铺在 `build/`**——链接顺序不变，`__kernel_end` 一个字节没动。

### `fs/mosfs.c` → 五个文件 + 一个编排层

0.11 的 `fs/` 是 `super.c / block_dev.c / bitmap.c / inode.c / namei.c /
file_dev.c`，每个文件一件事。这边只有一套 mosfs，就照它的分法拆：

| 新文件 | 接过去的东西 |
| --- | --- |
| `fs/super.c` | 超级块校验链、`fs_mount`、全局状态（`sb`/`fs_dev`/计数/`dent[]`） |
| `fs/blkio.c` | 盘上缓冲 `blk`、大端 u16/u32 存取、`blk_read/blk_write`、`blk_lookup/ensure` |
| `fs/bitmap.c` | 数据块位图 `bitmap_alloc/bitmap_clear` |
| `fs/inode.c` | `inode_read/write/alloc/zero/free_inode` |
| `fs/namei.c` | 根目录缓存、目录项遍历、`path_split`、`dir_add/del`、`locate_in_dir` |
| `fs/mosfs.c` | 只剩六个对外动作：`fs_mkdir/fs_read/fs_create/fs_write/fs_unlink/fs_rmdir` |

内部共享状态收进一个新头 `include/linux/fsint.h`（`sb`、`blk`、`dent[]`、计数、
函数声明）。它不是公开 API：`include/linux/mosfs.h` 才给 shell 和 `kernel/` 用，
`fsint.h` 只有 `fs/*.c` 能看——这是「模块内部」和「模块对外」第一次用两个
头文件分开。

### 两个坑，都记在这儿

1. **`build/*.d` 会记住旧路径。** 搬完文件直接 `make`，报的是
   `No rule to make target 'drivers/serial.c', needed by 'build/serial.o'`。
   这不是 Makefile 写错了——是上一次编译留下的 `.d` 依赖文件里还写着
   旧路径，make 照着它去找源文件。删掉 `build/*.d`（或 `make clean`）就好。
   搬家类改动，`.d` 一定要一起丢。
2. **批量改 `#include` 必须按目录逐个核对。** 第一次批量替换报了「残留为空」，
   可真编的时候 `kernel/printk.c`、`init/main.c`、`kernel/panic.c` 三处还是旧
   前缀——替换脚本漏掉了这几个目录。改用
   `grep -rn 'include "drivers/' --include=*.c --include=*.h` 逐个目录扫，
   再全量重传编译，一次过。

### 验证

- `make -j4` 干净通过，`linewidth.py` 29 行全 ≤79 列；
  `__kernel_end=0x20CC0`、余量 **4928** 字节（与拆分前逐字节相同；
  这是重排那一刻的实测——后面加了 `file_dev` 那层抽象，现在是
  `0x218A0` / 5984，见上一节的「代价」）。
- 正向 `tools/check-all.sh`：**34/34 PASS**（日志 `/tmp/ca-split.log`）。
- 负向 `tools/check-neg.sh`：**25/25 PASS**（日志 `/tmp/cn-split.log`）——
  10 个脚本的 `negpatch.py` 锚点路径随之改到 `drivers/char|block/`，
  `neg.sh -1 vfs_name` 的锚点从 `fs/mosfs.c` 改到 `fs/namei.c`（`name_eq`
  搬过去了），全部仍能命中并抓住变异。

## 一套 `file_operations`：fd 层不再自己读文件

上一节把文件系统拆成了六件，但 `fd.c` 里还压着另一件事：它既管
"这一格是哪个槽位的、读到哪儿了"，又亲手调 `fs_read` / `fs_write`。
Linux 0.11 不这么分——`fs/fd.c`（其实是 `fs/file_table.c`）只管表，
真正动字节的是 `fs/file_dev.c`（普通文件）、`fs/char_dev.c`（字符设备）、
`fs/block_dev.c`（块设备），三者各有一套 `struct file_operations`。
照这个分工再拆一次，设备节点才有地方落。

### `struct file_operations`：五个回调 + 一个名字

```c
struct file_operations {
    const char *kind;   /* "file" / "tty" / "hd"：只用于打证据 */
    int  (*open)(struct fd_file *f);
    int  (*read)(struct fd_file *f, void *buf, unsigned int len);
    int  (*write)(struct fd_file *f, const void *buf, unsigned int len);
    int  (*lseek)(struct fd_file *f, unsigned int off);
    void (*release)(struct fd_file *f);
};
```

`fd_file` 也跟着从 `fd.c` 的 `static` 结构提到 `include/linux/fd.h`——
三套实现都要看 `off` / `size` / `flags` / `ino`，定义必须只有一份。
表本身仍是 `fd.c` 里的静态数组，`used` 也在 `fd_file` 里，谁都没多出一张表。

`fd_read` / `fd_write` / `fd_seek` / `fd_close` 于是只剩三层动作：
取格（越界或别人的格 → `FD_E_BADFD`，不解引用）→ 交给 `f->fop` →
记账（`n_read` / `n_written` / `n_denied`）。**方向检查不在这一层**：
"能不能读"是文件类型的属性（tty 双向、文件按 flags、hd 有写但只收整扇区），
放到 fd 层就是抄三遍。

### 名字分派：设备先于盘

`fd_open` 第一件事是查一张两个条目的设备表：

| 名字 | 设备号 | fops | 大小从哪来 |
| --- | --- | --- | --- |
| `/dev/tty` | `DEV_TTY` | `char_dev.c` | 没有容量（`size=0`） |
| `/dev/hd` | `DEV_HD` | `block_dev.c` | `open` 时按扇区数 × 512 算 |

匹配是**精确**的（长度与结尾 NUL 都要对上），所以 `/dev/ttyx` 不会被当成
`/dev/tty`。顺序是"先设备、后盘"，理由是：如果盘上恰好有个叫 `/dev/tty`
的文件，设备必须赢——否则 ring3 的程序一 open 控制台，拿到的是个普通文件，
屏幕上什么也看不出来，只有 `fd` 命令里的 `k=file` 会暴露它。

没命中设备表的名字走老路：写模式 `fs_create` + 复查，读模式
`fs_path_lookup`（目录不许当文件打开），然后 `fop = &file_fops`。
`fd_open` 最后调一次 `fop->open`（普通文件没有这个回调）。

### `/dev/tty`：写串口、读键盘、拒绝 seek

- **写**：逐字节 `serial_putc`，计数在 `tty_out`。**不上屏**是有意为之：
  显存第 0 页是证据页、第 1 页归 shell 的环形区，再插一个写者进去，
  "屏幕上这一行是谁写的"就说不清了。控制台的可见输出仍只有 shell 一个写者，
  `/dev/tty` 走脱屏通道——和 `printk` 一个字符抄两份是同一个理由。
  判据也因此不能是"串口日志里有没有那几个字节"（日志里混着 printk），
  只能是 `tty_out_bytes()` 这个计数器。
- **读**：从键盘的共享环（`kbd_getchar()`，tag 0）取**一个**字符，没有就
  返回 0。非阻塞是这一版的取舍：真正的 tty 读要睡眠等字符，而睡眠队列
  还没接到"字符到了就唤醒"上，先明确成"这次读到 0 字节"，不装成 EOF。
- **lseek**：一律 `FD_E_ARG`。字符流没有"位置"，接受 seek 才是骗人。

### `/dev/hd`：把 off 翻译成 (lba, 扇内偏移)

`/dev/hd` 是 `blkdev` 里**第一个有扇区的槽**，不是写死的槽 0。

这一条是踩出来的：槽号 = ATA 盘号，而这台机器的数据盘挂在 IDE
**从盘**（`-drive if=ide,index=1`），也就是槽 1；槽 0（主盘）根本不存在。
第一版写死 `HD_DEV 0`，`dev` 命令直接回 `sz=E5`（`FD_E_IO`），
看起来像设备层坏了，其实只是找错了号。改成 `hd_pick()` 找第一个非空槽，
既对这台机器成立，也不需要谁去配置。

- **读**：任意 offset、任意长度，跨扇区逐块拼。单扇区中转用
  `block_dev.c` 自己的 `hd_sec[512]`，**不借 `ata_buffer()`**——那是
  `disk` 命令与 ata 层的共享区，借了就会出现"源和目的同一个缓冲"的覆盖。
- **写**：只收整扇区（`off` 与 `len` 都是 512 的倍数），半扇区改写要
  read-modify-write，留给下一版；越界、非对齐一律 `FD_E_ARG`。
- **绕过 mosfs**：读到的是盘上原始字节，不看 inode。块缓存是**写穿**的
  （`bcache_write` 先落盘），所以"原始设备"与"文件视图"不会看到不同内容。

### 代价：`PAGES_BASE` 抬到 `0x23000`

新增四个 .c（`file_dev` / `char_dev` / `block_dev` + `fd.c` 重写）加上
`dev` 命令，把 `__kernel_end` 从 `0x20CC0` 顶到 `0x218A0`，离
`PAGES_BASE = 0x22000` 只剩 0x800——不足一页，`make` 直接被
`kernel-fit.py` 拦下。抬到 **`0x23000`**：可用页 **110 → 109**
（`frames=…/0x6D`），`tests/expect/boot-screen.txt` 用 `verify.sh --update`
重生成，`tools/verify.py` 的硬断言同步改 `14/0x6D`。

副作用是页分配器给出的物理地址整体后移 `0x1000`，所以 row 11 的
`frames=0x00000004/0x6D`、row 22 的 `14/0x6D`、guard 页那条
`va=0x00030FFC`（原 `0x00031FFC`）都要跟着改。基准文件是代码，
这类"整体平移"就用 `--update` 重生成，再逐条核对确实只动了该动的字段。

### 证据：`dev` 命令四行

```
dev tty w=4                              ← 写 4 字节，tty 计数器 +4
dev hd sz=1048576 n=512 sig=0xAA55       ← 1MB 容量、读到 512 字节、真签名
dev bad=E3 on=0                          ← /dev/xx → FD_E_NOENT，格子收干净
fd open rc=0 fd=0 ino=2 sz=1048576 k=hd  ← fd 命令打开同一设备：设备号 2
```

`fd` 命令顺带多打一个 `k=`（`fd_kind()`），这样"这个 fd 后面接的是谁"
是屏幕上的一等事实，不用去猜 `ino` 的值。

`tools/dev.sh` 把这四行钉成断言，另外两处细节值得记：

1. 造盘**不用** `datadisk.py`：mosfs 的盘把 LBA0 空着（卷从 LBA1 起），
   而 `/dev/hd` 要证明的正是"绕过 mosfs、按扇区读原始字节"，
   所以脚本自己造 1MB 盘，在 LBA0 写图案 + `0x55AA`。
   第一版用 `datadisk.py`，`sig` 读到 `0x0000`——不是设备层错，是盘首本来就空。
2. `fd /dev/hd` 的 `ino` 是**设备号 2**，不是 inode 号：设备文件的
   `ino` 字段存的就是 `DEV_*`，这一点必须在证据里看得见。

### 验证

- `tools/dev.sh`：**PASS**——四行断言全中（自己起机起盘，不依赖别的脚本）。
- `tools/fd.sh`：**PASS** —— Cat/B/C/D/E/F 六段原样通过，
  说明"fd 拆出 fops"没有动普通文件那条路（open 得 fd=0、read 后 off=16、
  ring3 读回 16 字节、任务死后整表归零）。
- `tools/verify.sh -n 2`：**PASS**（24 行 + 区间断言，含新的 `14/0x6D`）。
- 全量 `tools/check-all.sh`：**35/35 PASS**（日志 `/tmp/ca-dev.log`）。
- 负向 `tools/check-neg.sh`：**26/26 PASS**（日志 `/tmp/cn-dev.log`）——
  新加的 `neg.sh -1 dev_hd` 把 `hd_pick()` 退回写死槽 0，
  `dev.sh` 的 hd 行立刻变 `sz=E5` 而红。

## tools 收敛：26 个负向脚本 → 一个（101 → 76 个文件）

`tools/` 长到 101 个文件时，26 个是 `neg_*.sh`：每个 20～26 行，前 20 行
`cp -r 整棵树 → negpatch → make -s → neg_case → 清理` 一模一样，真正的信息只有
「哪个文件、锚点、换成什么、跑哪个正向脚本」三行。三行信息撑起一个文件，
结果就是「加一条用例 = 抄一遍样板」，而且样板一改要改 26 处。

现在合成 `tools/neg.sh`：

- 三个辅助：`mk_tree`（`rm -rf` + `cp -r` 到 `/tmp/neg-<case>`）、
  `build_tree`（`make -s`，失败 `tail -5` 进日志）、`run_on`（按脚本名分派工程根，
  见下）；
- 26 个 `case_<名字>` 函数，函数体只有三件事：`negpatch` 的锚点与替换、
  `build_tree`、`neg_case "<tag>" run_on …`；
- `kbd` / `lease` / `range` 三条各有两个 case，就在同一个函数里连续建两棵树、跑两次
  （原来是两个独立脚本各写一遍样板）；
- `neg.sh -l` 列 case，`neg.sh -1 <name>` 跑单条，`neg.sh <词…>` 按包含匹配挑一组。

`tools/check-neg.sh` 相应缩成「逐条调 `neg.sh -1` + 每行一行结论 + 组级合计」，
不再自己拼 `NEG_TESTS` 文件清单——清单只有 `neg.sh` 里的 `CASES` 一份。

三个老式脚本（`console` / `kbd_shell` / `shell_hist`）也从「原地改源码 → 跑 → 还原」
搬到副本上：中断不再留下半坏的真树。

`run_on` 为什么按名字分支，是这轮唯一的新坑：正向脚本定位工程根有两种方式——
多数读 `$MINI_OS_DIR`；而 `verify.sh` / `mkimg.sh` / `qemu-debug.sh` 用
`dirname($0)/..` 自己算。对后者若只把 `MINI_OS_DIR` 指到变异树，跑的还是**真树里的**
脚本，ROOT 也算回真树，于是「变异做了、测试全绿」——最像假绿的一种。所以
`verify.sh` 这类必须 `bash /tmp/neg-x/tools/verify.sh`，直接跑树里那一份。

### 验证

- `tools/check-neg.sh`：**26/26 PASS**（日志 `/tmp/cn-merge.log`）——合并后的
  `neg.sh` 逐个跑完，`PASS  负向：N 个变异全部被对应正向测试抓住` 逐条命中；
  `kbd`/`lease`/`range` 各报 2 个变异。
- 删文件后抽查 `neg.sh -1 console`：**ok 被抓**（`console_start` 应为 `0x800`，
  变异后为 `0x0`）——证明老式三条迁到副本后仍然咬。
- `tools/verify.sh -n 1`：**PASS**——真树没有被负向测试污染（3 个老式脚本改造的直接验证）。
- 两端一致：`tools/` 各 76 个文件、文件名逐个比对无差异；
  `neg.sh` / `check-neg.sh` / `check-all.sh` / `README.md` 四个 SHA256 两端相同。
- 26 个旧脚本已删（本地临时备份 `%TEMP%\neg-bak-20261123`，不是工程内文件）。

### 还没做的收敛

`tools/` 仍是平铺 76 个文件。往下还有两档，都不影响内核代码：

1. **正向按主题合并**：`kbd-*.sh` 8 个（echo/edit/line/subq/owner/lane/depth/task）
   共用一段「起机 → 等 idle → 注键 → 取屏」，可以像 `neg.sh` 一样合成
   `kbd.sh` 的八张 case 表；`fs*.sh` 5 个同理。这一档能把 76 降到 60 左右。
2. **目录分层**：`tools/{build,qemu,disk,check}/` + 顶层只留 `mkimg.sh`、
   `check-all.sh`、`sshx.py` 几个入口。代价是所有脚本内的 `tools/xxx` 路径、
   `check-all.sh` 的 `TESTS`、Makefile 里的 `tools/linewidth.py` /
   `tools/kernel-fit.py` 都要跟着改，改完得重跑 35 项正向背书。

第一档只动脚本、不动调用关系，风险最低；第二档动的是所有交叉引用，
建议等脚本合并做完、引用点变少之后再动。

## 与 Linux 0.11 看齐：换目录、收头文件（逻辑零改动）

`tools/` 那边收敛的是**工具**，这一轮收的是**源码树本身**。判断标准只有一条：
把目录名和 `#include` 前缀换成 0.11 的样子，编出来的内核应该一个字节都不差。

### 旧结构 → 0.11 结构

| 之前 | 之后 | 为什么 |
| --- | --- | --- |
| `arch/`（4 个 .c + link.ld） | `kernel/` | 0.11 没有 arch/，idt/traps 都在 kernel/ |
| `core/`（8 个 .c） | `kernel/` | printk/panic/klog 就是 0.11 的 kernel/printk.c、panic.c |
| `sched/`（3 个 .c） | `kernel/` | 0.11 只有 kernel/sched.c 一个文件 |
| `sys/syscall.c` | `kernel/syscall.c` | 对应 0.11 kernel/sys_call.s + sys.c |
| `drivers/char/`（7 个 .c） | `kernel/chr_drv/` | 0.11 的 console/keyboard/serial/tty_io 都在这儿 |
| `drivers/block/`（3 个 .c） | `kernel/blk_drv/` | 0.11 的 hd.c / ll_rw_blk.c 在这儿 |
| `include/{arch,core,drivers,fs,init,lib,mm,sched,shell,sys}/` | `include/linux/` + `include/asm/` | 0.11 只有这两个头文件目录 |

顶层目录从 13 个降到 10 个：`boot fs include init kernel lib mm shell tests tools`。
其中 `tests tools` 是 0.11 原树没有的（回归基准与宿主端工具），`shell` 也是自造的——
0.11 的 shell 是挂在文件系统里的用户态程序，我们的命令行跑在内核里，没有对应物。

### 头文件：257 处 `#include` 换前缀

- `include/core/io.h` → `include/asm/io.h`（端口 `in/out`、`cli/sti/hlt` 属于机器指令级，
  对应 0.11 的 `include/asm/io.h`）；
- 其余 50 个头平铺进 `include/linux/`，源码里 `#include "core/printk.h"`、
  `#include "mm/page.h"`、`#include "drivers/char/vga.h"` 一律改成
  `#include "linux/printk.h"` 这种单一前缀；
- 50 个头无重名，所以平铺没有冲突——这也是能一刀切的前提；
- `-Iinclude` 不变，`vpath` 只改目录名，链接顺序完全没动。

### 证据：二进制逐字节不变

```
make clean && make -s
kernel : 114408 bytes @ 0x0A00        ← 与搬家前完全一致
内核装得下：__kernel_end=0x000218A0    ← 一致
布局契约：TASK_SIZE=108 TASK_OFF_TICKS=24 TASK_OFF_ESP0=92   ← 一致
tools/verify.sh -n 1  →  PASS（24 行 + 区间断言）
tools/check-all.sh    →  PASS 35 / FAIL 0（日志 /tmp/ca-v11.log）
tools/check-neg.sh    →  PASS 26 / FAIL 0（日志 /tmp/cn-v11.log）
```

纯搬家就是这样验收的：尺寸、边界、结构体偏移三项都不许动，动了就说明顺手改了逻辑。

### 这一刀的四个坑

1. **tar 会保留 mtime，`make` 因此跳过编译**。上传新树后 `make -s` 一句话没输出、
   `kernel.bin` 时间戳还是旧的——差点拿旧二进制去验收。必须 `make clean`，
   或者 `touch` 全部源码。判断重建没重建，看 `ls -l build/kernel.bin` 的时间戳。
2. **`tools/linewidth.py` 原来只扫一层目录**（`os.listdir(d)`），`kernel/` 有了
   `chr_drv / blk_drv` 两层之后会漏掉 10 个文件。改成 `os.walk` 递归，
   它的 `TARGET_DIRS` 也从 9 个目录名缩成 6 个（`init kernel mm fs lib shell`）。
3. **`tools/neg.sh` 里有 13 处补丁路径**（`drivers/char/vga.c`、`sched/task.c`、
   `drivers/block/part.c` …），路径不跟过去就是 `NOT-FOUND` / 退出码 2，
   负向组会整体报"用例本身失效"。锚点文本本身不受影响——`negpatch.py` 只认字节。
4. **README 里批量替换 `core/` → `kernel/` 会撞上目录树的四行**：原来 include 分组
   写作 `│   ├── core/`、`│   ├── arch/`，一通替换之后目录树里出现四个 `├── kernel/`。
   目录树这类**结构化文本**不能跟着正文正则一起替换，只能整块重写。

### 和 0.11 还差什么

0.11 的 `fs/` 是 16 个文件：`open.c`、`read_write.c`、`file_table.c`、`fcntl.c`、
`pipe.c`、`stat.c`、`ioctl.c`、`truncate.c` + 我们已有的 super/bitmap/inode/namei/
block_dev/char_dev/exec。我们现在把这些收进了 `fd.c`（表 + open/close）与
`file_dev.c`（read/write/lseek），`pipe.c` 已经补上（见下一章），**还没做的只剩
`fcntl.c`**——它要的是 fd 标志位（`O_NONBLOCK` 之类）真正生效。

另外 `mm/` 在 0.11 只有 `memory.c` + `page.s`，我们的 `heap.c / lease.c / pgdir.c /
aspace.c / paging.c` 是 0.11 之后才有的东西——这些不往 0.11 收，保留。

## 管道：`fs/pipe.c`（两个 fd 一个环，非阻塞）

0.11 的 `fs/pipe.c` 把管道挂在 inode 上：`inode->i_pipe` 指向一页缓冲，读写走
`read_pipe` / `write_pipe`，空了 `sleep_on`、满了也 `sleep_on`。我们的内核既没有
inode 缓存池，也没有进程睡眠唤醒（`task_sleep` 还没做），所以这一版**只借它的
分工，不借它的阻塞**。

### 三层分工（和四个 fops 实现并列）

| 层 | 位置 | 干什么 |
| --- | --- | --- |
| 占格子 | `fs/fd.c` `fd_pipe()` | 找两个空格子 → `pipe_new()` → 读端填 `FD_F_READ`、写端填 `FD_F_WRITE`，`fop` 都指 `pipe_fops` |
| 环 | `fs/pipe.c` | 静态 `pipes[8]`，每对 `512` 字节环 + `head` / `tail` / `cnt` |
| 分派 | `fs/fd.c` `fd_read/write` | 取格 → 调 `fop->read/write` → 记账；`file_dev` / `char_dev` / `block_dev` 走的是同一条路 |

`fd.h` 里 `struct file_operations` 现在有四个实例：`file_fops`（盘上文件）、
`tty_fops`、`hd_fops`、`pipe_fops`。管道是第一个**没有 inode、没有路径**的实现：
`fd_file.ino` 存的是环槽号，`name` 固定写 `|pipe`（`0x7C` 在可打印区间，`fd`
命令能照常打出来），`size` 填 `PIPE_BUF` 只是"容量"，不是文件长度。

### 五个设计决定

1. **`cnt` 是唯一满/空判据**。`head == tail` 判空是环形缓冲的经典坑：写满一圈时
   `head` 正好绕回 `tail`，"满"会被当成"空"。负向用例 `pipe_cnt` 就是这个坑——
   把判据换回 `head != tail`，5 字节来回仍然 `ok=1`（表面正常），只有在
   满环那一步 `rd` 从 512 掉到 0 才露馅。
2. **非阻塞**。空环 `read` 返回 0（沿用 `tty_read` 的语义：0 是"没数据"，不是 EOF），
   满环 `write` **短写**：写进去多少就返回多少，剩下的字节计进 `n_drop`。
   走阻塞就必须先有 `task_sleep` / `wakeup` 与信号，那是另一个子系统。
3. **`pipe_new()` 建槽时就置 `readers = 1, writers = 1`**。因为 `fd_pipe()` 一定
   同时建两端，不存在"只建一半"的中间态；`pipe_open` 因此是空函数，
   `pipe_release` 按 `flags` 减对应方向的计数，两端都归 0 才 `pipe_free()`。
4. **`fd_pipe()` 不经过 `fd_open()`**。`fd_open` 认的是"设备名或盘上路径"，
   管道两者都不是；而且它一次只填一格，管道必须原子地占两格。表满与环满都报
   `FD_E_FULL`——对调用方来说，两者能做的都是"先关掉别的东西"，分两个错误码没有意义。
5. **`FD_MAX = 4`，所以同时最多 2 对**。`pipe` 命令每测完一对就关一对，
   所以 `live` 回到 0 才能证明"两格真的还回了 fd 表"。

### 证据

```
$ bash tools/pipe.sh
  已到 kernel_idle（EIP=0x00006726）
  A. pipe r=0 w=1 n=5 ok=1
  B. pipe big w=512 d=88 rd=512 live=0
  C. 再跑一遍：pipe r=0 w=1 n=5 ok=1 / pipe big w=512 d=88 rd=512 live=0
  D. 收工后的 fd 表：fd s0=0 s1=0 s2=0 s3=0 s4=0 o=8 on=0 rd=1034 wr=1034 dn=0
PASS  管道：一对 fd 共享一个 512 字节环（5 字节来回、600 字节写短写 512/丢 88、
      读完 live=0、连跑两遍不积累、两格都还回 fd 表）
```

四段各钉一件事：A 是"两格 + 一个环 + 方向标志"通了；B 是 `w=512`（短写）、
`d=88`（丢弃记账）、`rd=512`（满环真满，不是覆盖）、`live=0`（槽回收）；
C 是同样两行再跑一遍，证明建/拆不积累；D 是 `on=0`，管道占的两格也随 `close` 还了。

负向（`tools/neg.sh -1 pipe_cnt`）：

```
[01] pipe_cnt
      B. pipe big w=512 d=88 rd=0 live=0        ← 满环被 head==tail 判成空
      FAIL  写满那一段不是 (512/88/512/0)
  ok   pipe-empty-by-headtail 被抓（rc=1）
PASS  负向：1 个变异全部被对应正向测试抓住
```

顺带三个计数变了：`tools/` 从 76 个文件变 **77**（多了 `pipe.sh`），
`check-all.sh` 的清单 35 → **36** 项，`check-neg.sh` 26 → **27** 条；
命令表从 28 → **29** 条（`pipe` 长 4）。等事件层又各加一（见末章：
78 个文件 / 37 项 / 28 条 / 30 条命令）。

### 代价：`PAGES_BASE` 从 `0x23000` 抬到 `0x25000`

`pipes[8]` 一对 536 字节（`512` 缓冲 + 三个游标 + 两个计数 + `used`），
8 对占 `0x10C0`。上一轮搬家后 `__kernel_end = 0x218A0`，余量 5984 字节，
加完管道直接到 `0x233A0`——**超过 `0x23000` 这条红线 928 字节**，
`kernel-fit.py` 当场拦下（正是它该干的活）。

抬到 `0x25000`：可用页 **109 → 107**（`frames=…/0x6B`），换 7264 字节余量。
三处基准随之变化，全部是"分配器给出的物理页整体后移 0x2000"的必然结果：

| 行 | 旧 | 新 |
| --- | --- | --- |
| row 7 / 8 缺页落页 | `0x25000` / `0x26000` | `0x27000` / `0x28000` |
| row 9 unmap 的 pa | `0x25000` | `0x27000` |
| row 21 guard 栈顶 va | `0x00031FFC` | `0x00033FFC` |
| row 11 / 22 总帧数 | `…/0x6D` | `…/0x6B` |

基准用 `tools/verify.sh --update` 重生成（`diff` 恰好这 5 行），`tools/verify.py`
的硬断言从 `14/0x6D` 改 `14/0x6B`，`tests/expect/boot-screen.txt` 一起提交。

```
make -s
内核装得下：__kernel_end=0x000233A0，分配区从 0x00025000 起（余量 7264 字节）
kernel : 117000 bytes @ 0x0A00
tools/verify.sh -n 2  →  PASS（重生成后两轮一致）
tools/check-all.sh    →  PASS 36 / FAIL 0（日志 /tmp/ca-pipe.log）
tools/check-neg.sh    →  PASS 27 / FAIL 0（日志 /tmp/cn-pipe2.log）
```

### 和 0.11 还差什么

- **阻塞与唤醒**：0.11 的管道靠 `sleep_on` 让读者等在空环上、写者等在满环上，
  我们这里一律立即返回。要做就得先有 `task_sleep` / `wakeup`（调度器已有
  3 级队列，但还没有"主动让出并挂在某个等待队列上"的接口）。
- **shell 的 `|` 语法**：现在的 `pipe` 命令是自检，不是把 `ls | cat` 的
  两段命令接起来；那需要 fork/exec + 参数里的 fd 重定向。
- **读端先关的语义**：0.11 里写者发现读者全关会返回 `-EPIPE`；
  我们这里写端只在自己被关后报 `FD_E_BADFD`，`readers == 0` 时写仍然成功
  （数据留在环里，反正没人读了）。这一条等阻塞一起改，否则"半开"状态没有意义。
## 等事件：`sleep_on` / `wake_up`（`task_wait_chan` + `task_wakeup`）

上一章最后一行说"阻塞要先把 `task_sleep`/`wakeup` 做出来"。其实管道要的不是
"睡到某个 tick"，是"等某个条件成立"——0.11 里叫 `interruptible_sleep_on(chan)`
和 `wake_up(chan)`。这一轮先把它做成独立、能自检的一层，管道与 shell 的 `|`
留到后面接。

### 两种"睡"的分工

| | `task_sleep(ms)` | `task_wait_chan(chan, cond, ctx)` |
| --- | --- | --- |
| 等什么 | 时间到（tick） | 条件成立（谁改变条件谁叫） |
| 放在哪 | 睡眠堆 sq，`wake_tick` 为键 | 不进任何队列，只把槽位挂在 `chan` 上 |
| 谁叫醒 | `wake_sleepers()` 每 tick 看堆顶 | `task_wakeup(chan)` 扫表 |
| 典型用途 | 延时、退避 | 管道、tty、等另一个任务交结果 |

状态机多一格 `TASK_WAIT`：它既不在就绪队列也不在睡眠堆，所以 `schedule()`
**不需要**为它写分支——`rq_top()` 永远取不到它，`any_worker_alive()` 认它算活着
（少这一条，main 会在还有任务等着的时候提前收工）。

### 为什么 API 要带 cond（这一层唯一的坑）

朴素写法是 `while (!cond()) sleep_on(chan)`。单核不关中断时有个丢失唤醒：

```
A: 检查 cond -> 假
A: 被 tick 抢走，切到 B
B: 置位、wake_up(chan)      ← 此刻 A 还没睡下，扫不到它
A: 回来，sleep_on(chan)     ← 睡死，没人再叫
```

`task_wait_chan` 把"再查一次 cond + 入睡"塞进同一个 `cli` 段，窗口只剩两种：
wakeup 发生在入睡**之前**（cond 已真，直接返回 0，不睡）或**之后**（扫到这个
槽位，搬回就绪队列）。随后 `sti; int $0x30` 切出去，和 `task_sleep` 同一套让出。

几条实现约定：

- `chan` 是任意指针，只当键用，不需要真实对象；两个通道不能同址（这里用
  `&kw.seq` 与 `&kw.ack` 两个不同字段）。
- `task_wakeup` 用 `cpu_save_flags` / `cpu_restore_flags` 成对，因此**可以**
  在关中断上下文（比如时钟中断里的回调）调用，不会破坏调用方的 IF。
- 唤醒时：`state = READY`、清 `chan`、`rq_push`、按当前级别重装时间片；
  不动 `runs`（那是"被选中上场"的计数，由 `schedule` 记）。

### 布局契约跟着动了（脚本不用改）

`task_t` 里加一个 `void *chan`（`wait` 字段已被老化计数占用，不能复用名字）：
`TASK_SIZE` 108 → **112**，`TASK_OFF_ESP0` 92 → **96**，
`arg` / `kbd_tag` / `esp0_stack` 依次 +4。

`shell-tasks.sh` / `userpg.sh` / `ring3pre.sh` / `lease.sh` 全部从
`build/layout.env` 取值，**一行都没改**——这就是上一轮布局契约的直接回报。

### 自检：两个 worker 用一个信箱 ping-pong 16 轮

`kernel/kwait.c`：writer 摆数据 → `wakeup(c_data)` → 等 `c_ack`；reader 被叫醒 →
校验数据 → `wakeup(c_ack)` → 等下一轮。`kwait` 命令跑一趟，两行证据。

```
$ bash tools/kwait.sh
  已到 kernel_idle（EIP=0x00006726）
  A. kwait n=16 ok=1 to=0
  B. kwait w=31 k=31 left=0
  C. t1 unused p=1 s=0/4 c=0x000000 r=17 e=0x000000 u=0 / t2 unused p=1 s=0/4 c=0x000000 r=16 e=0x000000 u=0 / 仍在等的槽位=0
PASS  等事件：16 轮 ping-pong 全过（数据逐轮对、无超时兜底、
      等待/唤醒 31/31 配对、收工后没有槽位挂在通道上）
```

四段各钉一件事：

- `n=16 ok=1`：16 轮数据（`i*7+3`）逐轮对上——唤醒之后真的跑到、读到了对端
  刚摆下的东西（错一个字节整趟算失败）；
- `to=0`：没走超时兜底。**靠 abort 才退出的绿是假绿**，必须单独排除；
- `w=31 k=31`：每轮 reader 睡一次、前 15 轮 writer 睡一次（最后一轮它不等确认），
  31 对等待/唤醒一一配对；脚本另有一条 `k >= 16`，防"条件总提前成立"的空跑；
- `left=0` 且 `t1/t2 unused`、`e=0 u=0`：没有槽位睡死在通道上，栈页与 esp0 页都还了。

超时兜底在 main 侧：等两端时用 200 tick（2 秒）上限，超了就置 `abort` 并把
两端全叫醒（`cond` 里认 `abort`），避免一次丢唤醒把 shell 永久挂住、
回归只能靠 timeout 猜。

### 负向：咬"行为"，不咬"形状"

第一版变异是"唤醒时把 `state` 写成 `TASK_WAIT` 而不是 `TASK_READY`"——
**没被抓**。原因是等价变异：任务已经靠 `rq_push` 进了就绪队列，`state` 只影响
"时间片用尽后要不要回队"，而 ping-pong 每一步都主动 wait，永远用不满一片。

换成"筛选条件写错"（把 `TASK_WAIT` 判成 `TASK_READY`）就抓住了：没人被搬回
就绪队列 → 卡在倒数第二轮 → 屏幕上 `kwait w=0 k=2 left=2`、`t1/t2 wait`。

这条得记进负向组的总则：**变异必须咬行为差，不能咬代码形状差**。

### 顺带修一个假红

`userpg.sh` 有一条硬断言"默认栈顶 < 0x20000"，那是"内核映像全程在 0x20000
以下"的时代留下的阈值。这一轮 `.bss` 长过 0x20000，默认栈顶变成 `0x20810`，
脚本直接假红。判据改成 `<= __kernel_end`（从 `nm` 取，和 `kernel-fit.py`
同一个边界），`ring3` 采样窗口的下界也从 `0x20000` 改成 `0x25000`（真分配区起点）。

### 还差什么

- **管道没接阻塞**：`pipe_read` / `pipe_write` 仍是"空读返 0 / 满则短写"。
  接上去只剩三处：空环满环时 `task_wait_chan`、两端读写后 `task_wakeup`、
  以及"读端全关 → 写端报 EPIPE"。
- **shell 的 `|`**：需要 fork/exec + 把两端 fd 塞进子任务；`task_wait_chan`
  是它的前置，不是它本身。
- **`fcntl`**：`O_NONBLOCK` 生效之后，同一个管道既能阻塞也能非阻塞，两套语义共存。