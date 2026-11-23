#!/bin/bash
# 按符号名读内存：不用记地址、也不会因为重编译而漂移。
# 用法： tools/sym-dump.sh <符号名> <字数>   例： tools/sym-dump.sh job 12
set -u
cd "${MINI_OS_DIR:-$HOME/mini-os}" || exit 1
SYM="$1"; N="${2:-8}"
ADDR=$(nm -n build/kernel.elf | awk -v s="$SYM" '$3==s{print "0x"$1; exit}')
[ -n "$ADDR" ] || { echo "no symbol $SYM"; exit 1; }
python3 tools/qemu-qmp.py "${QMP:-/tmp/qmp.sock}" "xp /${N}dw $ADDR"
