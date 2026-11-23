#!/bin/bash
# 第 24 行“可编辑输入行”的分段验证：退格 / 回车 / 写满重来 / 方向键 / Home / End / Delete
#
# 四个约束叠在一起，才有了下面这套写法：
#   1. 不 -S 起 QEMU：内核要真的跑起来、真的在空闲循环里等中断；
#   2. 注入前要**等到 kernel_idle**（不是 sleep 固定秒数）：自检里有 RTC 等待
#      （最长 4s），早注入的键会在自检途中被吃掉，看起来像“丢键”；
#   3. 取屏用 QMP 的 `pmemsave`，**不附着 gdb**——gdb 每次 attach/detach 都要停
#      一次 CPU，按键卡在这个窗口里就会丢；
#   4. `sendkey` 本身也不保证 100% 送达（同一键连打实测会丢 1~2 个/10 个），
#      所以每段都**先按回车把行清空再打**、失败整段重来；最后一段按“回显计数”
#      补发，直到计数够了才下结论。
set -u
cd "${MINI_OS_DIR:-$HOME/mini-os}" || exit 1
QMP=/tmp/qmp.sock
VGA=/tmp/vga-edit.bin
dump() { python3 tools/qemu-qmp.py $QMP "pmemsave 0xb8000 4000 $VGA" >/dev/null; }
row24() { python3 tools/vga-screen.py "$VGA" | sed -n 's/^row 24| //p'; }
sym() { nm -n build/kernel.elf | awk -v s="$1" '$3==s{print $1; exit}'; }
echo_count() {
  local a; a=$(sym echo_count)
  python3 tools/qemu-qmp.py $QMP "xp /1wx 0x$a" | tr -d '\r\n' | sed -n 's/.*: //p'
}
eip_now() {
  python3 tools/qemu-qmp.py $QMP 'info registers' | sed -n 's/.*EIP=\([0-9a-f]*\).*/\1/p'
}
wait_idle() {                            # 等到 EIP 落在 kernel_idle 里
  local want; want=$(sym kernel_idle); want=$((0x$want))
  local i eip
  for i in $(seq 1 40); do
    eip=$(eip_now); eip=$((0x${eip:-0}))
    if [ "$eip" -ge "$want" ] && [ "$eip" -lt "$((want + 32))" ]; then
      echo "  已到 kernel_idle（EIP=0x$(printf %08x $eip)）"; return 0
    fi
    sleep 0.5
  done
  echo "  FAIL  20s 内没等到 kernel_idle（EIP=0x$(printf %08x ${eip:-0})）"; return 1
}
stage() {                                # stage <期望> <键...>
  local want="$1"; shift
  local try got
  for try in 1 2 3 4; do
    python3 tools/qemu-sendkey.py $QMP ret >/dev/null     # 先清行，排除上一段的影响
    python3 tools/qemu-sendkey.py $QMP "$@"
    sleep 0.3; dump; got=$(row24)
    if [ "$want" = "$got" ]; then echo "  PASS  期望'$want' 实际'$got'"; return 0; fi
  done
  echo "  FAIL  期望'$want' 实际'$got'"; return 1
}

killall -q -9 qemu-system-i386 2>/dev/null
rm -f $QMP $VGA; sleep 1
(qemu-system-i386 -fda build/mini-os.img -boot a -m 16 -display none \
   -qmp unix:$QMP,server,nowait >/tmp/q-edit.log 2>&1 &)
sleep 2
wait_idle || exit 1

echo "阶段 1：打 a b c，再按两次退格 -> 应剩 'a'"
stage "a" a b c backspace backspace || exit 1
echo "阶段 2：打 d e，再回车 -> 整行应清空"
stage "" d e ret || exit 1
echo "阶段 3：回车后再打 x y -> 应为 'xy'"
stage "xy" x y || exit 1
echo "阶段 5：打 a b c，按两次 Left，再打 z（覆盖模式：写在第 1 列）-> 应为 'azc'"
stage "azc" a b c left left z || exit 1
echo "阶段 6：打 a b c 后按 Home，再打 q -> 应覆盖行首，'qbc'"
stage "qbc" a b c home q || exit 1
echo "阶段 7：打 a b c 后按 Left Left Left 回行首、再按 End，打 w -> 应接在行尾，'abcw'"
stage "abcw" a b c left left left end w || exit 1
echo "阶段 8：打 a b c d，Left Left，Delete -> 应删掉光标处的 'c'，'abd'"
stage "abd" a b c d left left delete || exit 1

echo "阶段 9：连打 80 个 'f' -> 撞满 79 列后整行清空重来（应剩 'fff'）"
# 行满的条件是“这一段累计 80 个字符”：先有的 x y 占 2 格，再有 77 个 f 填到
# 第 79 列，剩下的 3 个 f 触发清行重来。按键会丢，所以按回显计数补发。
base=$(echo_count); base=$((base))
target=$((base + 80))
for try in $(seq 1 20); do
  cur=$(echo_count); cur=$((cur))
  left=$((target - cur))
  [ "$left" -le 0 ] && break
  [ "$left" -gt 10 ] && left=10
  keys=""; for i in $(seq 1 $left); do keys="$keys f"; done
  python3 tools/qemu-sendkey.py $QMP $keys
  sleep 0.3
done
dump
got=$(row24); cur=$(echo_count); cur=$((cur))
echo "  回显计数 $base -> $cur（目标 $target）/ row24='$got' 期望='fff'"
if [ "$cur" -eq "$target" ] && [ "$got" = "fff" ]; then
  echo "  PASS  写满 79 列后清行重来"
else
  echo "  FAIL  计数或内容不符"; exit 1
fi

echo "PASS  第 24 行可编辑：退格 / 回车 / 写满重来 全部符合预期"

echo "阶段 10：打 a b c 回车、d e 回车，再按两次 Up -> 应翻到更早那条 'abc'"
stage "abc" a b c ret d e ret up up || exit 1
echo "阶段 11：打 a b c 回车、d e 回车，Up 之后按 Down -> 从最新那条退回更早的 'abc'"
stage "abc" a b c ret d e ret up down || exit 1
echo "阶段 12：Up Up 之后连按四次 Down -> 翻过最旧那条就回到空行"
# 连按四次是因为历史里已经攒着前几段的行（最多 4 条），Down 到“越过最旧”要多按几下；
# 越过之后再多按也是空行，所以这条断言稳定。
stage "" a b c ret d e ret up up down down down down || exit 1

echo "PASS  输入历史：回车入栈、Up/Down 翻页、翻到底回空行 全部符合预期"
killall -q -9 qemu-system-i386 2>/dev/null
