#!/bin/bash
# 用户态 #PF 的判据：**一律当越权**，不管地址落在哪。
#
# 为什么单独一条：这条判据以前的写法是 `err & 4` **加** `cr2 < 16MB`。
# 那时所有任务共用一张页目录，只有 16MB 恒等映射被 page_lock_kernel 锁过，
# 16MB 以上只能靠"地址不在按需窗口内"兜底。现在每个任务一张表、
# dir[4..1023] = 0（16MB 以上整个不存在），按地址判就成了漏洞：
# 用户态碰高地址会被放进 page_fault_map，补不上就 kernel_idle 把系统停住。
#
# 探针 boot/uprog6.asm 在 ring3 读 0x01100000，判据：
#   A. exec high 的 rc=E5 且 mag=0xDEAD0000 —— 任务被拦下、没跑完，
#      哨兵原样在（说明"回内核"那条路走通了，不是补页成功继续跑）；
#   B. stat 的 pf_fail=0 —— 这一趟根本没进 page_fault_map 的失败分支
#      （unhandled 分支唯一的记账点就是它，不必再去屏上数行）；
#   C. 之后敲 `irq` 还能留下 KV 记录、且 pf ≥1 —— 系统没被停进 kernel_idle，
#      而 #PF 那条向量真的被走过。
#
# 判据为什么走串口（这一版改的）：A 的 `mag=` 原来靠抓屏正则，屏幕 hex6 只打
# 6 位、字段顺序为列宽服务；B/C 原来靠 `grep -c unhandled` 与"irq 有没有回显"，
# 前者数的是排版、后者靠的是字符是否落在屏上。现在一律读 `KV exec` /
# `KV stat` / `KV irq`，屏幕只留作人看的对照。
#
# ——用 `irq`/`stat` 而不是 `ver`：`ver` 的输出和屏幕顶上的横幅一模一样，
#   停住了也能"读到"它。
set -u
cd "${MINI_OS_DIR:-$HOME/mini-os}" || exit 1
QMP=/tmp/qmp.sock
CON=/tmp/pgh-console.bin
SER=/tmp/pgh-serial.log
IMG=/tmp/pgh.img
sym() { nm -n build/kernel.elf | awk -v s="$1" '$3==s{print $1; exit}'; }
eip_now() { python3 tools/qemu-qmp.py $QMP 'info registers' | sed -n 's/.*EIP=\([0-9a-f]*\).*/\1/p'; }
rows() { python3 tools/vga-screen.py "$CON" | sed -n 's/^row *[0-9]*| //p'; }
dump() { python3 tools/qemu-qmp.py $QMP "pmemsave 0xb9000 4000 $CON" >/dev/null; }
kv() { python3 tools/kv.py "$SER" "$@" 2>/dev/null; }
type_cmd() { python3 tools/qemu-sendkey.py $QMP "$@" ret >/dev/null 2>&1; sleep 1.0; }

[ -f build/uprog6.bin ] || { echo "  FAIL  build/uprog6.bin 不存在（make 没生成）"; exit 1; }
# 盘上只放两个程序：根目录缓存上限是 FS_V1_MAX_FILES（8）个文件，
# 基础三件套（hello/long/blob）已经占掉 3 格，再多塞就读不到了。
# 用 `high=` 具名参数传第 11 个——位置参数里那串空串在中间层会被折叠。
python3 tools/datadisk.py $IMG build/uprog.bin high=build/uprog6.bin \
  >/tmp/datadisk-pgh.log 2>&1 || { echo "  FAIL  造盘失败"; exit 1; }

killall -q -9 qemu-system-i386 2>/dev/null
rm -f $QMP $CON $SER; sleep 1
(qemu-system-i386 -fda build/mini-os.img -boot a -m 16 -display none \
   -drive if=ide,index=1,format=raw,file=$IMG \
   -serial file:$SER \
   -qmp unix:$QMP,server,nowait >/tmp/q-pgh.log 2>&1 &)
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

type_cmd l s; sleep 1; dump
echo "  盘上的文件：$(rows | grep -oE '(uprog|high) +[0-9]+' | tr '\n' ' ')"
# 基线：`pf_fail` 是累计计数，起机自检故意往窗口外写、已经 +1 了。判据只能是
# "这一趟之后没再涨"——屏幕上 unhandled 行数看着是 0，只是证据区被滚掉了。
type_cmd s t a t; sleep 1; dump
pfail0=$(kv stat pf_fail)
echo "  基线 KV stat：$(kv stat)"
type_cmd e x e c spc h i g h; sleep 2; dump
echo "  A. KV exec：$(kv exec)"
echo "     屏幕对照：$(rows | grep -oE 'exec high rc=[^ ]* mag=0x[0-9A-F]+' | tail -1)"

# 还没收到记录就意味着它停在了别处：再等一轮，然后如实判失败
if [ -z "$(kv exec name)" ]; then
  sleep 3; dump
  echo "  A2. 再等三秒：$(kv exec)"
fi

type_cmd i r q; sleep 1; dump
echo "  C. KV irq：$(kv irq)"
type_cmd s t a t; sleep 1; dump
echo "  B. KV stat：$(kv stat)"

name=$(kv exec name)
code=$(kv exec rc)
magic=$(kv exec mag)
irqline=$(kv irq)
pf=$(kv irq pf)
pfail1=$(kv stat pf_fail)

fail=0
[ "$name" = high ] || { echo "  FAIL  A：exec 的 name 应为 high：'${name:-（空）}'"; fail=1; }
[ "$code" = E5 ] || { echo "  FAIL  A：exec high 的 rc 应为 E5：'${code:-（空）}'"; fail=1; }
[ "$magic" = 0xDEAD0000 ] || { echo "  FAIL  A：magic 应为 0xDEAD0000：'${magic:-（空）}'"; fail=1; }
[ -n "${pfail0:-}" ] || { echo "  FAIL  B：基线那次 stat 没取到 pf_fail（命令没执行）"; fail=1; }
[ "$pfail1" = "$pfail0" ] \
  || { echo "  FAIL  B：stat.pf_fail 从 ${pfail0:-?} 涨到 ${pfail1:-?}（越权被放进了 unhandled）"; fail=1; }
[ -n "$irqline" ] || { echo "  FAIL  C：越权之后 irq 没有留下 KV 记录（系统被停住了）"; fail=1; }
[ "${pf:-0}" -ge 1 ] 2>/dev/null || { echo "  FAIL  C：irq.pf 应 ≥1（#PF 真的被走过）：'${pf:-（空）}'"; fail=1; }

if [ "$fail" = 0 ]; then
  echo "PASS  用户态 #PF 一律当越权：高地址不再进补页路径、任务被拦下（哨兵还在）、"
  echo "      系统继续可用"
else
  echo "FAIL  上一行是失败原因"
fi
killall -q -9 qemu-system-i386 2>/dev/null
[ "$fail" = 0 ]
