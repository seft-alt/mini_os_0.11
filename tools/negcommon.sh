#!/bin/bash
# 负向组的公共判据（由 tools/neg_*.sh source）。
#
# 一条负向用例成立的标准只有一句：**把源码改坏之后，对应的正向测试必须失败**。
# “我改了代码、屏幕看起来不对”不算证据——测试若仍然全绿，说明它根本没覆盖到被改坏的
# 那一处，这条负向就是假的。所以这里只认**被测脚本的退出码**，不认人眼比对。
#
# 用法：
#   set -u
#   . "$HOME/mini-os/tools/negcommon.sh"
#   neg_begin
#   neg_case "no-gate" env MINI_OS_DIR=/tmp/negk bash tools/verify.sh -n 1
#   neg_end
#
# 注意：被测脚本自己会打印 PASS/FAIL 行，这里只额外打印它最后 8 行作证据，
# 判据不依赖那几行文字。
neg_begin() {
    NEG_BAD=0
    NEG_N=0
}

neg_case() {
    local tag=$1
    shift
    local out rc

    out=$("$@" 2>&1)
    rc=$?
    NEG_N=$((NEG_N + 1))
    printf '%s\n' "$out" | tail -8 | sed 's/^/    /'
    if [ "$rc" -ne 0 ]; then
        echo "  ok   $tag 被抓（rc=$rc）"
    else
        echo "  MISS $tag 变异没被抓（被测脚本仍 rc=0）"
        NEG_BAD=1
    fi
}

neg_end() {
    if [ "$NEG_BAD" = 0 ]; then
        echo "PASS  负向：$NEG_N 个变异全部被对应正向测试抓住"
        return 0
    fi
    echo "FAIL  负向：有变异没被抓住（见上面的 MISS 行）"
    return 1
}
