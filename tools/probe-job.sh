#!/usr/bin/env bash
# 一次性现场取证：跳过自检、敲 exec uprog、等它收尾，然后把 job 及其
# 邻居（exec/mosfs 的静态量）整段读出来，看看到底是谁被写了什么。
# 用法： tools/probe-job.sh [命令词...]   默认 exec uprog
set -u
cd "${MINI_OS_DIR:-$HOME/mini-os}" || exit 1
QMP=/tmp/qmp.sock
KEYS=${*:-exec uprog}

killall -q -9 qemu-system-i386 2>/dev/null
sleep 1
rm -f "$QMP"
python3 tools/datadisk.py /tmp/pj.img build/uprog.bin '' build/uprog.elf '' '' '' build/uprog3.bin >/dev/null

(qemu-system-i386 -fda build/mini-os.img -boot a -m 16 -display none \
   -drive if=ide,index=1,format=raw,file=/tmp/pj.img \
   -serial file:/tmp/pj.ser -qmp unix:$QMP,server,nowait >/tmp/pj.qemu.log 2>&1 &)
sleep 3

# shell 提示行说明键盘已活；注键后等它把活干完
python3 tools/qemu-sendkey.py "$QMP" $KEYS
sleep 6

dump() {  # dump <符号> [字数]
    local a
    a=$(nm -n build/kernel.elf | awk -v s="$1" '$3==s{print "0x"$1; exit}')
    if [ -z "$a" ]; then echo "== $1 <no symbol>"; return; fi
    echo "== $1 @ $a"
    python3 tools/qemu-qmp.py "$QMP" "xp /${2:-14}dw $a"
}

dump job 14
dump last_pages 2
dump n_exec 4
dump elf_loaded 2

python3 tools/qemu-qmp.py "$QMP" "pmemsave 0xb9000 4000 /tmp/pj.mem" >/dev/null
python3 tools/vga-screen.py /tmp/pj.mem | sed -n '1,10p'

killall -q -9 qemu-system-i386 2>/dev/null
