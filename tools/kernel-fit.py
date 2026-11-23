#!/usr/bin/env python3
"""检查内核映像没有长进物理页分配器的地盘。

为什么需要它：分配器管 0x20000–0x90000，它分配出去的第一件事就是 zero_page——
把整页抹成 0。内核的 .bss 里躺着 `cursor`、`line_len` 这类驱动状态，一旦映像
尾部越过分配区的起点，清零就变成“把光标位置抹掉”，屏幕从第 0 行开始覆写，
看起来像整屏上滚了几行（这个坑真的踩过一次：0x10010 处的 vga 静态变量被
zero_page(0x10000) 清零，banner 消失、24 行基准整体错位）。

判据用链接脚本给的 __kernel_end（.bss 之后），并且留一页余量：
  * 越过 PAGES_BASE        → 失败（现在就踩）
  * 只剩不到 1 页          → 失败（下一次加个变量就又踩）

用法：python3 tools/kernel-fit.py build/kernel.elf
"""
import re
import subprocess
import sys

SLACK = 4096


def nm_symbols(path):
    out = subprocess.run(["nm", "-n", path], capture_output=True,
                         text=True, check=True).stdout
    syms = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 3:
            try:
                syms[parts[2]] = int(parts[0], 16)
            except ValueError:
                continue
    return syms


def pages_base(path="mm/page.c"):
    src = open(path, encoding="utf-8").read()
    m = re.search(r"#define\s+PAGES_BASE\s+(0x[0-9A-Fa-f]+|\d+)", src)
    if not m:
        sys.exit("kernel-fit: 在 %s 里找不到 PAGES_BASE" % path)
    return int(m.group(1), 0)


def main(argv):
    elf = argv[0] if argv else "build/kernel.elf"
    syms = nm_symbols(elf)
    if "__kernel_end" not in syms:
        sys.exit("kernel-fit: %s 里没有 __kernel_end（kernel/link.ld 改了？）" % elf)
    end = syms["__kernel_end"]
    base = pages_base()
    limit = base - SLACK

    if end >= base:
        print("内核越界：__kernel_end=0x%08X ≥ PAGES_BASE=0x%08X" % (end, base),
              file=sys.stderr)
        print("  分配器 zero_page() 会把内核 .bss 清零（光标位置就在里面）",
              file=sys.stderr)
        print("  修法：把 mm/page.c 的 PAGES_BASE 往下抬到 0x%08X 之上"
              % ((end + SLACK + 0xFFF) & ~0xFFF), file=sys.stderr)
        return 1
    if end > limit:
        print("内核贴边：__kernel_end=0x%08X，离 PAGES_BASE=0x%08X 不足一页"
              % (end, base), file=sys.stderr)
        return 1
    print("内核装得下：__kernel_end=0x%08X，分配区从 0x%08X 起（余量 %d 字节）"
          % (end, base, base - end))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
