#!/usr/bin/env python3
"""检查自检界面每行的列宽（VGA 文本模式 80 列，可用 79 列）。

为什么需要它：屏幕 25 行里 24 行给了自检，一行最多 79 列——正好 80 列时行尾的
'\\n' 会把光标顶到下一行，凭空多出一行，第 25 行触发上滚，banner 被挤出屏幕。
这件事以前靠肉眼数格子，错过两次（banner 被顶掉、多出一行空白）。

约定（与 include/init/report.h 一致）：
  * 一行由**一次** printk 打完，格式串以 '\\n' 结尾；
  * 十六进制一律定宽：%08X / %03X / %02X / %01X；
  * %s 按 0 列算（只用在“二选一短语”这类窄字段上，两侧留了余量）；
  * %X / %x 没有给宽度时按 8 列算（最坏情况），逼着作者写定宽。

用法：python3 tools/linewidth.py            # 只在超宽/无法解析时报错
      python3 tools/linewidth.py -v         # 顺带打印每行宽度
"""
import os
import re
import sys

LIMIT = 79

# 会 printk 出整行的所有目录：界面字符串原来只在 init/ 里，后来 mm/page.c 的
# `[mem] FATAL`、kernel/chr_drv/ 的告警、kernel/exception.c 的异常现场也都能落满一行。
# 所以现在是“谁的 printk 带 '\n' 就查谁”，一个目录都不漏；kernel/ 下还有
# chr_drv/ 与 blk_drv/ 两层，所以按目录递归走。
# shell/ 的字符串是运行期拼的（用户名、变量值），宽度随输入变，静态算不出来——
# 由 SHELL_OUT_COLS 在运行时兜底，这里只查它的 printk（如果有）。
TARGET_DIRS = ["init", "kernel", "mm", "fs", "lib", "shell"]
TARGET_EXTRA = []

ESC_WIDTH = {"n": 0, "t": 0, "r": 0, "0": 0, "\\": 1, "\"": 1, "'": 1}
SPEC_RE = re.compile(r"%(-?)(\d*)([sdcuxX%])")
STRING_RE = re.compile(r'"((?:[^"\\]|\\.)*)"')


def literal_width(lit: str) -> int:
    """把一段 C 字符串字面量（不含两侧引号）折算成屏幕列数。"""
    width = 0
    i = 0
    n = len(lit)
    while i < n:
        ch = lit[i]
        if ch == "\\" and i + 1 < n:
            esc = lit[i + 1]
            width += ESC_WIDTH.get(esc, 1)
            i += 2
            continue
        if ch == "%":
            m = SPEC_RE.match(lit, i)
            if not m:
                width += 1                      # 孤立的 '%'
                i += 1
                continue
            spec = m.group(3)
            digits = m.group(2)
            if spec == "%":
                width += 1
            elif spec == "s":
                width += 0                      # 见文件头约定
            elif digits:
                width += int(digits)
            else:
                width += 8                      # %X / %x 未给宽度：按最坏情况算
            i = m.end()
            continue
        width += 1
        i += 1
    return width


def printk_calls(path):
    """取出每个 printk(...) 调用里的第一段字符串字面量（含列号）。"""
    src = open(path, encoding="utf-8").read()
    for m in re.finditer(r"\bprintk\s*\(", src):
        start = m.end()
        depth = 1
        i = start
        while i < len(src) and depth:
            if src[i] == "(":
                depth += 1
            elif src[i] == ")":
                depth -= 1
            i += 1
        body = src[start:i - 1]
        line = src.count("\n", 0, m.start()) + 1
        lits = STRING_RE.findall(body)
        if not lits:
            continue
        # 只取第一个字面量：后续的也是同一行里的拼接，但我们约定“一行一次 printk”，
        # 所以第一段必须已经包含 '\n'；不含的就是半行（跳过，不算行宽）。
        yield line, lits[0]


def targets():
    """按目录发现目标文件，顺序固定（便于 -v 输出可比对）。

    kernel/ 下分 chr_drv / blk_drv 两层，所以用 os.walk 递归，不再只看一层。
    """
    found = []
    for d in TARGET_DIRS:
        for root, dirs, files in os.walk(d):
            dirs.sort()
            found.extend(sorted(os.path.join(root, n) for n in files
                                if n.endswith(".c")))
    found.extend(TARGET_EXTRA)
    return found


def main(argv):
    verbose = "-v" in argv
    bad = 0
    checked = 0
    for path in targets():
        for line, lit in printk_calls(path):
            if not re.search(r"\\n", lit):
                continue                        # 半行（标签/尾巴）不参与行宽
            w = literal_width(lit)
            checked += 1
            if verbose:
                print("%-3d %s:%d" % (w, path, line))
            if w > LIMIT:
                bad += 1
                print("FAIL %s:%d 行宽 %d > %d" % (path, line, w, LIMIT), file=sys.stderr)
                print("     %s" % lit[:60], file=sys.stderr)
    if bad:
        print("列宽检查失败：%d 行超宽（上限 %d 列）" % (bad, LIMIT), file=sys.stderr)
        return 1
    print("列宽检查通过：%d 行，全部 ≤ %d 列" % (checked, LIMIT))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
