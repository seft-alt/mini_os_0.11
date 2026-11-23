#!/bin/bash
# 验证“深度可配”：环深与提交深都是运行时可改的数字，不是编译期写死的常量。
#
# 证据在内存里（main 的键盘自检 ⑥ 留下的七个非 static 全局）：
#   dep_lane_depth  = 6   —— kbd_lane_set_depth(1,6) 的返回值（真的生效）
#   dep_lane_peak   = 6   —— 同一条环连喂 8 个字符后到过 6（按新深度装）
#   dep_lane_drop   = 2   —— 8 进 6，多出来的 2 个被丢（差值，不掺前面的账）
#   dep_lane_restore= 4   —— 改回 4 也生效（不是只能放大）
#   dep_sub_cap     = 6   —— kbd_line_set_capacity(6) 的返回值
#   dep_sub_full    = 0   —— 深度 6 时压第 7 条才被拒（按新容量判满）
#   dep_sub_restore = 4   —— 改回默认 4
# 外加现在两条队列都应是空的（自检自己清干净）。
set -u
cd "${MINI_OS_DIR:-$HOME/mini-os}" || exit 1
QMP=/tmp/qmp.sock
sym() { nm -n build/kernel.elf | awk -v s="$1" '$3==s{print $1; exit}'; }
rd() {
  local a; a=$(sym "$1")
  python3 tools/qemu-qmp.py $QMP "xp /1dw 0x$a" | tr -d '\r' | awk '{print $NF}'
}
sumq() {                              # 四条环的 q_count 之和
  local a w s=0
  a=$(sym q_count); a=$((0x$a))
  for w in $(python3 tools/qemu-qmp.py $QMP "xp /4dw 0x$a" | tr -d '\r' | awk '{print $NF}'); do
    s=$((s + w))
  done
  echo $s
}

killall -q -9 qemu-system-i386 2>/dev/null
rm -f $QMP; sleep 1
(qemu-system-i386 -fda build/mini-os.img -boot a -m 16 -display none \
   -qmp unix:$QMP,server,nowait >/tmp/q-depth.log 2>&1 &)
sleep 2

want=$(sym kernel_idle); want=$((0x$want))
ok=0
for i in $(seq 1 40); do
  eip=$(python3 tools/qemu-qmp.py $QMP 'info registers' | sed -n 's/.*EIP=\([0-9a-f]*\).*/\1/p')
  eip=$((0x${eip:-0}))
  if [ "$eip" -ge "$want" ] && [ "$eip" -lt "$((want + 64))" ]; then
    echo "  已到 kernel_idle（EIP=0x$(printf %08x $eip)）"; ok=1; break
  fi
  sleep 0.5
done
[ "$ok" = 1 ] || { echo "  FAIL  没等到 kernel_idle"; killall -q -9 qemu-system-i386; exit 1; }

ld=$(rd dep_lane_depth);  ld=$((ld))
lp=$(rd dep_lane_peak);   lp=$((lp))
ldr=$(rd dep_lane_drop);  ldr=$((ldr))
lrs=$(rd dep_lane_restore); lrs=$((lrs))
sc=$(rd dep_sub_cap);     sc=$((sc))
sf=$(rd dep_sub_full);    sf=$((sf))
srs=$(rd dep_sub_restore); srs=$((srs))
lq=$(sumq)
sp=$(rd submit_pending);  sp=$((sp))

echo "  lane: depth=$ld peak=$lp drop=$ldr restore=$lrs"
echo "  subq: cap=$sc full=$sf restore=$srs | 现在 lane_total=$lq submit_pending=$sp"

fail=0
[ "$ld" = "6" ]  || { echo "  FAIL  环深设 6 应生效，实际 $ld"; fail=1; }
[ "$lp" = "6" ]  || { echo "  FAIL  8 个字符应把环塞到 6（新深度），实际 peak=$lp"; fail=1; }
[ "$ldr" = "2" ] || { echo "  FAIL  8 进 6 应丢 2 个，实际 $ldr"; fail=1; }
[ "$lrs" = "4" ] || { echo "  FAIL  改回 4 应生效，实际 $lrs"; fail=1; }
[ "$sc" = "6" ]  || { echo "  FAIL  提交深设 6 应生效，实际 $sc"; fail=1; }
[ "$sf" = "0" ]  || { echo "  FAIL  深度 6 时第 7 条应被拒，实际 $sf"; fail=1; }
[ "$srs" = "4" ] || { echo "  FAIL  提交深改回 4 应生效，实际 $srs"; fail=1; }
[ "$lq" = "0" ]  || { echo "  FAIL  自检应把四条环清空，实际 total=$lq"; fail=1; }
[ "$sp" = "0" ]  || { echo "  FAIL  自检应把提交队列清空，实际 pending=$sp"; fail=1; }

if [ "$fail" = 0 ]; then
  echo "PASS  深度可配：环深 6 真装 6 丢 2、提交深 6 第 7 条才拒，改回 4 都生效"
else
  echo "FAIL  上一行是失败原因"
fi
killall -q -9 qemu-system-i386 2>/dev/null
[ "$fail" = 0 ]
