#!/usr/bin/env python3
"""把 build/layout.bin（= kernel.elf 里 .layout 段剥出来的 u32 串）翻译成 shell 变量。

Usage: python3 tools/layout.py build/layout.bin > build/layout.env   （make 里自动做）

为什么绕这一道：回归脚本要用 `nm` 找到 `tasks` 之后，再按偏移读字段。偏移是
C 编译器的答案，脚本不该自己抄一遍；也不该为此去跑一个 32 位宿主机程序
（那要 32 位 libc）。用 `objcopy --only-section` 直接把编译器的答案剥出来最快：
它不执行任何代码，也不需要 debug 信息。

GENERATED 文件：由 `make image` 生成，不要提交、不要手改。
"""
import struct
import sys

# 顺序必须与 kernel/layout.c 里 layout_words[] 一致（一一对应，改一处要改两处）
FIELDS = [
    ("LAY_MAGIC", "magic"),
    ("TASK_SIZE", "task_size"),
    ("TASK_OFF_ESP", "off_esp"),
    ("TASK_OFF_TICKS", "off_ticks"),
    ("TASK_OFF_ESP0", "off_esp0"),
    ("TASK_OFF_ESP0_STACK", "off_esp0_stack"),
    ("TASK_OFF_ARG", "off_arg"),
    ("TASK_OFF_DTOR", "off_dtor"),
    ("TASK_OFF_STATE", "off_state"),
    ("TASK_OFF_DEATH", "off_death"),
    ("TASK_OFF_PRIO", "off_prio"),
    ("TASK_OFF_BASE_PRIO", "off_base_prio"),
    ("TASK_OFF_SLICE", "off_slice"),
    ("TASK_OFF_LEFT", "off_left"),
    ("TASK_OFF_RUNS", "off_runs"),
    ("TASK_OFF_KBD_TAG", "off_kbd_tag"),
    ("TASK_OFF_LEASES", "off_leases"),
    ("TASK_OFF_UCODE", "off_ucode"),
    ("TASK_OFF_UPAGES", "off_upages"),
    ("TASK_OFF_USTACK", "off_ustack"),
    ("TASK_SLOTS", "slots"),
    ("TASK_LEASES", "leases"),
    ("TASK_PRIO_LEVELS", "levels"),
]
MAGIC = 0x4C415941


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    with open(sys.argv[1], "rb") as f:
        raw = f.read()
    n = len(raw) // 4
    vals = list(struct.unpack("<%dI" % n, raw[:n * 4]))
    if n != len(FIELDS):
        sys.exit("FAIL layout.bin 有 %d 个字，期望 %d（kernel/layout.c 与 tools/layout.py 不一致）"
                 % (n, len(FIELDS)))
    if vals[0] != MAGIC:
        sys.exit("FAIL .layout 段首字是 0x%08x，不是 LAYA——段被挪走或合并了" % vals[0])
    print("# 由 tools/layout.py 从 build/layout.bin 生成（= kernel/layout.c 的答案），别手改")
    for (name, _), v in zip(FIELDS[1:], vals[1:]):
        print("%s=%d" % (name, v))


if __name__ == "__main__":
    main()
