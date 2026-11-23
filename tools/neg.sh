#!/bin/bash
# 负向组：28 组变异，一个驱动 + 一张 case 表。
#
# 为什么合成一个文件：原来是 tools/neg_*.sh 26 个脚本，每个 20~26 行，
# 其中 20 行是同一套样板（cp 整棵树 → negpatch → make → neg_case → 清理）。
# 26 份样板既占文件数，也让"新增一条用例"变成"再抄一遍样板"。
# 现在样板只有一份，新增用例只需加一个 case_xxx 函数。
#
# 用法：
#   tools/neg.sh                跑全部（check-neg.sh 就是这么调的）
#   tools/neg.sh -l             只列 case 名
#   tools/neg.sh console kbd    只跑名字里含这些词的（调试单条用）
#
# 判据不变：一条负向成立，唯一标准是"把源码改坏之后，对应的正向脚本 rc≠0"。
# 判分的活仍然交给 tools/negcommon.sh（neg_begin / neg_case / neg_end），
# 这里只负责把"变异树"准备好。
#
# 每条 case 都在**副本**上改（/tmp/neg-<name>），真源码一个字节都不动——
# 老式"原地改再还原"一旦中途中断，源码就停在坏的那一版，这是踩过的坑。
set -u
R="$HOME/mini-os"
PATCH="$R/tools/negpatch.py"
. "$R/tools/negcommon.sh"

NEGTMP=/tmp/neg-x

# ---------------------------------------------------------------
# 三件套：建树 / 编树 / 在树上跑正向脚本
# ---------------------------------------------------------------
mk_tree() {
    rm -rf "$1" || return 1
    cp -r "$R" "$1" || return 1
}

build_tree() {
    ( cd "$1" && make -s >"$1.make.log" 2>&1 ) \
        || { tail -5 "$1.make.log"; return 3; }
}

run_on() {
    local t=$1 s=$2
    shift 2
    # 两类脚本定位工程目录的方式不同，必须分开：
    #   - 绝大多数读 $MINI_OS_DIR，跑真树里的脚本、把变量指到变异树；
    #   - verify.sh / mkimg.sh 用 dirname($0)/.. 定位，必须直接跑**树里那一份**，
    #     否则 ROOT 会算回真工程，变异根本没进被测镜像。
    case "$s" in
        verify.sh|qemu-debug.sh|mkimg.sh) bash "$t/tools/$s" "$@" ;;
        *) env MINI_OS_DIR="$t" bash "$R/tools/$s" "$@" ;;
    esac
}

# ---------------------------------------------------------------
# 28 组变异（函数名 = 原脚本名去掉 neg_ 与 .sh）
# 每个函数：建树 → 打补丁 → 编 → neg_case；返回非 0 = "变异做不出来"
# ---------------------------------------------------------------
case_bcache_hit() {
    local t=$NEGTMP
    mk_tree $t || return 1
    ( cd $t || return 1
      python3 "$PATCH" fs/bcache.c \
        "    if (tags[s].valid && tags[s].dev == dev && tags[s].lba == lba) {" \
        "    if (tags[s].valid && tags[s].dev == dev && tags[s].lba == lba && 0) {" \
        || return 2
      build_tree $t || return 3 ) || return 1
    neg_case "bcache-hit-always-miss" run_on $t bcache.sh
}

case_bcache_wt() {
    local t=$NEGTMP
    mk_tree $t || return 1
    ( cd $t || return 1
      python3 "$PATCH" fs/bcache.c \
        "        copy_bytes(line_bytes(s), src);" \
        "        (void)src;                  /* NEG: 写穿但不更新缓存行 */" \
        || return 2
      build_tree $t || return 3 ) || return 1
    neg_case "bcache-write-no-line-update" run_on $t bcache.sh
}

case_console() {
    local t=$NEGTMP
    mk_tree $t || return 1
    ( cd $t || return 1
      python3 "$PATCH" kernel/chr_drv/vga.c \
        "    crtc_write(CRTC_START_HI, (unsigned char)((start >> 8) & 0xFF));
    crtc_write(CRTC_START_LO, (unsigned char)(start & 0xFF));" \
        "    (void)start;                       /* NEG: 页切换失效 */" \
        || return 2
      build_tree $t || return 3 ) || return 1
    neg_case "console-no-crtc-start" run_on $t shell-console.sh
}

case_dev_hd() {
    local t=$NEGTMP
    mk_tree $t || return 1
    ( cd $t || return 1
      python3 "$PATCH" fs/block_dev.c \
        "    for (i = 0; i < BLK_MAX_DEV; i++)" \
        "    for (i = 0; i < 1; i++)  /* NEG */" \
        || return 2
      build_tree $t || return 3 ) || return 1
    neg_case "dev-hd-slot0" run_on $t dev.sh
}

case_kbd_depth() {
    local t=$NEGTMP
    mk_tree $t || return 1
    ( cd $t || return 1
      python3 "$PATCH" kernel/chr_drv/kbd_queue.c \
        '    lane_depth[tag] = depth;' \
        'lane_depth[tag] = KBD_LANE_DEF;   /* 故意忽略新值：深度回到编译期那 4 格 */' \
        || return 2
      build_tree $t || return 3 ) || return 1
    neg_case "depth-ignored" run_on $t kbd-depth.sh
}

case_kbd_echo() {
    local t=$NEGTMP
    mk_tree $t || return 1
    ( cd $t || return 1
      python3 "$PATCH" kernel/chr_drv/kbdline.c \
        "        row[i] = (unsigned short)((0x0F << 8) | (unsigned char)line_buf[i]);" \
        "(void)0;   /* 故意不写屏：函数还在、调用还在，就是不出字 */" \
        || return 2
      build_tree $t || return 3 ) || return 1
    neg_case "no-echo-render" run_on $t kbd-echo.sh
}

case_kbd_lane() {
    local t=$NEGTMP
    mk_tree $t || return 1
    ( cd $t || return 1
      python3 "$PATCH" kernel/chr_drv/kbd_queue.c \
        "    if (q_count[lane] >= depth) {" \
        "if (q_count[lane] >= KBD_LANE_MAX) {   /* 故意按最大格数算 */" \
        || return 2
      build_tree $t || return 3 ) || return 1
    neg_case "lane-capacity-16" run_on $t kbd-lane.sh
}

case_kbd_line() {
    local t=$NEGTMP
    mk_tree $t || return 1
    ( cd $t || return 1
      python3 "$PATCH" init/main.c \
        "        n = kbd_line_take(line_taken_buf, 79);" \
        "n = 0;   /* 故意不取：提交槽里的行永远留着 */" \
        || return 2
      build_tree $t || return 3 ) || return 1
    neg_case "never-take-line" run_on $t kbd-line.sh
}

case_kbd_owner() {
    local t=$NEGTMP
    mk_tree $t || return 1
    ( cd $t || return 1
      python3 "$PATCH" kernel/chr_drv/kbd_queue.c \
        "    unsigned int lane = owner_active;" \
        "unsigned int lane = 0;   /* 故意不进归属那条环 */" \
        || return 2
      build_tree $t || return 3 ) || return 1
    neg_case "no-owner-lane" run_on $t kbd-owner.sh
}

case_kbd_shell() {
    local t=$NEGTMP
    mk_tree $t || return 1
    ( cd $t || return 1
      python3 "$PATCH" shell/shell.c \
        '{ "echo",  4, cmd_echo  },' \
        '{ "echo",  5, cmd_echo  },' \
        || return 2
      build_tree $t || return 3 ) || return 1
    neg_case "shell-cmd-len-wrong" run_on $t kbd-shell.sh
}

case_kbd_subq() {
    local t=$NEGTMP
    mk_tree $t || return 1
    ( cd $t || return 1
      python3 "$PATCH" kernel/chr_drv/kbdline.c \
        "    if (submit_pending >= submit_depth) {" \
        "if (submit_pending >= 1) {   /* 故意退回一格 */" \
        || return 2
      build_tree $t || return 3 ) || return 1
    neg_case "submit-depth-1" run_on $t kbd-subq.sh
}

case_kbd_task() {
    local t=$NEGTMP
    mk_tree $t || return 1
    ( cd $t || return 1
      python3 "$PATCH" kernel/task.c \
        "kbd_owner_force(cur ? tasks[cur].kbd_tag : 0);" \
        "(void)0;   /* 故意不搬：归属永远是开局的 0 */" \
        || return 2
      build_tree $t || return 3 ) || return 1
    neg_case "no-owner-follow" run_on $t kbd-task.sh
}

# 两个补丁、两个 case（原来是两个 rm -rf / cp -r 段）
case_kbd() {
    local t=$NEGTMP

    mk_tree $t || return 1
    ( cd $t || return 1
      python3 "$PATCH" kernel/chr_drv/keyboard.c \
        "idt_set_gate(IDT_VEC_KEYBOARD, irq1_keyboard);" \
        "(void)0;" || return 2
      build_tree $t || return 3 ) || return 1
    neg_case "no-gate" run_on $t verify.sh -n 1
    rm -rf $t

    mk_tree $t || return 1
    ( cd $t || return 1
      python3 "$PATCH" kernel/chr_drv/keyboard.c \
        "outb(PIC_MASTER_MASK, (unsigned char)(inb(PIC_MASTER_MASK) & ~0x02));" \
        "(void)0;" || return 2
      build_tree $t || return 3 ) || return 1
    neg_case "no-unmask" run_on $t verify.sh -n 1
}

case_kwait() {
    local t=$NEGTMP
    mk_tree $t || return 1
    ( cd $t || return 1
      # 唤醒的筛选条件写错：等着的任务是 TASK_WAIT，判据却问 TASK_READY，
      # 于是"扫到了、但一眼就跳过"。现象是没人被叫回就绪队列→ 16 轮走不完，
      # 只能靠超时兜底退出。
      # 注：早先试过"只改 state 不改就绪队列"的变异，那个是**等价变异**——
      # 任务已经靠 rq_push 进了队列，state 只影响"时间片用尽后要不要回队"，
      # 而 ping-pong 每一步都主动 wait，永远用不满时间片。负向用例必须咬真行为。
      python3 "$PATCH" kernel/task.c \
        "if (tasks[i].state != TASK_WAIT || tasks[i].chan != chan)" \
        "if (tasks[i].state != TASK_READY || tasks[i].chan != chan)  /* NEG */" \
        || return 2
      build_tree $t || return 3 ) || return 1
    neg_case "kwait-scan-wrong-state" run_on $t kwait.sh
}

case_lease_owner() {
    local t=$NEGTMP
    mk_tree $t || return 1
    ( cd $t || return 1
      python3 "$PATCH" kernel/task.c \
        "page_lease_release_owner(reclaim_owner);" \
        "(void)reclaim_owner;" || return 2
      build_tree $t || return 3 ) || return 1
    neg_case "no-lease-release-on-death" run_on $t verify.sh -n 1
}

case_lease_table() {
    local t=$NEGTMP
    mk_tree $t || return 1
    ( cd $t || return 1
      python3 "$PATCH" kernel/task.c \
        "    for (i = 0; i < TASK_LEASES; i++)
        if (tasks[slot].leases[i] == phys)
            return 1;" \
        "    return 1;                                       /* NEG: 不记账 */" \
        || return 2
      build_tree $t || return 3 ) || return 1
    neg_case "lease-table-noop" run_on $t verify.sh -n 1
}

case_lease() {
    local t=$NEGTMP

    mk_tree $t || return 1
    ( cd $t || return 1
      python3 "$PATCH" mm/lease.c \
        "    if (leases[slot].refs > 0)
        return 0;" \
        "    /* NEG: 不看 refs，一还就关 */" || return 2
      build_tree $t || return 3 ) || return 1
    neg_case "A-close-ignores-refs" run_on $t verify.sh -n 1
    rm -rf $t

    mk_tree $t || return 1
    ( cd $t || return 1
      python3 "$PATCH" mm/lease.c \
        "        leases[i].ttl--;" \
        "        /* NEG: ttl 不递减 */" || return 2
      build_tree $t || return 3 ) || return 1
    neg_case "B-no-ttl-decrement" run_on $t verify.sh -n 1
}

case_lock() {
    local t=$NEGTMP
    mk_tree $t || return 1
    ( cd $t || return 1
      python3 "$PATCH" init/main.c \
        "    page_lock_kernel(0, KERNEL_LOCK_END);
" "    /* NEG: 不锁内核页 */" || return 2
      build_tree $t || return 3 ) || return 1
    neg_case "no-kernel-lock" run_on $t verify.sh -n 1
}

case_nomark() {
    local t=$NEGTMP
    mk_tree $t || return 1
    ( cd $t || return 1
      python3 "$PATCH" init/main.c \
        "    page_lease_open(PAGE_LEASE_OWNER_KERNEL, user_code, 200);
" "    /* NEG: 代码页不放行 */" || return 2
      build_tree $t || return 3 ) || return 1
    neg_case "no-user-code-mark" run_on $t verify.sh -n 1
}

case_notify_order() {
    local t=$NEGTMP
    mk_tree $t || return 1
    ( cd $t || return 1
      python3 "$PATCH" kernel/task.c \
        "reclaim_dtor(reclaim_arg, reclaim_reason);" \
        "if (reclaim_owner) { page_lease_release_owner(reclaim_owner); reclaim_owner = 0; } reclaim_dtor(reclaim_arg, reclaim_reason);" \
        || return 2
      build_tree $t || return 3 ) || return 1
    neg_case "reclaim-before-dtor" run_on $t verify.sh -n 1
}

case_pipe_cnt() {
    local t=$NEGTMP
    mk_tree $t || return 1
    ( cd $t || return 1
      # 经典坑：用 head==tail 判空。5 字节时 tail 追上 head 就停，看似正常；
      # 但满环时 head 正好绕回 tail，于是"满"被误判成"空"——读端拿不到 512 字节。
      python3 "$PATCH" fs/pipe.c \
        "    for (i = 0; i < len && p->cnt > 0; i++) {" \
        "    for (i = 0; i < len && p->tail != p->head; i++) {  /* NEG */" \
        || return 2
      build_tree $t || return 3 ) || return 1
    neg_case "pipe-empty-by-headtail" run_on $t pipe.sh
}

case_part_sig() {
    local t=$NEGTMP
    mk_tree $t || return 1
    ( cd $t || return 1
      python3 "$PATCH" kernel/blk_drv/part.c \
        "                                blkdev_drive(dv), start, cnt)" \
        "                                blkdev_drive(dv), 0, cnt)  /* NEG */" \
        || return 2
      build_tree $t || return 3 ) || return 1
    neg_case "part-base-dropped" run_on $t part.sh
}

case_range() {
    local t=$NEGTMP

    mk_tree $t || return 1
    ( cd $t || return 1
      python3 "$PATCH" init/main.c \
        "    page_lock_kernel(0, KERNEL_LOCK_END);" \
        "    page_lock_kernel(0, 0x00100000U);   /* NEG: 只锁 1MB */" || return 2
      build_tree $t || return 3 ) || return 1
    neg_case "A-lock-only-1MB" run_on $t verify.sh -n 1
    rm -rf $t

    mk_tree $t || return 1
    ( cd $t || return 1
      python3 "$PATCH" init/main.c \
        "    page_lease_close(PAGE_LEASE_OWNER_KERNEL, user_stack);
" "    /* NEG: 用户栈不标内核 */" || return 2
      build_tree $t || return 3 ) || return 1
    neg_case "B-no-mark-kernel" run_on $t verify.sh -n 1
}

case_shell_hist() {
    local t=$NEGTMP
    mk_tree $t || return 1
    ( cd $t || return 1
      python3 "$PATCH" shell/shell.c \
        "    ST->hist_head = (ST->hist_head + 1) % SHELL_HIST;
    if (ST->hist_n < SHELL_HIST)
        ST->hist_n++;" \
        "    ST->hist_head = (ST->hist_head + 1) % SHELL_HIST;
    (void)0;                           /* 负向补丁：历史不记账 */" \
        || return 2
      build_tree $t || return 3 ) || return 1
    neg_case "hist-not-recorded" run_on $t shell-console.sh
}

case_shell_vars() {
    local t=$NEGTMP
    mk_tree $t || return 1
    ( cd $t || return 1
      python3 "$PATCH" shell/var.c \
        "    slot = SHELL_VARS;
" "    slot = SHELL_VARS;
    return -1;                                   /* NEG: 永远写不进去 */
" || return 2
      build_tree $t || return 3 ) || return 1
    neg_case "var-set-always-fails" run_on $t shell-vars.sh
}

case_vfs_name() {
    local t=$NEGTMP
    mk_tree $t || return 1
    ( cd $t || return 1
      python3 "$PATCH" fs/namei.c \
        "        if (ent[k] != (unsigned char)nm[k])" \
        "        if (0)  /* NEG: 不比名字 */" \
        || return 2
      build_tree $t || return 3 ) || return 1
    neg_case "vfs-name-eq-dropped" run_on $t vfs.sh
}

case_vfs_rmdir() {
    local t=$NEGTMP
    mk_tree $t || return 1
    ( cd $t || return 1
      python3 "$PATCH" fs/mosfs.c \
        "    if (size != 0)" \
        "    if (0)  /* NEG */" \
        || return 2
      build_tree $t || return 3 ) || return 1
    neg_case "vfs-rmdir-nonempty" run_on $t vfs.sh
}

case_vfs_wpath() {
    local t=$NEGTMP
    mk_tree $t || return 1
    ( cd $t || return 1
      python3 "$PATCH" fs/mosfs.c \
        "    rc = dir_add(pino, leaf, ino);" \
        "    rc = dir_add(sb.root, leaf, ino);  /* NEG */" \
        || return 2
      build_tree $t || return 3 ) || return 1
    neg_case "vfs-write-to-root" run_on $t vfs.sh
}

# ---------------------------------------------------------------
# 驱动
# ---------------------------------------------------------------
CASES="bcache_hit bcache_wt console dev_hd kbd_depth kbd_echo kbd_lane
kbd_line kbd_owner kbd_shell kbd_subq kbd_task kbd kwait lease_owner lease_table
lease lock nomark notify_order part_sig pipe_cnt range shell_hist shell_vars
vfs_name vfs_rmdir vfs_wpath"

run_one() {
    local n=$1
    if ! declare -F "case_$n" >/dev/null; then
        echo "  SKIP  case_$n 不存在"
        return 0
    fi
    NEGTMP=/tmp/neg-$n
    neg_begin
    if ! "case_$n"; then
        echo "FAIL  $n：变异做不出来（补丁锚点漂了或编不过），用例本身失效"
        rm -rf "$NEGTMP"
        return 1
    fi
    neg_end
    local rc=$?
    killall -q -9 qemu-system-i386 2>/dev/null
    rm -rf "$NEGTMP"
    return $rc
}

sel=none
if [ "${1:-}" = "-l" ]; then
    for c in $CASES; do echo "  $c"; done
    exit 0
fi
if [ "${1:-}" = "-1" ]; then
    sel=${2:?-1 后面要跟一个 case 名}
fi

# 参数：`-1 <name>` 跑单条（check-neg.sh 用）；一串过滤词按包含匹配；无参 = 全部
filter=""
if [ "$sel" != none ]; then
    filter=$sel
elif [ $# -gt 0 ]; then
    filter="$*"
fi

fail=0
n=0
for c in $CASES; do
    if [ "$sel" != none ]; then
        [ "$c" = "$sel" ] || continue
    elif [ -n "$filter" ] && ! printf '%s' "$filter" | grep -q "$c"; then
        continue
    fi
    n=$((n + 1))
    printf '[%02d] %s\n' "$n" "$c"
    killall -q -9 qemu-system-i386 2>/dev/null
    rm -f /tmp/qmp.sock
    sleep 1
    run_one "$c" || fail=$((fail + 1))
done

echo "-------------------------------------------------"
if [ "$fail" = 0 ]; then
    echo "负向组：$n 条 case 全部被对应正向测试抓住"
else
    echo "负向组：$fail / $n 条 case 没通过"
fi
[ "$fail" = 0 ]
