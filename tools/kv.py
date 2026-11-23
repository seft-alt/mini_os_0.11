#!/usr/bin/env python3
"""从串口日志里取一条 KV 记录（kernel/kv.c 的输出）。

用法：
  python3 tools/kv.py <serial.log> <tag>            # 打印最后一条：k=v k=v ...
  python3 tools/kv.py <serial.log> <tag> <key>      # 只打印那一个值
  python3 tools/kv.py <serial.log> <tag> --index N   # 取第 N 条（1 起；负数从尾部算）

退出码：
  0 找到；2 没有这个 tag 的记录；3 有记录但没有这个 key；4 参数不对。

为什么不直接 grep：KV 行的 key 可以重复、值里可能有 `=` 之外的怪字符，
而且脚本常常要"第二条 pgdir"这种按序取。把这些约定收在一个脚本里，
回归脚本就只剩断言，不再各自写一套正则。
"""
import sys


def records(path, tag):
    try:
        with open(path, "rb") as fh:
            text = fh.read().decode("utf-8", "replace")
    except FileNotFoundError:
        return []
    out = []
    prefix = "KV " + tag + " "
    for line in text.splitlines():
        # 串口可能混进 \r 或早先被截断的半行，两种都不收
        line = line.rstrip("\r")
        if line.startswith(prefix):
            out.append(line[len(prefix):].strip())
    return out


def main():
    argv = sys.argv[1:]
    if len(argv) < 2:
        sys.exit(__doc__)

    path, tag = argv[0], argv[1]
    index = None
    key = None
    rest = argv[2:]
    i = 0
    while i < len(rest):
        a = rest[i]
        if a == "--index" and i + 1 < len(rest):
            try:
                index = int(rest[i + 1])
            except ValueError:
                sys.exit("--index 要一个整数")
            i += 2
            continue
        if key is None:
            key = a
            i += 1
            continue
        sys.exit("多余的参数：%s" % a)

    recs = records(path, tag)
    if not recs:
        sys.exit(2)
    try:
        # 1 起算；负数从尾部算（-1 = 最后一条）。省略 --index 也取最后一条。
        rec = recs[-1] if index is None else recs[index - 1]
    except IndexError:
        sys.exit(2)

    if key is None:
        print(rec)
        return

    val = None
    for kvs in rec.split():
        if "=" not in kvs:
            continue
        k, v = kvs.split("=", 1)
        if k == key:
            val = v              # 重复 key 取最后一个（后写的覆盖前者）
    if val is None:
        sys.exit(3)
    print(val)


if __name__ == "__main__":
    main()
