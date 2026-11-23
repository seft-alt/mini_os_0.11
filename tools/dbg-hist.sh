#!/bin/bash
# 逐键调试历史翻页：每按一个键就取一次屏，看清是哪一步不生效
set -u
cd "${MINI_OS_DIR:-$HOME/mini-os}" || exit 1
QMP=/tmp/qmp.sock
VGA=/tmp/vga-hist.bin
row24() { python3 tools/vga-screen.py "$VGA" | sed -n 's/^row 24| //p'; }
sym() { nm -n build/kernel.elf | awk -v s="$1" '$3==s{print $1; exit}'; }
send() { python3 tools/qemu-sendkey.py $QMP "$@"; sleep 0.4; }
dump() { python3 tools/qemu-qmp.py $QMP "pmemsave 0xb8000 4000 $VGA" >/dev/null; }
probe() {                             # probe <标签>
  dump
  local hp hc
  hp=$(python3 tools/qemu-qmp.py $QMP "xp /1dw 0x$(sym hist_pos)" | sed -n 's/.*: //p')
  hc=$(python3 tools/qemu-qmp.py $QMP "xp /1dw 0x$(sym hist_count)" | sed -n 's/.*: //p')
  echo "$1 -> row24='$(row24)' hist_pos=$hp hist_count=$hc"
}

killall -q -9 qemu-system-i386 2>/dev/null
rm -f $QMP $VGA; sleep 1
(qemu-system-i386 -fda build/mini-os.img -boot a -m 16 -display none \
   -qmp unix:$QMP,server,nowait >/tmp/q-hist.log 2>&1 &)
sleep 2
want=$(sym kernel_idle); want=$((0x$want))
for i in $(seq 1 40); do
  eip=$(python3 tools/qemu-qmp.py $QMP 'info registers' | sed -n 's/.*EIP=\([0-9a-f]*\).*/\1/p')
  [ $((0x${eip:-0})) -ge "$want" ] && [ $((0x${eip:-0})) -lt "$((want + 32))" ] && break
  sleep 0.5
done

send a b c ret; probe "abc+ret"
send d e ret;   probe "de+ret"
send up;        probe "up1"
send up;        probe "up2"
send down;      probe "down1"
send down;      probe "down2"
killall -q -9 qemu-system-i386 2>/dev/null
