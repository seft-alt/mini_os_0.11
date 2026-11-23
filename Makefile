# ==============================================================
# mini-os 构建脚本（Linux：nasm + gcc + ld -m elf_i386）
#
#   make            # 生成 build/mini-os.img
#   make run        # qemu-system-i386 图形窗口启动
#   make debug      # 无头 QEMU + gdb 远程调试（断点 / 寄存器 / 还原屏幕）
#   make clean
#
# 源码在哪个目录由 Linux 0.11 决定（见 README「模块划分」）：
#   boot/          引导三段 + 中断桩 + 用户程序探针（nasm）
#   init/          启动与自检（main / demo / report）
#   kernel/        与设备无关的一切：中断门、异常、栈、TSS、printk、panic、
#                  调度、系统调用、时间基准、布局契约、链接脚本
#   kernel/chr_drv/ 字符设备：VGA、键盘、串口、定时器、RTC
#   kernel/blk_drv/ 块设备：ATA、块设备表、分区
#   mm/            物理页分配器 + 按需分页 + 每任务页目录 + 租约 + 堆
#   fs/            一切"文件从哪儿来"：super / bitmap / inode / namei / 块缓存 /
#                  exec / elf / fd / file_dev / char_dev / block_dev
#   lib/           string.c（-fno-builtin）与 time.c
#   include/linux/ 全部内核头（0.11 是单一 linux/ 目录，不再是 per-module）
#   include/asm/   只放机器指令级的东西（端口 IO）
#   shell/         自造：内核里的命令行（0.11 的 shell 是用户态程序，我们没有对应）
# 目标文件仍平铺在 build/ 下：链接顺序比目录结构更重要。
# ==============================================================
NASM    := nasm
CC      := gcc
LD      := ld
OBJCOPY := objcopy

BUILD   := build
IMG     := $(BUILD)/mini-os.img
KERNEL  := $(BUILD)/kernel.elf

CFLAGS  := -m32 -ffreestanding -fno-pic -mno-sse -mno-sse2 -mno-mmx \
           -fno-stack-protector -fno-asynchronous-unwind-tables -fno-ident \
           -O2 -nostdlib -Wall -Werror -Iinclude -MMD -MP -fstack-usage

# C 源文件分散在这几个模块目录里；vpath 让 $(BUILD)/%.o: %.c 一条规则通吃
SRC_DIRS := init kernel kernel/chr_drv kernel/blk_drv mm fs lib shell
vpath %.c $(SRC_DIRS)

# 链接顺序：main.o（入口）在最前，printk.o / stat.o 紧跟——它们是最早被调到的输出路径
C_OBJS  := $(addprefix $(BUILD)/,main.o printk.o panic.o stat.o layout.o vector.o klog.o kv.o serial.o heap.o report.o demo.o idt.o exception.o stack.o clock.o \
                                  page.o paging.o pgdir.o lease.o aspace.o task.o queue.o tune.o tss.o vga.o rtc.o timer.o \
                                  keyboard.o kbd_queue.o kbdline.o shell.o state.o var.o console.o fmt.o cmd_fs.o cmd_prog.o cmd_info.o cmd_time.o ata.o blkdev.o part.o super.o blkio.o bitmap.o inode.o namei.o mosfs.o fd.o file_dev.o char_dev.o block_dev.o pipe.o kwait.o exec.o elf.o syscall.o string.o time.o bcache.o)
DEPS    := $(C_OBJS:.o=.d)

# 链接顺序有意义：head.o 必须在最前，其 .text 起点即 pg_dir（物理 0）
OBJS    := $(BUILD)/head.o $(BUILD)/intr.o $(C_OBJS)

.PHONY: all image width run debug verify stack clean

# 列宽检查先跑：它只读源码，超宽就在编译前失败，不用等 QEMU 跑一遍才发现
all: width image

width:
	@python3 tools/linewidth.py

$(BUILD):
	mkdir -p $(BUILD)

# 引导扇区：必须正好 512 字节，末尾 0xAA55
$(BUILD)/boot.bin: boot/boot.asm | $(BUILD)
	$(NASM) -f bin $< -o $@

# setup：占 4 个扇区（2048 字节），实际内容由 mkimg.sh 补齐
$(BUILD)/setup.bin: boot/setup.asm | $(BUILD)
	$(NASM) -f bin $< -o $@

$(BUILD)/head.o: boot/head.asm | $(BUILD)
	$(NASM) -f elf32 $< -o $@

$(BUILD)/intr.o: boot/intr.asm | $(BUILD)
	$(NASM) -f elf32 $< -o $@

# 示例用户程序：纯二进制（不进内核），由 datadisk.py 当作 mosfs 里的一个文件装进数据盘
$(BUILD)/uprog.bin: boot/uprog.asm | $(BUILD)
	$(NASM) -f bin $< -o $@

# 负向用例：指针越界 + 不写指纹（exec.sh 的第二趟用它证明"关真的在拦"）
$(BUILD)/badprog.bin: boot/badprog.asm | $(BUILD)
	$(NASM) -f bin $< -o $@

# ring3 里赖着不走的探针：userpg.sh 在它跑着的时候采样 TSS.esp0，
# 断言"那时候的 esp0 是这个任务自己的页，而不是默认栈"
$(BUILD)/uprog3.bin: boot/uprog3.asm | $(BUILD)
	$(NASM) -f bin $< -o $@

# 参数传递的探针：exec-argv.sh 用它证明 argc/argv 真的递到了 ring3
$(BUILD)/uprog4.bin: boot/uprog4.asm | $(BUILD)
	$(NASM) -f bin $< -o $@

# fd 层的探针：fd.sh 用它证明 ring3 能 open/read/write/close 盘上的真文件
$(BUILD)/uprog5.bin: boot/uprog5.asm | $(BUILD)
	$(NASM) -f bin $< -o $@

# 高地址探针：pghigh.sh 用它证明"用户态碰 16MB 以上一律当越权"（不补页、不停机）
$(BUILD)/uprog6.bin: boot/uprog6.asm | $(BUILD)
	$(NASM) -f bin $< -o $@

# ELF 版示例程序：同一段源码先编成 elf32 目标，再按 tools/uprog.ld 拼成
# "p_offset == p_vaddr == 0、总共装得进一页"的 ET_EXEC（默认 ld 会把 .text
# 放到文件偏移 0x1000，那样文件就超过了一页）。
$(BUILD)/uprog.elf: boot/uprog.asm tools/uprog.ld | $(BUILD)
	$(NASM) -f elf32 $< -o $(BUILD)/uprog_o.o
	$(LD) -m elf_i386 -T tools/uprog.ld -o $@ $(BUILD)/uprog_o.o

# 跨页版：同一个 ELF 格式，但 .bss 被 tools/uprog2.ld 摆到 0x1800，
# p_memsz 越过 4KB，exec 必须给它一段连续两页（page_alloc_run(2)）。
$(BUILD)/uprog2.elf: boot/uprog2.asm tools/uprog2.ld | $(BUILD)
	$(NASM) -f elf32 $< -o $(BUILD)/uprog2_o.o
	$(LD) -m elf_i386 -T tools/uprog2.ld -o $@ $(BUILD)/uprog2_o.o

# 写保护探针：同一套 ELF 拼装，但 tools/uprog7.ld 的 PHDRS 带 FLAGS(5)（无 PF_W），
# 于是 exec 把整页标成只读；它第一条指令就写自己的代码页 → 期望 #PF 越权。
$(BUILD)/uprog7.elf: boot/uprog7.asm tools/uprog7.ld | $(BUILD)
	$(NASM) -f elf32 $< -o $(BUILD)/uprog7_o.o
	$(LD) -m elf_i386 -T tools/uprog7.ld -o $@ $(BUILD)/uprog7_o.o

# 一条规则覆盖全部模块目录（靠上面的 vpath 找到源文件）
$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

# lib/string.c 必须关掉内建优化：否则 -O2 会把 memset 里的字节循环又变成一次
# 对 memset 的调用（自己调自己），清个缓冲区就栈溢出。这条显式规则优先于上面的模式规则。
$(BUILD)/string.o: lib/string.c | $(BUILD)
	$(CC) $(CFLAGS) -fno-builtin -c $< -o $@

# 内核：head.o 的 .text 必须位于物理 0，由 kernel/link.ld 保证；ELF 只作符号来源，
# objcopy 剥成纯二进制后进镜像
$(KERNEL): $(OBJS) kernel/link.ld
	$(LD) -m elf_i386 -T kernel/link.ld -o $@ $(OBJS)

# 剥成纯二进制之前先验一遍：内核（含 .bss）不能长进物理页分配区，
# 否则分配器 zero_page() 会把 vga 的 cursor/line_len 清零（屏幕上滚错位的根源）
$(BUILD)/kernel.bin: $(KERNEL) tools/kernel-fit.py mm/page.c
	@python3 tools/kernel-fit.py $<
	$(OBJCOPY) -O binary $< $@

image: $(IMG) $(BUILD)/layout.env

# 布局契约：结构体偏移的唯一来源（见 kernel/layout.c 的说明）。
# 先把 .layout 段整个剥成一串 u32，再由 layout.py 翻译成 shell 变量。
# 回归脚本 source build/layout.env，不再自己写 108 / +24 / +92。
$(BUILD)/layout.bin: $(KERNEL)
	$(OBJCOPY) -O binary --only-section=.layout $< $@

$(BUILD)/layout.env: $(BUILD)/layout.bin tools/layout.py
	python3 tools/layout.py $< > $@
	@grep -E '^(TASK_SIZE|TASK_OFF_TICKS|TASK_OFF_ESP0)=' $@ | sed 's/^/布局契约：/' | tr '\n' ' '
	@echo

$(IMG): $(BUILD)/boot.bin $(BUILD)/setup.bin $(BUILD)/kernel.bin $(BUILD)/uprog.bin $(BUILD)/uprog.elf $(BUILD)/uprog2.elf $(BUILD)/badprog.bin $(BUILD)/uprog3.bin $(BUILD)/uprog4.bin $(BUILD)/uprog5.bin $(BUILD)/uprog6.bin $(BUILD)/uprog7.elf tools/mkimg.sh
	bash tools/mkimg.sh

run: $(IMG)
	qemu-system-i386 -fda $(IMG) -boot a -m 16

# gdb 远程调试：qemu gdbstub（-S 挂起等 gdb）+ gdb 批量脚本，断在 kernel_halt
debug: $(IMG)
	bash tools/qemu-debug.sh

# 回归自检：屏幕 24 行结构比对 + 抖动项区间断言（默认跑 2 轮）
verify: $(IMG)
	bash tools/verify.sh

# 栈用量报告：CFLAGS 里的 -fstack-usage 会为每个 C 文件产出 build/<name>.su
stack: $(OBJS)
	python3 tools/stack-report.py $(BUILD)

# 全量回归：屏幕证据 + 五个 shell/ring3 端到端脚本，全部串行跑（QEMU 只能一个一个起）
check: $(IMG) $(BUILD)/layout.env
	bash tools/check-all.sh

clean:
	rm -rf $(BUILD)

-include $(DEPS)

