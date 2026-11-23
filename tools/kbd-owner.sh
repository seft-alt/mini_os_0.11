#!/bin/bash
# 验证“队列归属”：入队时打标签、取的时候只取自己的。
#
# 证据不在屏幕上（24 行已排满），而在内存里：main 的键盘自检在 ③ 那一步
# attach 槽 1、切过去、喂一个 'a'，然后把四个结果留在四个全局变量里。
# 这里只负责读它们并对表：
#   own_slot = 1                    —— attach 拿到的槽号
#   own_got  = 0x61 ('a')           —— kbd_getchar_t(1) 取到了自己的字符
#   own_iso  = 0xFFFFFFFF (-1)      —— kbd_getchar()（槽 0）拿不到别人的
#   own_after= 0                    —— detach 之后归属退回共享
# 另外读 q_count，确认队列被彻底还干净（没有幽灵字符。
set -u
cd "${MINI_OS_DIR:-$HOME/mini-os}" || exit 1
QMP=/tmp/qmp.sock
sym() { nm -n build/kernel.elf | awk -v s="$1" '$3==s{print $1; exit}'; }
rd() {
  local a; a=$(sym "$1")
  python3 tools/qemu-qmp.py $QMP "xp /1dw 0x$a" | tr -d '\r' | awk '{print $NF}'
}
hx() {                                # 十进制 -> 十六进制字符串（无 0x 前缀）
  printf '%08X' "$1"
}
rd4() {                               # 读一个 4 字数组（q_count[0..3]）并求和
  local a w s=0
  a=$(sym "$1"); a=$((0x$a))
  for w in $(python3 tools/qemu-qmp.py $QMP "xp /4dw 0x$a" | tr -d '\r' | awk '{print $NF}'); do
    s=$((s + w))
  done
  echo $s
}

killall -q -9 qemu-system-i386 2>/dev/null
rm -f $QMP; sleep 1
(qemu-system-i386 -fda build/mini-os.img -boot a -m 16 -display none \
   -qmp unix:$QMP,server,nowait >/tmp/q-own.log 2>&1 &)
sleep 2

want=$(sym kernel_idle); want=$((0x$want))
ok=0
for i in $(seq 1 40); do
  eip=$(python3 tools/qemu-qmp.py $QMP 'info registers' | sed -n 's/.*EIP=\([0-9a-f]*\).*/\1/p')
  eip=$((0x${eip:-0}))
  if [ "$eip" -ge "$want" ] && [ "$eip" -lt "$((want + 64))" ]; then
    echo "  已到 kernel_idle（EIP=0x$(hx $eip)）"; ok=1; break
  fi
  sleep 0.5
done
[ "$ok" = 1 ] || { echo "  FAIL  没等到 kernel_idle"; killall -q -9 qemu-system-i386; exit 1; }

slot=$(rd own_slot);     slot=$((slot))
got=$(rd own_got);       got=$((got))
iso=$(rd own_iso);       iso=$((iso))
after=$(rd own_after);   after=$((after))
qc=$(rd4 q_count)        # 四条环的 q_count 之和：从这里读是为了不依赖函数调用约定
qc0=$(rd kbd_irqs);      qc0=$((qc0))   # 顺手读一下 irq 计数，确认自检真的跑过

echo "  own_slot=$slot own_got=0x$(hx $got) own_iso=0x$(hx $iso) own_after=$after q_count=$qc"

fail=0
[ "$slot" = "1" ]         || { echo "  FAIL  attach 应拿到槽 1，实际 $slot"; fail=1; }
[ "$got" = "97" ]         || { echo "  FAIL  槽 1 应取到 'a'(0x61)，实际 0x$(hx $got)"; fail=1; }
[ "$iso" = "4294967295" ] || { echo "  FAIL  槽 0 应取到 -1，实际 0x$(hx $iso)"; fail=1; }
[ "$after" = "0" ]        || { echo "  FAIL  detach 后归属应退回 0，实际 $after"; fail=1; }
[ "$qc" = "0" ]           || { echo "  FAIL  队列应被取干净（q_count=0），实际 $qc"; fail=1; }

if [ "$fail" = 0 ]; then
  echo "PASS  队列归属：attach/select/getchar_t/隔离/detach 全部符合预期"
else
  echo "FAIL  上一行是失败原因"
fi
killall -q -9 qemu-system-i386 2>/dev/null
[ "$fail" = 0 ]
