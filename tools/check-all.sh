#!/bin/bash
# 一键全跑：屏幕证据 + 全部端到端脚本。
#
# 为什么要有这个：
#   1. QEMU 只能一个一个起，脚本之间必须串行，而且每次起机前得先 killall ——
#      这两条以前全靠人手记，忘一次就是"端口被占"或"读到上一轮的状态"；
#   2. 单跑的脚本各自 PASS，不代表连着跑也 PASS（残留进程、/tmp 里的旧 dump、
#      构建产物时间戳都会互相影响）；
#   3. 这里只负责"跑 + 汇总表"，判定标准仍然写在各脚本内部，不在这一层重写；
#   4. `--neg` 追加跑负向组（26 条变异，全在 tools/neg.sh 里）：正向证明“功能在”，负向证明“测试会红”。
set -u
cd "${MINI_OS_DIR:-$HOME/mini-os}" || exit 1

WITH_NEG=0
for a in "$@"; do
  case "$a" in
    --neg) WITH_NEG=1 ;;
    *)     echo "unknown arg: $a（用法：check-all.sh [--neg]）" >&2; exit 2 ;;
  esac
done

VERIFY_ROUNDS=${VERIFY_ROUNDS:-2}
TESTS="verify.sh kbd-shell.sh shell-vars.sh shell-console.sh ring3-io.sh shell-irq.sh shell-tasks.sh serial.sh klog.sh heap.sh disk.sh fs.sh blkdev.sh bcache.sh part.sh vfs.sh fd.sh dev.sh pipe.sh kwait.sh exec.sh exec-argv.sh elf.sh pg.sh pgdir.sh kv.sh pghigh.sh wx.sh fs2.sh fs3.sh fs4.sh userpg.sh ring3pre.sh lease.sh date.sh clock.sh panic.sh"

for t in $TESTS; do
  [ -f "tools/$t" ] || { echo "  SKIP  tools/$t 不存在"; continue; }
done

[ -f build/mini-os.img ] || { echo "FAIL  build/mini-os.img 不存在，先 make"; exit 1; }
# 布局契约：脚本们要按 task_t 的偏移读字段，缺了这个文件就等于仪器没校准
[ -f build/layout.env ] || { echo "FAIL  build/layout.env 不存在，先 make"; exit 1; }

fail=0
pass=0
skip=0
for t in $TESTS; do
  [ -f "tools/$t" ] || continue
  killall -q -9 qemu-system-i386 2>/dev/null
  rm -f /tmp/qmp.sock
  sleep 1                                   # 端口要真的放开（日志里那句 "port busy" 就是它）
  if [ "$t" = verify.sh ]; then
    sleep 2                                # gdb 要连 1234，刚 kill 完端口还在收尾
    out=$(bash tools/$t -n "$VERIFY_ROUNDS" 2>&1); rc=$?
  else
    out=$(bash tools/$t 2>&1); rc=$?
  fi
  last=$(printf '%s\n' "$out" | grep -E 'PASS|FAIL|全部通过' | tail -1)
  if [ "$rc" = 0 ]; then
    pass=$((pass + 1)); printf '  PASS  %-16s %s\n' "$t" "${last:-（无结论行）}"
  else
    fail=$((fail + 1))
    printf '  FAIL  %-16s rc=%s %s\n' "$t" "$rc" "${last:-（无结论行）}"
    printf '%s\n' "$out" | grep -E '  FAIL' | head -5
    # 没有 FAIL 行却退出非零 = 脚本自己的收尾出错（或被打断），把结尾几行也摆出来
    printf '%s\n' "$out" | tail -6 | sed 's/^/        /'
  fi
done
killall -q -9 qemu-system-i386 2>/dev/null

echo "-------------------------------------------------"
echo "合计：PASS $pass / FAIL $fail（共 $((pass + fail)) 个脚本）"

rc=0
[ "$fail" = 0 ] || rc=1

if [ "$WITH_NEG" = 1 ]; then
  echo
  echo "================== 负向组（变异测试）=================="
  bash tools/check-neg.sh
  [ $? = 0 ] || { echo "负向组未通过（见上面的 MISS/FAIL 条目）"; rc=1; }
fi

[ "$rc" = 0 ]
