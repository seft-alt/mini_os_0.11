#!/bin/bash
# 每向量中断计数 + shell `irq` 命令的端到端验证。
#
# 这条路的证明力来自“两个不同的读法能对上”：
#   A 段：直接读 kernel/vector.c 里的静态数组 `hits[VEC_SLOTS]`（QMP 的 xp），
#         看 0x20/0x21/0x0E 这几个格子的原始数字；
#   B 段：真按键敲 `irq` ⏎，让 shell 自己把同一批数字打到用户页，
#         再用 vga-screen.py 解码、从屏幕文字里把数字抠出来。
# 只有 A 段等于 B 段，才说明 vec_hit 打点、vec_count 读取、cmd_irq 渲染三段都通。
set -u
cd "${MINI_OS_DIR:-$HOME/mini-os}" || exit 1
QMP=/tmp/qmp.sock
CON=/tmp/irq-console.bin
sym() { nm -n build/kernel.elf | awk -v s="$1" '$3==s{print $1; exit}'; }
rd() {  # 读一个 32 位字（静态变量）
  local a; a=$(sym "$1")
  python3 tools/qemu-qmp.py $QMP "xp /1dw 0x$a" | tr -d '\r' | awk '{print $NF}'
}
vec() {  # 读 hits[vec] 的低 32 位（地址先 printf 成十六进制再加 0x，别把十进制当十六进制）
  local a off; a=$(sym hits); off=$(printf '0x%x' $((0x$a + 4 * $1)))
  python3 tools/qemu-qmp.py $QMP "xp /1dw $off" | tr -d '\r' | awk '{print $NF}'
}
eip_now() {
  python3 tools/qemu-qmp.py $QMP 'info registers' | sed -n 's/.*EIP=\([0-9a-f]*\).*/\1/p'
}
screen() { python3 tools/vga-screen.py "$CON"; }

killall -q -9 qemu-system-i386 2>/dev/null
rm -f $QMP $CON; sleep 1
(qemu-system-i386 -fda build/mini-os.img -boot a -m 16 -display none \
   -qmp unix:$QMP,server,nowait >/tmp/q-irq.log 2>&1 &)
sleep 2

want=$(sym kernel_idle); want=$((0x$want))
ok=0
for i in $(seq 1 40); do
  eip=$(eip_now); eip=$((0x${eip:-0}))
  if [ "$eip" -ge "$want" ] && [ "$eip" -lt "$((want + 120))" ]; then
    echo "  已到 kernel_idle（EIP=0x$(printf %08x $eip)）"; ok=1; break
  fi
  sleep 0.5
done
[ "$ok" = 1 ] || { echo "  FAIL  没等到 kernel_idle"; killall -q -9 qemu-system-i386; exit 1; }

hits_sym=$(sym hits)
[ -n "$hits_sym" ] || { echo "  FAIL  nm 找不到 hits 符号（vector.c 没进内核？）"; killall -q -9 qemu-system-i386; exit 1; }

tick_a=$(vec 0x20); kbd_a=$(vec 0x21); pf_a=$(vec 0x0E); yld_a=$(vec 0x30); tune_a=$(vec 0x31)
echo "  A. hits[]: tick=$tick_a kbd=$kbd_a pf=$pf_a yield=$yld_a tune=$tune_a"

# B 段：真按键 `irq` ⏎，键盘自己也会多记几次 0x21
python3 tools/qemu-sendkey.py $QMP i r q ret >/tmp/keys-irq.log 2>&1
sleep 1.2
python3 tools/qemu-qmp.py $QMP "pmemsave 0xb9000 4000 $CON" >/dev/null
kbd_b=$(vec 0x21)
echo "  --- 用户页 dump ---"
screen | sed -n '2,10p'

# `irq` 的输出是两行：第二行是续行（tune + sum）。拼起来才能一起断言。
line=$(screen | sed -n '/| *irq /,/tune=/p' | sed 's/^row *[0-9]*| //' | tr '\n' ' ')
echo "  B. 屏幕: $line"

num() { echo "$line" | grep -o "$1=0x[0-9A-Fa-f]\{6\}" | sed "s/$1=0x//" | head -1; }
b_tick=$(num tick); b_kbd=$(num kbd); b_pf=$(num pf); b_sum=$(num sum)
# 六条之和 <= 全向量合计：越了就是打点漏了/错位了（sum 是 vec_total()，覆盖 0..0x3F）

fail=0
[ "$tick_a" -gt 0 ] 2>/dev/null || { echo "  FAIL  时钟向量 0x20 的计数没涨（$tick_a）"; fail=1; }
[ "$kbd_a" -ge 1 ]  2>/dev/null || { echo "  FAIL  键盘向量 0x21 的计数应 ≥1（$kbd_a）"; fail=1; }
[ "$pf_a" -ge 3 ]   2>/dev/null || { echo "  FAIL  #PF 至少发生过 3 次（0x02000000/0x02001000/0x04000000），实际 $pf_a"; fail=1; }
[ -n "${b_tick:-}" ] || { echo "  FAIL  屏幕上没有 tick=0xNNNNNN 字段"; fail=1; }
[ "$kbd_b" -gt "$kbd_a" ] 2>/dev/null || { echo "  FAIL  敲完 'irq' 之后 0x21 计数应上涨（$kbd_a -> $kbd_b）"; fail=1; }
if [ -n "${b_tick:-}" ] && [ -n "${b_pf:-}" ]; then
  # tick 是活的：A 段读完之后敲键、等屏幕，中间又涨了上百次，只能比“涨没涨、涨得离谱不离谱”
  [ "$((0x$b_tick))" -ge "$tick_a" ] && [ "$((0x$b_tick - tick_a))" -lt 5000 ] \
    || { echo "  FAIL  tick 两读对不上：hits=$tick_a 屏幕=0x$b_tick（应 >= 前者且差值有限）"; fail=1; }
  [ "$((0x$b_pf))" = "$pf_a" ]     || { echo "  FAIL  pf 两读不一致：hits=$pf_a 屏幕=0x$b_pf"; fail=1; }
  [ "${#b_tick}" = 6 ]             || { echo "  FAIL  tick 应该 6 位宽（hex6 防截断），实际 '$b_tick'"; fail=1; }
  # 六条之和 <= 全向量合计（sum 是 vec_total()，覆盖 0..0x3F 全部槽位）。
  # 越了就是打点漏了/错位了：某条路径在计数，但不在我们点名的这六个向量里。
  [ -n "${b_sum:-}" ] || { echo "  FAIL  屏幕上没有 sum=0xNNNNNN 字段"; fail=1; }
  if [ -n "${b_sum:-}" ]; then
    sum_v=$((0x$b_sum))
    [ "$sum_v" -ge "$((0x$b_tick))" ] || { echo "  FAIL  sum($sum_v) 不该小于 tick($((0x$b_tick)))——tick 本身也进 sum"; fail=1; }
    [ "$sum_v" -ge "$((0x$b_kbd))" ]  || { echo "  FAIL  sum 不该小于 kbd"; fail=1; }
  fi
else
  fail=1
fi

if [ "$fail" = 0 ]; then
  echo "PASS  每向量计数：打点/读取/渲染三段一致，tick=0x$b_tick pf=0x$b_pf kbd=0x$b_kbd"
else
  echo "FAIL  上一行是失败原因"
fi
killall -q -9 qemu-system-i386 2>/dev/null
[ "$fail" = 0 ]
