#!/bin/bash
# 负向组：26 条变异，全部由 tools/neg.sh 执行；本脚本只负责"逐条报一行 + 组级结论"。
#
# 为什么单独一个入口：正向脚本证明"功能在"，负向脚本证明"测试真的会红"。
# 后者每条都要 复制整棵源码树 + 打补丁 + 重编 + 起 QEMU，单条 30~90s，
# 所以不塞进默认的 check-all，而是 `check-all.sh --neg` 或本脚本单独跑。
#
# 判据只有一条：**每条 case 自己返回 0**。变异与判据都在 tools/neg.sh 的
# case_xxx 函数里，这里不再复述任何期望值，也不重复实现"建树/打补丁/编译"。
set -u
cd "${MINI_OS_DIR:-$HOME/mini-os}" || exit 1

NEG_CASES="bcache_hit bcache_wt console dev_hd kbd_depth kbd_echo kbd_lane
kbd_line kbd_owner kbd_shell kbd_subq kbd_task kbd kwait lease_owner lease_table
lease lock nomark notify_order part_sig pipe_cnt range shell_hist shell_vars
vfs_name vfs_rmdir vfs_wpath"

[ -f build/mini-os.img ] || { echo "FAIL  build/mini-os.img 不存在，先 make"; exit 1; }

pass=0
fail=0
for c in $NEG_CASES; do
    out=$(bash tools/neg.sh -1 "$c" 2>&1)
    rc=$?
    last=$(printf '%s\n' "$out" | grep -E '^PASS|^FAIL|^  MISS' | tail -1)
    if [ "$rc" = 0 ]; then
        pass=$((pass + 1))
        printf '  PASS  %-16s %s\n' "$c" "${last:-（无结论行）}"
    else
        fail=$((fail + 1))
        printf '  FAIL  %-16s rc=%s %s\n' "$c" "$rc" "${last:-（无结论行）}"
        printf '%s\n' "$out" | grep -E '^  (MISS|FAIL)|^FAIL|NOT-FOUND|AMBIGUOUS' | head -4
        printf '%s\n' "$out" | tail -4 | sed 's/^/        /'
    fi
done
killall -q -9 qemu-system-i386 2>/dev/null

echo "-------------------------------------------------"
echo "负向合计：PASS $pass / FAIL $fail（共 $((pass + fail)) 条）"
[ "$fail" = 0 ]
