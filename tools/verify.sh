#!/usr/bin/env bash
# 回归自检：跑一次 QEMU+gdb，抓 24 行屏幕，和基准做结构化比对 + 区间断言。
#
#   tools/verify.sh            # 比对（默认基准 tests/expect/boot-screen.txt）
#   tools/verify.sh --update   # 有意改了屏幕就把本次输出写成新基准
#   tools/verify.sh -n 3       # 连跑 3 次，全部通过才算过（默认 2 次，抓抖动）
#
# 为什么默认跑两次：dtick / gap / sw 这些量和 TCG 的定时器服务速率有关，
# 单次通过不能排除偶发越界；两次都过才勉强算稳。

set -uo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
ROUNDS=2
UPDATE=0

while [ $# -gt 0 ]; do
    case "$1" in
        --update) UPDATE=1; shift ;;
        -n)       ROUNDS=$2; shift 2 ;;
        *)        echo "unknown arg: $1" >&2; exit 2 ;;
    esac
done

[ -f "$ROOT/build/mini-os.img" ] || { echo "missing build/mini-os.img - run 'make' first" >&2; exit 1; }

TMP=$(mktemp -d /tmp/mini-os-verify.XXXXXX)
trap 'rm -rf "$TMP"' EXIT

fail=0
for i in $(seq 1 "$ROUNDS"); do
    echo "--- run $i/$ROUNDS ---"
    if ! bash "$ROOT/tools/qemu-debug.sh" >"$TMP/run$i.log" 2>&1; then
        echo "FAIL: qemu/gdb 运行失败，日志尾部：" >&2
        tail -20 "$TMP/run$i.log" >&2
        fail=1
        continue
    fi
    grep -E '^row' "$TMP/run$i.log" >"$TMP/run$i.txt" || true

    if [ "$UPDATE" -eq 1 ]; then
        python3 "$ROOT/tools/verify.py" --actual "$TMP/run$i.txt" --update
    else
        python3 "$ROOT/tools/verify.py" --actual "$TMP/run$i.txt" || fail=1
    fi
done

if [ "$fail" -ne 0 ]; then
    echo
    echo "回归失败。若这是‘有意改变行为’，跑 'tools/verify.sh --update' 更新基准，"
    echo "并把基准一起提交——基准文件也是代码。"
    exit 1
fi

echo
echo "全部通过（$ROUNDS 轮）"
