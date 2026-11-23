#!/bin/bash
# 真按键验证：QEMU + QMP 注入按键 + pmemsave 取回第 24 行
#
# 和 screen.sh 的区别：这里**不用 -S**（内核要真的跑起来、真的在空闲循环里
# 等中断），注入前先等到 kernel_idle（自检里有最长 4s 的 RTC 等待，早注入的键
# 会被自检吃掉），取屏用 QMP 的 pmemsave，不附着 gdb（不动 CPU、不抢键）。
#
# 断言：第 24 行（唯一空着的最后一行）出现我们注入的那串字符。
set -u
cd "${MINI_OS_DIR:-$HOME/mini-os}" || exit 1
KEYS="${*:-h e l l o}"
TEXT=$(echo "$KEYS" | tr -d ' ')
QMP=/tmp/qmp.sock
VGA=/tmp/vga-kbd.bin
sym() { nm -n build/kernel.elf | awk -v s="$1" '$3==s{print $1; exit}'; }

killall -q -9 qemu-system-i386 2>/dev/null
rm -f $QMP $VGA; sleep 1
(qemu-system-i386 -fda build/mini-os.img -boot a -m 16 -display none \
   -qmp unix:$QMP,server,nowait >/tmp/q-kbd.log 2>&1 &)
sleep 2

want=$(sym kernel_idle); want=$((0x$want))
for i in $(seq 1 40); do
  eip=$(python3 tools/qemu-qmp.py $QMP 'info registers' | sed -n 's/.*EIP=\([0-9a-f]*\).*/\1/p')
  eip=$((0x${eip:-0}))
  [ "$eip" -ge "$want" ] && [ "$eip" -lt "$((want + 32))" ] && break
  sleep 0.5
done

python3 tools/qemu-sendkey.py $QMP $KEYS || { echo "SENDKEY-FAIL"; exit 1; }
sleep 0.4
python3 tools/qemu-qmp.py $QMP "pmemsave 0xb8000 4000 $VGA" >/dev/null

echo "---- 期望 row 24 出现: $TEXT ----"
python3 tools/vga-screen.py "$VGA" | grep -q "^row 24| $TEXT" \
  && { echo "PASS  真按键回显 = $TEXT"; rc=0; } \
  || { echo "FAIL  row 24 没有 '$TEXT'"; python3 tools/vga-screen.py "$VGA" | grep '^row 24' || echo "(row 24 空)"; rc=1; }
killall -q -9 qemu-system-i386 2>/dev/null
exit "$rc"
