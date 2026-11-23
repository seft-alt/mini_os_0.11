#!/bin/bash
# ring3 端口探测：证明 TSS 的 I/O 位图真的把 ring3 的 in/out 拦住了。
#
# 背景（见 kernel/tss.c）：iomap_base = TSS 结构大小 0x68，等于“这张 TSS 里没有位图”。
# 于是 CPL3 执行 `in al,0x60` 时，CPU 在“查位图”那一步发现偏移超出段界限 → #GP(0)。
#
# 断言：
#   1. 自检仍能跑到 kernel_idle —— 说明 #GP 被收回了内核，而不是把整个内核停掉
#      （以前 13 号向量是“打印完 kernel_halt”，ring3 一碰端口就会死在半路）；
#   2. ring3_io_deny == 1 —— 那一趟探测确实被拦，且判据（CPL3 + 错误码 0）成立；
#   3. 第 0 页证据仍然完整 —— row 14 还是 CPL=3、row 23 还是 unhandled，
#      说明这条路没有把后面的自检打断。
set -u
cd "${MINI_OS_DIR:-$HOME/mini-os}" || exit 1
QMP=/tmp/qmp.sock
P0=/tmp/ring3io-p0.bin

sym() { nm -n build/kernel.elf | awk -v s="$1" '$3==s{print $1; exit}'; }
rd() {
  local a; a=$(sym "$1")
  python3 tools/qemu-qmp.py $QMP "xp /1dw 0x$a" | tr -d '\r' | awk '{print $NF}'
}
eip_now() {
  python3 tools/qemu-qmp.py $QMP 'info registers' | sed -n 's/.*EIP=\([0-9a-f]*\).*/\1/p'
}

killall -q -9 qemu-system-i386 2>/dev/null
rm -f $QMP $P0; sleep 1
(qemu-system-i386 -fda build/mini-os.img -boot a -m 16 -display none \
   -qmp unix:$QMP,server,nowait >/tmp/q-ring3io.log 2>&1 &)
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
[ "$ok" = 1 ] || {
  echo "  FAIL  没等到 kernel_idle：ring3 的 in 很可能把内核停下了（13 号没有回内核的路径）"
  killall -q -9 qemu-system-i386; exit 1
}

deny=$(rd ring3_io_deny)
echo "  ring3_io_deny=$deny（期望 1）"

python3 tools/qemu-qmp.py $QMP "pmemsave 0xb8000 4000 $P0" >/dev/null

fail=0
[ "$deny" = "1" ] || { echo "  FAIL  ring3 的 in 没被 #GP 拦住（iomap_base 摆错了？）"; fail=1; }
python3 tools/vga-screen.py "$P0" | grep -q "cpl=0x3" \
  || { echo "  FAIL  row 14 丢了 cpl=0x3（ring3 那趟被 I/O 探测挤掉了？）"; fail=1; }
python3 tools/vga-screen.py "$P0" | grep -q "unhandled" \
  || { echo "  FAIL  row 23 丢了 unhandled（后面的自检被打断）"; fail=1; }

if [ "$fail" = 0 ]; then
  echo "PASS  ring3 端口探测：I/O 位图拦下 in、#GP 收回内核、自检走完"
else
  echo "FAIL  上一行是失败原因"
fi
killall -q -9 qemu-system-i386 2>/dev/null
[ "$fail" = 0 ]
