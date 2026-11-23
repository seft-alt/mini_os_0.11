#!/bin/bash
# 一次性收口：make → verify 基准刷新 → verify 复核 → 三个键盘负向
# （放到 VM 上 nohup 跑，输出 /tmp/vrun.out；本地只负责短命令轮询）
set -u
cd "${MINI_OS_DIR:-$HOME/mini-os}" || exit 1
killall -q -9 qemu-system-i386 2>/dev/null

echo "=== make ==="
make -s 2>&1 | tail -2

echo "=== verify --update -n 2 ==="
bash tools/verify.sh --update -n 2 2>&1 | tail -4

echo "=== verify -n 2 ==="
bash tools/verify.sh -n 2 2>&1 | tail -4

echo "=== neg_kbd_echo ==="
bash tools/neg_kbd_echo.sh 2>&1 | tail -3

echo "=== neg_kbd (no-gate / no-unmask) ==="
bash tools/neg_kbd.sh 2>&1 | tail -8

echo "=== neg_kbd_line ==="
bash tools/neg_kbd_line.sh 2>&1 | tail -6

echo "=== neg_kbd_owner ==="
bash tools/neg_kbd_owner.sh 2>&1 | tail -6

echo "=== neg_kbd_task ==="
bash tools/neg_kbd_task.sh 2>&1 | tail -6

echo "=== done ==="
