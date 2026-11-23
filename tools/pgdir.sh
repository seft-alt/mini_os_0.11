#!/bin/bash
# 每任务页目录：每个任务一张私有的页目录，用户页的 U/S 只开在自己的表里。
#
# 判据（`pgd` 的两条 KV 记录就是全部证据；屏幕那两行只作对照）：
#   A. s1..s4 四个槽位都有非 0 的页目录地址（0 = 那一格没建起来）；
#   B. 空闲时 us=0 —— 没有任务在跑，就不该有任何一页对 ring3 开着；
#      tc=0x00000000 —— TSS.cr3 必须等于"CPU 此刻在用的表"，空闲时就是内核表；
#   C. 跑一趟 `exec uprog` 之后**再敲一次** pgd：
#      mp（成功放行次数）变大、sw（切表次数）变大、fl（失败次数）不变、
#      us 回到 0、tc 回 0x00000000 —— "跑的时候放行、跑完收掉"这条闭环真的走了一遍，
#      而且 TSS.cr3 跟着表一起回来了；
#   D. 两趟之间 s1 的地址不变 —— 页表是每个槽位的常驻资源，不是每次新建。
#
# 为什么值得一条单独的回归：页目录错一个位（PDE 少 US、PT 少了 identity）
# 的表现是"程序跑不起来"或"程序跑得起来但能读内核"，两种都难从别的测试里看出来。
#
# 取证从这一版起走**串口 KV**（`kernel/kv.c` + `tools/kv.py`），不再抓显存解析。
# 原因：屏幕是给人看的排版，`hex6` 只打 6 位、字段顺序为列宽服务；把它当机器
# 接口踩过三次坑（`tc=0x0x000000`、`ms=` 被当成 `s=`、grep 字符类里没有 `x`）。
# KV 里十六进制固定 8 位、字段是 `k=v`，脚本只做断言。屏幕上那两行仍然打，
# 只是不再参与判定——`kv.sh` 专门证明"两路并存"。
#
# 取证坑（保留，因为屏幕对照还在看）：屏幕上的旧行不会自己消失。跑完 exec 不重敲
# pgd 就直接看屏幕，读到的是第一屏那句话（mp/sw 一点没变），看起来像"功能没生效"，
# 其实只是没重新取证。KV 这边同理：必须**再敲一次**才会有第二条记录。
set -u
cd "${MINI_OS_DIR:-$HOME/mini-os}" || exit 1
QMP=/tmp/qmp.sock
CON=/tmp/pgd-console.bin
SER=/tmp/pgd-serial.log
IMG=/tmp/pgd.img
sym() { nm -n build/kernel.elf | awk -v s="$1" '$3==s{print $1; exit}'; }
eip_now() { python3 tools/qemu-qmp.py $QMP 'info registers' | sed -n 's/.*EIP=\([0-9a-f]*\).*/\1/p'; }
rows() { python3 tools/vga-screen.py "$CON" | sed -n 's/^row *[0-9]*| //p'; }
dump() { python3 tools/qemu-qmp.py $QMP "pmemsave 0xb9000 4000 $CON" >/dev/null; }
type_cmd() { python3 tools/qemu-sendkey.py $QMP "$@" ret >/dev/null 2>&1; sleep 1.0; }
kv() { python3 tools/kv.py "$SER" "$@"; }

[ -f build/uprog.bin ] || { echo "  FAIL  build/uprog.bin 不存在（make 没生成）"; exit 1; }
python3 tools/datadisk.py $IMG build/uprog.bin >/tmp/datadisk-pgd.log 2>&1 \
  || { echo "  FAIL  造盘失败"; exit 1; }

killall -q -9 qemu-system-i386 2>/dev/null
rm -f $QMP $CON $SER; sleep 1
(qemu-system-i386 -fda build/mini-os.img -boot a -m 16 -display none \
   -drive if=ide,index=1,format=raw,file=$IMG \
   -serial file:$SER -qmp unix:$QMP,server,nowait >/tmp/q-pgd.log 2>&1 &)
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

fail=0
type_cmd p g d; sleep 0.5; dump
kv1=$(kv pgdir)
scr1=$(rows | grep -oE 'pgdc sw=[0-9]+.*tc=0x[0-9A-F]+' | tail -1)
echo "  A. 空闲时的 KV：${kv1:-（串口里没有）}"
echo "     屏幕对照：${scr1:-（没有屏幕行）}"

type_cmd e x e c spc u p r o g; sleep 2
# 跑完之后必须**再敲一次 pgd**：屏幕与 KV 都不会自己刷新
type_cmd p g d; sleep 0.5; dump
kv2=$(kv pgdir --index 2)
scr2=$(rows | grep -oE 'pgdc sw=[0-9]+.*tc=0x[0-9A-F]+' | tail -1)
echo "  C. 跑完 uprog 的 KV：${kv2:-（串口里没有）}"
echo "     屏幕对照：${scr2:-（没有屏幕行）}"

[ -n "$kv1" ] || { echo "  FAIL  第一条 pgdir 记录没有出来"; fail=1; }
[ -n "$kv2" ] || { echo "  FAIL  第二条 pgdir 记录没有出来（跑完没重敲 pgd？）"; fail=1; }
[ -n "$scr1" ] || { echo "  FAIL  屏幕上连对照行都没有（两路布局被改坏了？）"; fail=1; }

for s in s1 s2 s3 s4; do
  v=$(kv pgdir "$s" --index 1 2>/dev/null || true)
  case "$v" in 0x00000000|'') echo "  FAIL  $s 没有页目录：'$v'"; fail=1;; esac
done
[ "$(kv pgdir us --index 1)" = "0" ] || { echo "  FAIL  空闲时 us 应为 0"; fail=1; }
[ "$(kv pgdir tc --index 1)" = "0x00000000" ] \
  || { echo "  FAIL  空闲时 TSS.cr3 应等于内核表"; fail=1; }
[ "$(kv pgdir fe --index 1)" = "0x00000000" ] \
  || { echo "  FAIL  空闲时不该有 ring3 缺页记录"; fail=1; }

m1=$(kv pgdir mp --index 1); m2=$(kv pgdir mp --index 2)
w1=$(kv pgdir sw --index 1); w2=$(kv pgdir sw --index 2)
f1=$(kv pgdir fl --index 1); f2=$(kv pgdir fl --index 2)
[ -n "$m1" ] && [ -n "$m2" ] && [ "$m2" -gt "$m1" ] \
  || { echo "  FAIL  mp 没有变大：$m1 -> $m2"; fail=1; }
[ -n "$w1" ] && [ -n "$w2" ] && [ "$w2" -gt "$w1" ] \
  || { echo "  FAIL  sw 没有变大：$w1 -> $w2"; fail=1; }
[ -n "$f1" ] && [ "$f1" = "$f2" ] \
  || { echo "  FAIL  fl 变了（有放行失败）：$f1 -> $f2"; fail=1; }
[ "$(kv pgdir us --index 2)" = "0" ] || { echo "  FAIL  跑完 us 应回 0"; fail=1; }
[ "$(kv pgdir tc --index 2)" = "0x00000000" ] \
  || { echo "  FAIL  跑完 TSS.cr3 没回内核表"; fail=1; }
s1a=$(kv pgdir s1 --index 1); s1b=$(kv pgdir s1 --index 2)
[ -n "$s1a" ] && [ "$s1a" = "$s1b" ] \
  || { echo "  FAIL  s1 的地址变了：$s1a -> $s1b"; fail=1; }

if [ "$fail" = 0 ]; then
  echo "PASS  每任务页目录：四个槽位各有私有表、空闲 us=0 且 TSS.cr3=内核表、"
  echo "      跑一趟 exec 之后 mp $m1->$m2、sw $w1->$w2、fl 不变、us 与 TSS.cr3 都回来、"
  echo "      表地址不变（$s1a）；判据全部来自串口 KV，屏幕两行只作对照"
else
  echo "FAIL  上一行是失败原因"
fi
killall -q -9 qemu-system-i386 2>/dev/null
[ "$fail" = 0 ]
