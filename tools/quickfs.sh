#!/bin/bash
# 拆分 fs/ 之后的快速回归：五个 fs 脚本 + fd 层，逐个跑、只看结论行
set -u
cd "${MINI_OS_DIR:-$HOME/mini-os}" || exit 1
for t in fs.sh fs2.sh fs3.sh fs4.sh vfs.sh fd.sh; do
  out=$(bash "tools/$t" 2>&1 | tail -2 | tr '\n' ' ')
  printf '%-8s %s\n' "$t" "$out"
  case "$out" in
    *FAIL*) echo "   ^^ 失败，详细输出另跑：bash tools/$t" ;;
  esac
done
