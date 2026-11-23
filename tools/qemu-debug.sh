#!/usr/bin/env bash
# QEMU + GDB remote debugging for mini-os (Linux / macOS).
#
# Boots the floppy image with qemu's gdb stub stopped at reset (-S), attaches
# gdb, breaks at kernel_halt, prints registers and dumps the VGA text buffer to
# a file, which tools/vga-screen.py then renders as a 80x25 screen.
#
# The breakpoint address comes from nm, so it survives code edits.
#
# Usage:  tools/qemu-debug.sh [kernel-symbol-file]
# Prereq: qemu-system-i386, gdb, nm, python3

set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD="$ROOT/build"
IMG="$BUILD/mini-os.img"
PORT=${GDB_PORT:-1234}

# Windows builds leave kernel.exe (PE), Linux builds leave kernel.elf (ELF);
# nm reads both, so accept whichever exists.
SYMS=${1:-}
if [ -z "$SYMS" ]; then
    for cand in "$BUILD/kernel.elf" "$BUILD/kernel.exe"; do
        [ -f "$cand" ] && SYMS=$cand && break
    done
fi

[ -f "$IMG" ]  || { echo "missing $IMG - run 'make' first" >&2; exit 1; }
[ -n "$SYMS" ] || { echo "missing kernel.elf/kernel.exe - run 'make' first" >&2; exit 1; }

command -v qemu-system-i386 >/dev/null || { echo "qemu-system-i386 not installed" >&2; exit 1; }
command -v gdb >/dev/null || { echo "gdb not installed" >&2; exit 1; }

# 注意这里的 awk **不能写 exit**：`nm -n` 的输出是一整条流，awk 一 exit 就会关掉
# 读端，nm 拿到 SIGPIPE（141），而本脚本开着 pipefail —— 于是赋值失败、脚本
# 在打印任何东西之前就退出（表现为"日志 0 字节、rc=141"）。内核变大之后
# nm 要写的行更多，这个竞态从"偶尔"变成"一半"。所以用标志位读完整条流。
ADDR=$(nm -n "$SYMS" | awk '$3=="kernel_idle" && !f {print "0x"$1; f=1}')
if [ -z "$ADDR" ]; then
    ADDR=$(nm -n "$SYMS" | awk '$3=="kernel_halt" && !f {print "0x"$1; f=1}')
fi
[ -n "$ADDR" ] || { echo "symbol kernel_idle/kernel_halt not found in $SYMS" >&2; exit 1; }
echo "symbols   : $SYMS"
echo "breakpoint: 空闲入口 @ $ADDR"

VGA=$(mktemp /tmp/mini-os-vga.XXXXXX.bin)
QEMU_LOG=$(mktemp /tmp/mini-os-qemu.XXXXXX.log)
trap 'kill ${QPID:-} 2>/dev/null || true; rm -f "$VGA"' EXIT

# 端口必须真的空着才能起 QEMU：上一个会话没退干净时，新 QEMU 会在 bind 失败后
# 立刻退出，gdb 连上去只看到一堆空行——这条曾经表现为“make 之后第一次 verify
# 必定报 qemu/gdb 运行失败，第二次就正常”。
#
# **不要用“连一下试试”来探端口**：QEMU 的 gdbstub 一次只服务一个客户端，探针
# 自己就是一次连接；它断开之后 stub 还在收尾，紧接着 gdb 再连就会被拒
# （症状是约一半的运行报 "Remote connection closed"，而我们只看到"没抓到屏幕"）。
# 所以改成**看 listen 状态、不建连接**。
port_listen() {
    if command -v ss >/dev/null 2>&1; then
        ss -ltn 2>/dev/null | awk '{print $4}' | grep -qE "[:.]$PORT$"
    else
        (exec 3<>"/dev/tcp/127.0.0.1/$PORT") 2>/dev/null && { exec 3>&- 3<&-; return 0; }
        return 1
    fi
}
for _ in $(seq 1 20); do
    port_listen || break
    sleep 0.25
done

# 一次尝试：起 QEMU → 等 stub → gdb 抓现场 → 停 QEMU。成功判据是 VGA dump 非空，
# 不是 gdb 的退出码（连不上时 gdb 也会“正常”退出，只是什么也没抓到）。
run_once() {
    : >"$VGA"
    qemu-system-i386 \
        -fda "$IMG" -boot a -m 16 \
        -display none -gdb "tcp::$PORT" -S \
        >"$QEMU_LOG" 2>&1 &
    QPID=$!

    # Wait for the gdb stub port instead of sleeping a fixed amount.
    # 同样只看 listen 状态，不建连接（探针会占掉 stub 的唯一客户端位）。
    for _ in $(seq 1 40); do
        if port_listen; then
            break
        fi
        if ! kill -0 "$QPID" 2>/dev/null; then
            echo "qemu exited early:" >&2
            cat "$QEMU_LOG" >&2
            return 1
        fi
        sleep 0.25
    done

    gdb -batch -q \
        -ex "set architecture i386" \
        -ex "set confirm off" \
        -ex "set pagination off" \
        -ex "set remotetimeout 30" \
        -ex "file $SYMS" \
        -ex "target remote 127.0.0.1:$PORT" \
        -ex "break *$ADDR" \
        -ex "continue" \
        -ex "info registers eip esp ebp cs ss cr0" \
        -ex "dump binary memory $VGA 0xb8000 0xb8fa0" \
        -ex "detach" \
        -ex "quit" 2>&1 | grep -Ev '^\[|^Remote debugging|^0x[0-9a-f]+ in ' || true

    kill "$QPID" 2>/dev/null || true
    wait "$QPID" 2>/dev/null || true
    QPID=
    [ -s "$VGA" ]                       # 抓到屏幕才算这一轮成功
}

ok=0
for attempt in 1 2; do
    if run_once; then
        ok=1
        break
    fi
    echo "第 $attempt 次没抓到屏幕，重试一次（多半是上一个 QEMU 还没退干净）" >&2
    sleep 1
done
if [ "$ok" -ne 1 ]; then
    echo "qemu/gdb 两次都没抓到屏幕，放弃" >&2
    exit 1
fi

echo
if command -v python3 >/dev/null && [ -s "$VGA" ]; then
    python3 "$ROOT/tools/vga-screen.py" "$VGA"
else
    echo "VGA dump: $VGA (python3 missing, raw bytes only)"
fi
