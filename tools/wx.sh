#!/bin/bash
# 用户页写保护（W^X 的第一步）：代码页只读、栈可写。
#
# 之前 ring3 能进的那几页一律 P|RW|US —— "能读"和"能写"是一起给的。于是
# 用户程序可以改自己的代码页：一个 bug（或一次恶意取指改写）就能把
# "可执行"变成"可篡改"，而内核看不出任何异常。
#
# 这一版把权限拆开：
#   - fs/elf.c 的 elf_load_map 按 ELF 的 PF_W 位算出一张"哪几页可写"的图；
#   - exec 放行时按图走 pgdir_set_user_page(..., writable)：纯代码段（ld 的
#     FLAGS(5)，没有 PF_W）整页只给 US、清掉 RW；
#   - 裸二进制仍然整段可写（老路径一个字节都不变，见 A 这一条）。
#
# 探针 boot/uprog7.asm（tools/uprog7.ld 拼成只读代码段）：第一条指令就写
# 自己所在的代码页。判据：
#   A. `exec uprog`（裸二进制，可写）照常 rc=0 mag=0x45584543 val=36 —— 写保护
#      没有误伤"程序在栈/数据上写"这条正常路径；
#   B. `exec ro`（只读代码页）rc=E5 mag=0xDEAD0000 —— 哨兵还在，说明它没跑完；
#   C. 同一条 exec 记录里的 fe=0x00000007 —— 错误码是 P|W|U（"页在、想写、
#      用户态"），这是"写保护拦住"与"页不存在/普通缺页"唯一分得开的地方：
#      只判 B 的话，"程序跑完 return、只是没写指纹"也长得一模一样。
#      `pgd` 的 fe 同口径再取一次，两处相等才算数；
#   D. `irq` 的 pf ≥ 1（这条向量真的被走过）且 `irq` 记录存在（命令真的执行了）
#      —— 既没进补页失败分支，也没把系统停进 kernel_idle；
#   D2. `stat` 的 pf_fail = 0（unhandled 分支一次都没进）。
#
# 判据为什么走串口（这一版改的）：上面 A/B/C 三个字段原来靠"抓屏 + 正则"，
# 屏幕的 hex6 只打 6 位（fe=0x000007），且字段顺序是为列宽服务的——`grep -oE`
# 的字符类里没有 `x`，`fe=0x000007` 会被截成 `fe=0x0`。现在字段一律从
# `KV exec` / `KV pgdir` / `KV irq` / `KV stat` 取，屏幕只留作人看的对照。
#
# 为什么用 `irq`/`stat` 而不是 `ver`：`ver` 的输出和屏幕顶上的横幅一模一样，
# 系统停住了也能"读到"它。
set -u
cd "${MINI_OS_DIR:-$HOME/mini-os}" || exit 1
QMP=/tmp/qmp.sock
CON=/tmp/wx-console.bin
SER=/tmp/wx-serial.log
IMG=/tmp/wx.img
sym() { nm -n build/kernel.elf | awk -v s="$1" '$3==s{print $1; exit}'; }
eip_now() { python3 tools/qemu-qmp.py $QMP 'info registers' | sed -n 's/.*EIP=\([0-9a-f]*\).*/\1/p'; }
rows() { python3 tools/vga-screen.py "$CON" | sed -n 's/^row *[0-9]*| //p'; }
dump() { python3 tools/qemu-qmp.py $QMP "pmemsave 0xb9000 4000 $CON" >/dev/null; }
kv() { python3 tools/kv.py "$SER" "$@" 2>/dev/null; }
type_cmd() { python3 tools/qemu-sendkey.py $QMP "$@" ret >/dev/null 2>&1; sleep 1.0; }

[ -f build/uprog7.elf ] || { echo "  FAIL  build/uprog7.elf 不存在（make 没生成）"; exit 1; }
[ -f build/uprog.bin ] || { echo "  FAIL  build/uprog.bin 不存在（make 没生成）"; exit 1; }
# 5 个文件：hello/long/blob + uprog（裸二进制，正向对照）+ ro（只读代码段）。
# 根目录缓存上限是 FS_V1_MAX_FILES（8），再多塞就读不到了。
# `wx=` 具名传：第 11 个位置参数前面一串空串在中间层会被折叠。
python3 tools/datadisk.py $IMG build/uprog.bin wx=build/uprog7.elf \
  >/tmp/datadisk-wx.log 2>&1 || { echo "  FAIL  造盘失败"; exit 1; }
tail -1 /tmp/datadisk-wx.log | sed 's/^/  /'

killall -q -9 qemu-system-i386 2>/dev/null
rm -f $QMP $CON $SER; sleep 1
(qemu-system-i386 -fda build/mini-os.img -boot a -m 16 -display none \
   -drive if=ide,index=1,format=raw,file=$IMG \
   -serial file:$SER \
   -qmp unix:$QMP,server,nowait >/tmp/q-wx.log 2>&1 &)
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
echo "  盘上的文件：$(rows | grep -oE '(uprog|ro) +[0-9]+' | tr '\n' ' ')"
nkv=$(grep -c '^KV ' $SER 2>/dev/null || true)
echo "  起机到 idle：KV 行 ${nkv:-0}（应为 0：KV 只由命令触发）"

# 基线：`pf_fail` 是**累计**计数，起机自检那一步故意往窗口外写、已经 +1 了。
# 所以判据只能是"这一趟之后没再涨"，不是"等于 0"（屏幕上的 unhandled 行数
# 之所以看着是 0，只是因为证据区被滚动刷掉了——那正是"数排版"的老问题）。
type_cmd s t a t; sleep 1; dump
pf0=$(kv stat pf)
pfail0=$(kv stat pf_fail)
echo "  基线 KV stat：pf=${pf0:-?} pf_fail=${pfail0:-?}"

# A：正向对照。裸二进制整段可写，程序在栈上写指纹/字符串 → 必须照常跑完。
type_cmd e x e c spc u p r o g; sleep 2; dump
echo "  A. KV exec（第 1 条）：$(kv exec --index 1)"
echo "     屏幕对照：$(rows | grep -oE 'exec uprog rc=[^ ]* mag=0x[0-9A-F]+ val=[0-9]+' | head -1)"

# B：只读代码页。第一条指令写自己的代码页 → 期望 #PF → 越权 → 任务死。
type_cmd c l e a r; type_cmd e x e c spc r o; sleep 2; dump
echo "  B. KV exec（第 2 条）：$(kv exec --index 2)"
echo "     屏幕对照：$(rows | grep -oE 'exec ro rc=[^ ]* mag=0x[0-9A-F]+' | tail -1)"

# C：错误码的证据。exec 记录落盘那一刻读到的就是这趟的现场。
type_cmd p g d; sleep 1; dump
echo "  C. exec 记录里的 fe：$(kv exec fe --index 2)"
echo "     pgd 记录里的 fe：$(kv pgdir fe)"

# D：系统还活着 + 没进补页失败分支。
type_cmd i r q; sleep 1; dump
echo "  D. KV irq：$(kv irq)"
type_cmd s t a t; sleep 1; dump
echo "  D2. KV stat（越权之后）：$(kv stat)"

good_rc=$(kv exec rc --index 1)
good_mag=$(kv exec mag --index 1)
good_val=$(kv exec val --index 1)
legal_name=$(kv exec name --index 2)
legal_rc=$(kv exec rc --index 2)
legal_mag=$(kv exec mag --index 2)
fe=$(kv exec fe --index 2)
fep=$(kv pgdir fe)
pf=$(kv irq pf)
irqline=$(kv irq)
pf1=$(kv stat pf)
pfail1=$(kv stat pf_fail)

fail=0
[ "$good_rc" = 0 ] || { echo "  FAIL  A：exec uprog 的 rc 应为 0：'$good_rc'"; fail=1; }
[ "$good_mag" = 0x45584543 ] || { echo "  FAIL  A：magic 应为 0x45584543：'$good_mag'"; fail=1; }
[ "$good_val" = 36 ] || { echo "  FAIL  A：val 应为 36：'$good_val'"; fail=1; }
[ "$legal_name" = ro ] || { echo "  FAIL  B：第 2 条 exec 应是 ro：'$legal_name'"; fail=1; }
[ "$legal_rc" = E5 ] || { echo "  FAIL  B：exec ro 的 rc 应为 E5：'$legal_rc'"; fail=1; }
[ "$legal_mag" = 0xDEAD0000 ] || { echo "  FAIL  B：magic 应为 0xDEAD0000：'$legal_mag'"; fail=1; }
[ "$fe" = 0x00000007 ] || { echo "  FAIL  C：写只读页的错误码应是 0x7（P|W|U）：'${fe:-（空）}'"; fail=1; }
[ "$fep" = 0x00000007 ] || { echo "  FAIL  C：pgd 的 fe 应与 exec 同口径 0x00000007：'${fep:-（空）}'"; fail=1; }
[ -n "$irqline" ] || { echo "  FAIL  D：越权之后 irq 没有留下 KV 记录（系统被停住了）"; fail=1; }
[ "${pf:-0}" -ge 1 ] 2>/dev/null || { echo "  FAIL  D：irq.pf 应 ≥1（#PF 真的被走过）：'${pf:-（空）}'"; fail=1; }
[ -n "${pfail0:-}" ] || { echo "  FAIL  D2：基线那次 stat 没取到 pf_fail（命令没执行）"; fail=1; }
[ "$pfail1" = "$pfail0" ] \
  || { echo "  FAIL  D2：stat.pf_fail 从 ${pfail0:-?} 涨到 ${pfail1:-?}（越权被放进了 unhandled）"; fail=1; }

if [ "$fail" = 0 ]; then
  echo "PASS  用户页写保护：只读代码页在 ring3 里写不进去（fe=0x7 = 页在/想写/用户态），"
  echo "      任务被干净拦下、哨兵还在、系统继续可用；可写页（裸二进制）没有回归"
else
  echo "FAIL  上一行是失败原因"
fi
killall -q -9 qemu-system-i386 2>/dev/null
[ "$fail" = 0 ]
