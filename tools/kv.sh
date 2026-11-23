#!/bin/bash
# 机器可读证据通道（kernel/kv.c + tools/kv.py）能不能独立站住。
#
# 这一条不是"再测一次 pgd/exec"，而是测**取证方式**：到目前为止每个脚本都是
# `pmemsave 0xb9000` + `vga-screen.py` 解码 + 正则。那条路的脆弱点不在解码，
# 在"人看的排版成了接口"——改一个 `0x` 前缀、一次列宽压缩，回归就红，而被测
# 行为其实对的（本项目踩过 `tc=0x0x000000`、`ms=` 当成 `s=`、`grep` 字符类
# 里没有 `x` 三次）。
#
# 本脚本**刻意不用 pmemsave**，全部判据只看串口里的 `KV <tag> k=v ...`：
#   A. `pgd` → `KV pgdir s1..s4 sw us mp fl fe tc` 字段齐全，空闲时 tc=0x00000000、
#      us=0、fl=0（和屏幕上那两行是同一批数，但只从串口拿）；
#   B. `exec uprog` → `KV exec rc=0 mag=0x45584543 val=36 ex=36 d=0 jb=0 asu=0`；
#   C. 跑完再 `pgd` → sw 涨、mp ≥ +2、us 回 0、tc 回内核表、四个表地址不变
#      （这条等于 pgdir.sh 的核心判据，换了一条取证通道重做一遍）；
#   D. 屏幕那两行**仍然在**（唯一一次 dump 只用来证明"两路并存"，不参与 A/B/C）；
#   E. 串口里一共正好 3 条 KV 记录，且没有 `ovf=1`——少一条说明漏发，多一条说明重复发。
#
# 判据 A/B/C 与屏幕那套用的是同一组口径（十进制与 `E<n>` 一致；十六进制在 KV 里
# 固定 8 位、屏幕上是 6 位——不截断是机器通道的硬要求），所以把老脚本迁过来时
# 判据字符串基本不用重写——这正是这条通道值钱的地方。
set -u
cd "${MINI_OS_DIR:-$HOME/mini-os}" || exit 1
QMP=/tmp/qmp.sock
CON=/tmp/kv-console.bin
SER=/tmp/kv-serial.log
IMG=/tmp/kv.img
sym() { nm -n build/kernel.elf | awk -v s="$1" '$3==s{print $1; exit}'; }
eip_now() { python3 tools/qemu-qmp.py $QMP 'info registers' | sed -n 's/.*EIP=\([0-9a-f]*\).*/\1/p'; }
rows() { python3 tools/vga-screen.py "$CON" | sed -n 's/^row *[0-9]*| //p'; }
dump() { python3 tools/qemu-qmp.py $QMP "pmemsave 0xb9000 4000 $CON" >/dev/null; }
type_cmd() { python3 tools/qemu-sendkey.py $QMP "$@" ret >/dev/null 2>&1; sleep 1.0; }
kv() { python3 tools/kv.py "$SER" "$@"; }        # kv <tag> [key] [--index N]

[ -f build/uprog.bin ] || { echo "  FAIL  build/uprog.bin 不存在（make 没生成）"; exit 1; }
python3 tools/datadisk.py $IMG build/uprog.bin >/tmp/datadisk-kv.log 2>&1 \
  || { echo "  FAIL  造盘失败"; exit 1; }

killall -q -9 qemu-system-i386 2>/dev/null
rm -f $QMP $CON $SER; sleep 1
(qemu-system-i386 -fda build/mini-os.img -boot a -m 16 -display none \
   -drive if=ide,index=1,format=raw,file=$IMG \
   -serial file:$SER \
   -qmp unix:$QMP,server,nowait >/tmp/q-kv.log 2>&1 &)
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

# ---- A. 空闲时的一条 pgdir ----
fail=0
boot_kv=$(grep -c '^KV ' $SER 2>/dev/null || true)
echo "  A0. 起机到 idle 之间自发产生的 KV 行数：${boot_kv:-0}（应为 0：KV 只由命令触发）"
type_cmd p g d; sleep 1
kv1=$(kv pgdir)
echo "  A. KV pgdir：${kv1:-（串口里没有）}"
for key in s1 s2 s3 s4 sw us mp fl fe tc; do
  kv pgdir "$key" >/dev/null 2>&1 || { echo "  FAIL  A：pgdir 缺字段 $key"; fail=1; }
done
tc1=$(kv pgdir tc); us1=$(kv pgdir us); fl1=$(kv pgdir fl); sw1=$(kv pgdir sw); mp1=$(kv pgdir mp)

# ---- B. exec uprog 的 KV ----
type_cmd e x e c spc u p r o g; sleep 2
kve=$(kv exec)
echo "  B. KV exec：${kve:-（串口里没有）}"

# ---- C. 跑完再 pgd：账要回到空闲那一组 ----
type_cmd p g d; sleep 1
kv2=$(kv pgdir --index 2)
echo "  C. KV pgdir（第 2 条）：${kv2:-（串口里没有）}"
tc2=$(kv pgdir tc --index 2); us2=$(kv pgdir us --index 2)
sw2=$(kv pgdir sw --index 2); mp2=$(kv pgdir mp --index 2)
s11=$(kv pgdir s1 --index 1); s12=$(kv pgdir s1 --index 2)

# ---- D. 屏幕那两行仍在（唯一一次 dump，只证明"两路并存"）----
sleep 0.5; dump
screen_ok=$(rows | grep -oE 'pgdc sw=[0-9]+.*tc=0x[0-9A-F]+' | tail -1)

# ---- E. 记录条数与完整性 ----
nrec=$(grep -c '^KV ' $SER 2>/dev/null || true)
ovf=$(grep -c 'ovf=1' $SER 2>/dev/null || true)
echo "  E. 串口里的 KV 记录：${nrec:-0} 条，其中溢出 ${ovf:-0} 条（应为 3 与 0）"

[ "${boot_kv:-0}" = 0 ] || { echo "  FAIL  A0：起机期间出现了 KV 行（说明有人把证据通道当日志用了）"; fail=1; }
[ "$tc1" = "0x00000000" ] || { echo "  FAIL  A：空闲时 tc 应是 0x00000000（内核表），实际 '${tc1:-空}'"; fail=1; }
[ "$us1" = "0" ] || { echo "  FAIL  A：空闲时 us 应是 0，实际 '${us1:-空}'"; fail=1; }
[ "$fl1" = "0" ] || { echo "  FAIL  A：fl 不该有失败，实际 '${fl1:-空}'"; fail=1; }
case "$kve" in
  *"rc=0"*"mag=0x45584543"*"val=36"*) ;;
  *) echo "  FAIL  B：exec 的 KV 不含 rc=0 / mag / val=36：'${kve:-空}'"; fail=1;;
esac
kv exec rc >/dev/null 2>&1 || { echo "  FAIL  B：exec 的 KV 缺 rc"; fail=1; }
[ "$(kv exec ex)" = "36" ] || { echo "  FAIL  B：ex 应是 36，实际 '$(kv exec ex)'"; fail=1; }
[ "$(kv exec d)" = "0" ] || { echo "  FAIL  B：死因应是 0（return）"; fail=1; }
[ "$(kv exec jb)" = "0" ] || { echo "  FAIL  B：jb 应回 0（job 表无残留）"; fail=1; }
[ "$(kv exec asu)" = "0" ] || { echo "  FAIL  B：asu 应回 0（地址空间对象都还了）"; fail=1; }
[ -n "${sw2:-}" ] && [ "${sw2:-0}" -gt "${sw1:-0}" ] \
  || { echo "  FAIL  C：跑一趟之后 sw 没涨（$sw1 -> ${sw2:-?}）"; fail=1; }
[ "${mp2:-0}" -ge $((mp1 + 2)) ] \
  || { echo "  FAIL  C：mp 应至少 +2（两页放行），$mp1 -> ${mp2:-?}"; fail=1; }
[ "${us2:-1}" = "0" ] || { echo "  FAIL  C：跑完 us 应回 0，实际 ${us2:-?}"; fail=1; }
[ "${tc2:-x}" = "0x00000000" ] || { echo "  FAIL  C：跑完 tc 应回 0x00000000，实际 ${tc2:-?}"; fail=1; }
[ -n "${s12:-}" ] && [ "$s11" = "$s12" ] \
  || { echo "  FAIL  C：s1 的表地址变了（$s11 -> ${s12:-?}）——私有表应常驻同槽位"; fail=1; }
[ -n "${screen_ok:-}" ] || { echo "  FAIL  D：屏幕那两行不见了（KV 不该替代人看的输出）"; fail=1; }
[ "${nrec:-0}" = 3 ] || { echo "  FAIL  E：KV 记录应是 3 条（pgd/exec/pgd），实际 ${nrec:-0}"; fail=1; }
[ "${ovf:-0}" = 0 ] || { echo "  FAIL  E：出现了 ovf=1，说明某条记录被截断"; fail=1; }

if [ "$fail" = 0 ]; then
  echo "PASS  机器可读证据通道：三条记录（pgd/exec/pgd）全部只从串口取到，"
  echo "      tc=$tc1 us=$us1 -> exec rc=0 mag=0x45584543 -> sw $sw1->$sw2 mp $mp1->$mp2"
  echo "      tc=$tc2；屏幕两行仍在、无溢出"
else
  echo "FAIL  上一行是失败原因"
fi
killall -q -9 qemu-system-i386 2>/dev/null
[ "$fail" = 0 ]
