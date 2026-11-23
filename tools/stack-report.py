#!/usr/bin/env python3
"""-fstack-usage 产出的 build/*.su 汇总：谁最吃栈、有没有动态栈。

内核栈 4KB（`kernel_stack[1024]`），任务栈一页 4KB。这两个数字是硬上限，
但没有 DWARF 的情况下 gdb 也算不出“最坏调用链”，所以这里只做能真做的事：

* 列出每个函数自己的栈帧（最大的在前面）；
* 标出 `dynamic` 一列，但**不把它当告警**：在 -m32 且禁用 SSE 的构建里，GCC 只要
  发现函数需要运行期栈对齐（含 16 字节对齐的局部变量）就会标 dynamic，
  和 alloca/VLA 是同一个标记。真正要担心的是“帧特别大”或“源码里出现 VLA”，
  前者这里直接管，后者由 `grep -n '\[[a-z_]+\]'` 人工扫一眼即可；
* 给出“单帧占栈比例”，任何一帧超过 1/8（512 字节）就报警：一帧这么大，
  两条调用叠起来就吃掉四分之一，剩下的余量不足以叫“安全”。

用法：tools/stack-report.py build
"""

import os
import re
import sys

STACK_BYTES = 4096
WARN_FRAME = 512

# 形如 build/main.su 里的一行：
#   init/main.c:150:5:main	96	static
LINE_RE = re.compile(r"^(?P<file>[^:]+):(?P<line>\d+):(?P<col>\d+):"
                     r"(?P<fn>[^\t]+)\t(?P<size>\d+)\t(?P<qual>\w+)")


def collect(build_dir):
    entries = []
    for name in sorted(os.listdir(build_dir)):
        if not name.endswith(".su"):
            continue
        path = os.path.join(build_dir, name)
        with open(path, encoding="utf-8", errors="replace") as fh:
            for raw in fh:
                m = LINE_RE.match(raw.rstrip("\n"))
                if not m:
                    continue
                # GCC 偶尔把函数名写成 "file.c:12:3:inner" 形式（内联/静态函数），取最后一段
                fn = m.group("fn").split(":")[-1]
                entries.append((
                    int(m.group("size")),
                    m.group("qual"),
                    fn,
                    f"{m.group('file')}:{m.group('line')}",
                ))
    return entries


def main():
    build_dir = sys.argv[1] if len(sys.argv) > 1 else "build"
    if not os.path.isdir(build_dir):
        print(f"no such directory: {build_dir}", file=sys.stderr)
        return 1

    entries = collect(build_dir)
    if not entries:
        print("no .su files - 确认 CFLAGS 里带了 -fstack-usage 并重新 make", file=sys.stderr)
        return 1

    entries.sort(reverse=True)
    print(f"{'bytes':>7}  {'% of 4KB':>8}  {'qual':<8}{'function':<28}where")
    print("-" * 72)
    for size, qual, fn, where in entries[:15]:
        pct = size * 100.0 / STACK_BYTES
        print(f"{size:>7}  {pct:>7.1f}%  {qual:<8}{fn:<28}{where}")
    if len(entries) > 15:
        print(f"... 其余 {len(entries) - 15} 个函数省略")

    worst = entries[0]
    print()
    print(f"函数总数: {len(entries)}    最大单帧: {worst[0]} 字节 ({worst[2]})")
    print(f"单帧上限（4KB 栈）: {STACK_BYTES} 字节，报警线 {WARN_FRAME} 字节")

    fails = []
    if worst[0] > WARN_FRAME:
        fails.append(f"最大单帧 {worst[0]} 字节 > {WARN_FRAME}，调用链只剩很少余量")

    if fails:
        print("WARN")
        for f in fails:
            print("  - " + f)
        return 0          # 只警告不失败：栈帧大小不是回归项，别卡住构建
    print("OK  没有超过报警线的单帧")
    return 0


if __name__ == "__main__":
    sys.exit(main())
