#!/usr/bin/env python3
"""把 VGA 屏幕输出和基准文件做比对，并对“允许抖动”的数字做区间断言。

两类检查分开做，因为它们回答的问题不同：

1. 结构化比对（diff）：去掉 volatile 数字之后，24 行必须逐字一致。
   这抓到的是“布局/文案/行数/顺序变了”——比如多打一行把第 25 行顶掉。
2. 区间断言（range）：抖动项不能只靠“看起来差不多”，必须落在物理上合理的
   范围里。这抓到的是“数字错得离谱但格式没变”——比如 dtick 掉到 3、
   或阶段四一次 yield 都没生效（sw 从 0x2EEF 掉到 0x8）。

用法：
    tools/verify.py --actual /tmp/screen.txt --expect tests/expect/boot-screen.txt
    tools/verify.py --actual /tmp/screen.txt --update      # 有意改了屏幕就重写基准
"""

import argparse
import re
import sys

# "row  3| ..." —— vga-screen.py 的行格式，序号部分可能补空格
ROW_RE = re.compile(r"^row\s*(\d+)\|(.+)$")

# 会被“时钟频率 / 调度时序”影响的数字：比对前统一抹成占位符
VOLATILE = [
    (re.compile(r"esp=0x[0-9A-F]+"), "esp=0x<N>"),
    (re.compile(r"cnt=0x[0-9A-F]+"), "cnt=0x<N>"),
    (re.compile(r"dtick=0x[0-9A-F]+"), "dtick=0x<N>"),
    (re.compile(r"\btick=0x[0-9A-F]+"), "tick=0x<N>"),
    (re.compile(r"ms=0x[0-9A-F]+"), "ms=0x<N>"),
    (re.compile(r"gap=0x[0-9A-F]+"), "gap=0x<N>"),
    # 阶段三合并行里 C/B 各自的睡眠唤醒间隔
    (re.compile(r"gc=0x[0-9A-F]+"), "gc=0x<N>"),
    (re.compile(r"gb=0x[0-9A-F]+"), "gb=0x<N>"),
    (re.compile(r"\bt=0x[0-9A-F]+"), "t=0x<N>"),
    (re.compile(r"sw=0x[0-9A-F]+"), "sw=0x<N>"),
    (re.compile(r"Y=0x[0-9A-F]+/0x[0-9A-F]+/0x[0-9A-F]+"),
     "Y=0x<N>/0x<N>/0x<N>"),
    # 每阶段的 tick 账单会有一两个 tick 落在记账边界（正好在换栈/退出瞬间，
    # 那个 tick 记给了 main），所以 U/V/H/L 也不能当零容差项
    (re.compile(r"\bU=0x[0-9A-F]+"), "U=0x<N>"),
    (re.compile(r"\bV=0x[0-9A-F]+"), "V=0x<N>"),
    (re.compile(r"\bH=0x[0-9A-F]+"), "H=0x<N>"),
    (re.compile(r"\bL=0x[0-9A-F]+"), "L=0x<N>"),
    # 长睡回血之后的级别（阶段六那一行的后半段）
    # 长睡回血的三段对照（默认阈值 / 阈值抬到 60 / 改回 30）
    (re.compile(r"rest=0x[0-9A-F]+/0x[0-9A-F]+/0x[0-9A-F]+"),
     "rest=0x<N>/0x<N>/0x<N>"),
    # 线性回血的两段（睡 2 个单价 / 睡 1 个单价之后的级别）
    (re.compile(r"deep=0x[0-9A-F]+/0x[0-9A-F]+"), "deep=0x<N>/0x<N>"),
    # 0x31 系统调用的三段返回值（旧值 / 非法值被拒 / 拒后仍保持）
    (re.compile(r"\|tune [0-9A-F]{2}/[0-9A-F]{2}/[0-9A-F]{2}"),
     "|tune <N>/<N>/<N>"),
]


def normalize(text):
    for pat, repl in VOLATILE:
        text = pat.sub(repl, text)
    return text


def parse_rows(path):
    rows = {}
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            m = ROW_RE.match(line.rstrip("\n"))
            if m:
                rows[int(m.group(1))] = m.group(2)
    return rows


def hexval(text, key):
    m = re.search(key + r"=0x([0-9A-F]+)", text)
    return int(m.group(1), 16) if m else None


def check_ranges(rows):
    """每条断言都写清‘为什么是这个范围’，否则半年后没人敢动它。"""
    fails = []

    def need(row, key, lo, hi, why):
        val = hexval(rows.get(row, ""), key)
        if val is None:
            fails.append(f"row {row}: 找不到 {key}=，行内容 {rows.get(row, '')!r}")
        elif not (lo <= val <= hi):
            fails.append(f"row {row}: {key}=0x{val:X} 不在 0x{lo:X}..0x{hi:X}（{why}）")

    # 分频值读回：必须小于装载值 0x2E9C，否则说明 8254 没真的被重编程
    need(3, "cnt", 0x1, 0x2E9B, "读回的计数器应小于分频值")
    # 100Hz 下一个整秒理论 100 tick；TCG 服务速率有限，实测 70 上下（对照组 18/49/112）
    need(4, "dsec", 0x1, 0x1, "RTC 两次整秒边界之间应正好差 1 秒")
    need(4, "dtick", 0x28, 0x78, "dtick 抖动允许 40..120")

    # 睡眠精度：50ms 应为 5 tick、100ms 应为 10 tick，各留 ±1 的打印抖动。
    # 阶段三的 C/B 两行合并成一行（把行数让给 ring3 那行），所以两段都落在 row 16。
    need(16, "gc", 0x4, 0x7, "C 睡 50ms → gc 约 5")
    need(16, "gb", 0x9, 0xB, "B 睡 100ms → gb 约 10")
    # 合并之后“C 先醒”的证据从行序换成了显式的 wake_seq（C 记 1、B 记 2 → 0x12）。
    # 反过来就是 0x21：睡眠队列退化成了“谁先睡谁先醒”。
    wseq = hexval(rows.get(16, ""), "seq")
    if wseq != 0x12:
        fails.append(
            f"row 16: 醒来顺序 seq=0x{wseq or 0:X} 应为 0x12（C 50ms 先于 B 100ms；"
            "0x21 说明睡眠队列没按到期时间排序）")

    # 阶段一 / 阶段二合并在 row 12（屏幕 24 行不够，两段账目挤一行）：
    #   p1 U/V 是两个同优先级任务对半分 12 tick；p2 H/L 是优先级 + 老化保底；
    #   s1/sw 是两段各自的切换次数。
    u = hexval(rows.get(12, ""), "U")
    v = hexval(rows.get(12, ""), "V")
    if u is None or v is None:
        fails.append(f"row 12: 取不到阶段一的 U/V → {rows.get(12, '')!r}")
    else:
        for name, val in (("U", u), ("V", v)):
            if not (0x4 <= val <= 0x8):
                fails.append(f"row 12: {name}=0x{val:X} 超出 4..8（12 tick 该对半分）")
        if not (0xA <= u + v <= 0xC):
            fails.append(f"row 12: U+V=0x{u + v:X} 不在 10..12（有 tick 记漏了）")

    h = hexval(rows.get(12, ""), "H")
    l = hexval(rows.get(12, ""), "L")
    if h is None or l is None:
        fails.append(f"row 12: 取不到阶段二的 H/L → {rows.get(12, '')!r}")
    else:
        if not (0x7 <= h <= 0xB):
            fails.append(f"row 12: H=0x{h:X} 超出 7..11（优先级应当主导）")
        if not (0x1 <= l <= 0x4):
            fails.append(f"row 12: L=0x{l:X} 超出 1..4（0 就是饿死，说明老化没生效）")
        if h <= l:
            fails.append(f"row 12: H=0x{h:X} 不大于 L=0x{l:X}（优先级形同虚设）")

    # 两个对外契约（row 13）：调用门的 DPL 是从 GDT 描述符里读回来的，
    # 白名单外写返回 0、白名单内写返回旧值；参数所有权靠 dtor 计数 + 载荷清零。
    dpl = hexval(rows.get(13, ""), "dpl")
    if dpl != 0x3:
        fails.append("row 13: 调用门 dpl 应为 0x3（描述符 DPL 位没摆对，ring3 将 call 不进来）")
    deny = hexval(rows.get(13, ""), "deny")
    if deny != 0x0:
        fails.append("row 13: 白名单外的写返回非 0（老化字段本该被门拒掉，白名单没生效）")
    old = hexval(rows.get(13, ""), "old")
    if old != 0x1E:
        fails.append("row 13: 白名单内的写应返回旧值 0x1E（回血阈值 30）")
    dtor = hexval(rows.get(13, ""), "dtor")
    if dtor != 0x1:
        fails.append("row 13: dtor 调用次数应为 1（没交接参数所有权，或重复析构）")
    mag = hexval(rows.get(13, ""), "mag")
    if mag != 0x0:
        fails.append("row 13: 载荷 magic 应为 0（析构没跑，所有权没真正交出去）")
    seen = hexval(rows.get(13, ""), "seen")
    if seen != 0x1:
        fails.append("row 13: 任务自己没确认载荷（传参断了，或载荷在任务活着时就被改了）")
    why = hexval(rows.get(13, ""), "why")
    if why != 0x0:
        fails.append(
            "row 13: dtor 收到的死因应为 0x0（入口 return 就该报 RETURN；"
            "不是 0 说明死因没随所有权一起交到 dtor 手上）")

    # ring3（row 14）：这一行是“跨特权级真的发生”的全部证据，缺一条就得怀疑：
    #   tr   —— TSS 真的装上了（ltr 之后 str 读回 0x30）；没有它 ring3 的 lcall
    #           会在换栈那一步 #GP(0)
    #   cpl  —— 门处理体里看到的调用者 CS 低两位 = 3，证明这次调用真的来自 ring3，
    #           而不是“内核自己 lcall 了一遍”（后者是 0）
    #   ret  —— 第一次门调用返回的旧值 0x1E，由**用户态代码自己**写进栈页；
    #           它同时证明 retf 回到了 ring3 且 eax 带回了值
    #   rej  —— 两次被拒都返回 0：`00/00` = 超每字段上限（5000 > 256）一个 0，
    #           野指针 0x3000 不在用户窗口一个 0（后者还证明内核没有替用户 deref）
    #   set  —— 被放行那次真的写进去了（restore_ticks 变 0x28=40）
    #   cap  —— 门的可写上限是表里的 0x100（不是硬编码，也不是无上限）
    #   knl  —— 四次越权探测的错误码连写，见下面单独那一段
    #   ls   —— 租借账：还清最后一个借用者时那一页是否真关上 / 超时强制回收页数
    if hexval(rows.get(14, ""), "tr") != 0x30:
        fails.append(f"row 14: TR=0x{hexval(rows.get(14, ''), 'tr')} 应为 0x30"
                     "（ltr 没生效，ring3 的 lcall 会在换栈处 #GP）")
    if hexval(rows.get(14, ""), "cpl") != 0x3:
        fails.append("row 14: 门里看到的 CPL 应为 3（不是 3 说明这次调用并非来自 ring3）")
    if hexval(rows.get(14, ""), "ret") != 0x1E:
        fails.append("row 14: 用户态拿回的旧值应为 0x1E（门返回没回到 ring3，或 eax 没带值）")
    if "rej=00/00" not in rows.get(14, ""):
        fails.append(f"row 14: 两次被拒的写都应返回 00/00 → {rows.get(14, '')!r}"
                     "（cap 或窗口校验没拦住）")
    if hexval(rows.get(14, ""), "set") != 0x28:
        fails.append("row 14: 放行那次应把 restore_ticks 改成 0x28=40（写没真正生效）")
    if hexval(rows.get(14, ""), "cap") != 0x100:
        fails.append("row 14: 门的每字段上限应为 0x100（cap 表丢了或被写死）")
    # knl 是四位数字连写（不是 0x 形式）：依次是
    #   [0] 0x8000 内核 text（1MB 以内）        → 5
    #   [1] 0xA00000 1MB 以上的空白恒等映射页   → 5（证明锁的范围不止 1MB）
    #   [2] 用户栈页 refs 从 2 还到 1 之后      → 0（还有借用者，页仍开着）
    #   [3] 同一页 refs 归零之后                → 5（最后一个借用者说了算）
    if "knl=5505" not in rows.get(14, ""):
        fails.append(f"row 14: 应为 knl=5505（低区内核页/高区空白页/refs=1 的用户页/"
                     f"refs=0 的同一页）→ {rows.get(14, '')!r}"
                     "；中间那位是 0 说明 page_lock_kernel 的范围没盖到 1MB 以上，"
                     "末位是 0 说明 refs 归零时没真的关上")
    if "ls=1/3/1" not in rows.get(14, ""):
        fails.append(f"row 14: 应为 ls=1/3/1（超时强制回收 1 页 / 按借用表随任务收回 3 页 / 撤销通知 1 次且晚于 dtor）"
                     f"→ {rows.get(14, '')!r}"
                     "；前一位不是 1 说明 ttl 到 0 没回收，中一位不是 3 说明没按这张任务自己的表逐页收，"
                     "末位是 0 说明通知没来、或通知比 dtor 先到")
    # 键盘（IRQ1）：PIC 屏蔽字读回必须是 0xFC（IRQ0/IRQ1 放行，其余仍屏蔽）；
    # irq=1 是 `int 0x21` 软触发真门后的计数；q=3 是解码后队列里的 "Hei"；
    # sh=1 说明 Shift+h 真的出了大写 H。
    if "| [kbd] mk=0xFC irq=0x01 q=0x3 sh=0x1" not in rows.get(5, ""):
        fails.append(f"row 5: 应为 [kbd] mk=0xFC irq=0x01 q=0x3 sh=0x1"
                     f"→ {rows.get(5, '')!r}"
                     "；mk 不是 0xFC 说明 IRQ1 没放开（或被别的位污染），"
                     "irq=0 说明 0x21 的门没指向真桩，q 不是 3 说明解码/入队不对，"
                     "sh=0 说明 Shift 状态没生效")
    if "21 kbd" not in rows.get(2, ""):
        fails.append(f"row 2: idt 行里应有 21 kbd（键盘门已装）→ {rows.get(2, '')!r}")
    if "lt=0x1/0x4" not in rows.get(17, ""):
        fails.append(f"row 17: 应为 lt=0x1/0x4（借用表满拒绝 1 次 / 水位 4 = 容量）"
                     f"→ {rows.get(17, '')!r}；前一位是 0 说明表满了还在借（没记就不该放行），"
                     "后一位不是 4 说明表没真记满或容量变了")

    # 阶段三：参数是“压进新栈”传过去的，不是靠全局变量偷渡。
    # B 拿到 main 给的 0xDEAD0001 并原样转手 spawn D，两行都对才能证明链路的第二跳没丢参。
    if "null=0x00000000" not in rows.get(15, ""):
        fails.append(f"row 15: 取不到空入口被拒的证据 → {rows.get(15, '')!r}"
                     "（应是 null=0x00000000；非 0 说明入口为空时也建了任务）")
    for row in (16, 17):
        a = hexval(rows.get(row, ""), "arg")
        if a is None:
            fails.append(f"row {row}: 取不到 arg= → {rows.get(row, '')!r}")
        elif a != 0xDEAD0001:
            fails.append(f"row {row}: arg=0x{a:X} != 0xDEAD0001（参数没传到 [esp+4]；"
                         "0x0 通常是 spawn 时没把 arg 摆进栈帧）")

    # 阶段四：yield 生效时切换次数是 tick 数的三个数量级；退化成 tick 自带切换就只有个位数
    need(18, "sw", 0x1000, 0x200000, "yield 生效时 sw 必须远大于 tick 数")
    # 三个任务 tick 均分：12 tick 分三份，每个都不该超过 8
    ys = re.search(r"Y x3: Y=0x([0-9A-F]+)/0x([0-9A-F]+)/0x([0-9A-F]+)",
                   rows.get(18, ""))
    if not ys:
        fails.append("row 18: 阶段四行的格式变了，取不到三个 Y 值")
    else:
        # 口径是“三个之和 = 这一段采到的 tick 总数，且谁都不超过 8”：
        # 单个 Y=0 属于正常抖动（某个任务两轮里一次没被轮到），单值下界 1 会误报。
        vals = [int(v, 16) for v in ys.groups()]
        for i, v in enumerate(vals, 1):
            if v > 0x8:
                fails.append(
                    f"row 18: Y{i}=0x{v:X} 超过 8（单个任务独占整段 tick，yield 没均摊）")
        if not (0x5 <= sum(vals) <= 0x20):
            fails.append(f"row 18: Y 之和={sum(vals)} 不在 5..32（没采到整段 tick）")

    # 阶段五：睡眠堆必须按 wake_tick 排序，不能按创建顺序。4 个 sleeper 睡
    # 100/200/50/150ms、创建顺序 slot1..4，正确顺序是 3,1,4,2（= 0x3142）。
    # 这条不能只靠“结构比对”，因为它正是最容易被一次“重构”悄悄改成 FIFO 的地方。
    order_m = re.search(r"sq order:\s*0x([0-9A-F]+)", rows.get(19, ""))
    order = int(order_m.group(1), 16) if order_m else None
    if order is None:
        fails.append(f"row 19: 取不到睡眠堆的醒来顺序 → {rows.get(19, '')!r}")
    elif order != 0x3142:
        fails.append(
            f"row 19: 醒来顺序 0x{order:04X} != 0x3142（50/100/150/200ms 应依次 3/1/4/2；"
            "若是 0x1234 说明队列退化成按创建顺序）")

    # 阶段六：历史惩罚必须真的把它从 prio2 压到 prio0。这条不能用区间——
    # 衰减的语义就是“精确降到最低级”；若降级逻辑被删掉，这行会是 2 -> 2。
    decay = re.search(r"prio 0x([0-9A-F]+) -> 0x([0-9A-F]+)", rows.get(20, ""))
    if not decay:
        fails.append(f"row 20: 取不到衰减前后的优先级 → {rows.get(20, '')!r}")
    else:
        start, end = int(decay.group(1), 16), int(decay.group(2), 16)
        if (start, end) != (0x2, 0x0):
            fails.append(
                f"row 20: 衰减 0x{start:X} -> 0x{end:X} != 0x2 -> 0x0（4 片一级、8 个 tick 该压到最低级）")
        burned = hexval(rows.get(20, ""), "ticks")
        if burned is None or not (0x8 <= burned <= 0xA):
            fails.append(f"row 20: 烧掉 ticks=0x{burned or 0:X} 不在 8..10（slice=1 时每片 1 tick）")

    # 长睡回血：衰减到 prio0 之后再睡 400ms（40 tick > 阈值 30 tick），级别该还回一级。
    # 允许 1..2：醒来若在队列里排了一会儿会被老化临时抬高一级；0 就说明回血没生效。
    # 长睡回血 + 参数在线可调：同一个任务睡三次 400ms，中间把阈值从 30 抬到 60。
    # 期望三段：回血(1..2) / 不回血(0) / 又回血(1..2)。中间那个 0 是“参数真的
    # 在起作用”的指纹；若 task_sleep 里改成写死常量，它会变 1。
    rest = re.search(r"rest=0x([0-9A-F]+)/0x([0-9A-F]+)/0x([0-9A-F]+)", rows.get(20, ""))
    if not rest:
        fails.append(f"row 20: 取不到回血三段值 → {rows.get(20, '')!r}")
    else:
        r_default, r_high, r_back = (int(g, 16) for g in rest.groups())
        if not (0x1 <= r_default <= 0x2):
            fails.append(
                f"row 20: 默认阈值下长睡之后级别=0x{r_default:X} 不在 1..2（回血没生效）")
        if r_high != 0:
            fails.append(
                f"row 20: 阈值抬到 60 tick 之后级别=0x{r_high:X} 应为 0"
                "（不为 0 说明 task_sleep 没读 sched_tune.restore_ticks，参数是死常量）")
        if not (0x1 <= r_back <= 0x2):
            fails.append(
                f"row 20: 阈值改回 30 之后级别=0x{r_back:X} 不在 1..2（改回来没生效）")

    # 线性回血：单价 30 tick。睡 600ms（60 tick = 2 个单价）该一次还两级，
    # 睡 300ms（30 tick = 1 个单价）只还一级。旧实现（够阈值就还一级）两段都会是 1，
    # 所以 ④ 的 2 就是“按倍数折算”与“够阈值就还一级”的分界线。
    deep = re.search(r"deep=0x([0-9A-F]+)/0x([0-9A-F]+)", rows.get(20, ""))
    if not deep:
        fails.append(f"row 20: 取不到线性回血两段值 → {rows.get(20, '')!r}")
    else:
        d_long, d_short = int(deep.group(1), 16), int(deep.group(2), 16)
        if d_long != 0x2:
            fails.append(
                f"row 20: 睡 2 个单价之后级别=0x{d_long:X} 应为 0x2"
                "（不是按倍数折算：可能又写回了“够阈值就还一级”）")
        if not (0x1 <= d_short <= 0x2):
            fails.append(
                f"row 20: 睡 1 个单价之后级别=0x{d_short:X} 不在 1..2（一个单价至少要还一级）")

    # 阶段七：栈溢出必须被 guard 页拦住，而且只死那个任务（内核继续跑到 done 行）。
    # 三个条件一起看：命中的是 slot 1、地址落在栈页下面那一页、结果是 killed。
    guard = re.search(r"\[sched\] guard: slot=0x([0-9A-F]+) va=0x([0-9A-F]+)", rows.get(21, ""))
    if not guard:
        # 走到这里通常是 guard 没拦住：worker 会打 not-trapped
        fails.append(
            f"row 21: 取不到 guard 命中行 → {rows.get(21, '')!r}"
            "（若打出 not-trapped，说明越界写静默成功了，guard 页没生效）")
    else:
        if int(guard.group(1), 16) != 0x1:
            fails.append(f"row 21: 命中 slot=0x{guard.group(1)}，应当是 0x1")
        va = int(guard.group(2), 16)
        if not (0x10000 <= va < 0x90000):
            fails.append(f"row 21: 缺页地址 0x{va:X} 不在页分配器区间内（guard 页应该是低端物理页）")
        if "task killed" not in rows.get(21, ""):
            fails.append(f"row 21: 没有把任务杀掉 → {rows.get(21, '')!r}")
        # 死因必须沿“被守卫杀掉”这条路径传到 dtor：2 = TASK_DEATH_GUARD。
        # 若仍是 0，说明缺页处理把 eip 换回了 task_exit，dtor 被当成正常收工。
        gwhy = hexval(rows.get(21, ""), "why")
        if gwhy != 0x2:
            fails.append(
                f"row 21: 被 guard 杀掉的任务，dtor 收到的死因=0x{gwhy or 0:X} 应为 0x2"
                "（死因没沿 guard 路径传出去，析构会把横死当正常收工）")

    # 队列必须排空、栈页必须全还（guard 页也在内：任务退出时要连它一起还）。
    # 14 = 3（内核自己 + 一个栈页 + 一个 guard 页的基线）+ 2 张常驻的 ring3 页
    # （用户代码页与用户栈页）+ 8 张每任务页目录的常数页（slot 1..4 各一张 PDE +
    # 一张私有 PT0）+ 1 张块缓存页（bcache 从分配器要的一页，8 × 512 字节）。
    # 四类页都没有回收：这个内核还没有"进程销毁"这个
    # 概念（ring3 的 demo 跑完就停在原地），页目录是槽位的常驻资源。
    # 这条断言因此同时钉住四件事——"worker 的栈页都还了"、"ring3 只占两页"、
    # "页目录只占 8 页，没有随任务趟数增长"、"块缓存只占一页"。
    if "rq=0" not in rows.get(22, "") or "sq=0" not in rows.get(22, ""):
        fails.append(f"row 22: 队列没排空 → {rows.get(22, '')!r}")
    if "frames=0x0000000E/0x0000006B" not in rows.get(22, ""):
        fails.append(
            f"row 22: 页数应为 14/0x6B（3 基线 + 2 常驻 ring3 页 + 8 页页目录 + 1 页块缓存；"
            f"总页 107 因为 PAGES_BASE 抬到 0x25000）"
            f" → {rows.get(22, '')!r}")

    # int 0x31 系统调用：三段必须精确是 1E/00/3C。
    #   1E = 旧值（默认阈值 30，说明返回值真的回传了）；
    #   00 = 非法值 0 被范围校验拒绝（返回值 0）；
    #   3C = 拒绝之后 restore_ticks 仍是 60（说明“拒绝”不是“写了再改回”）。
    tune = re.search(r"tune ([0-9A-F]{2})/([0-9A-F]{2})/([0-9A-F]{2})", rows.get(22, ""))
    if not tune:
        fails.append(f"row 22: 取不到 0x31 的三段值 → {rows.get(22, '')!r}")
    else:
        t_old, t_bad, t_kept = (int(g, 16) for g in tune.groups())
        if t_old != 0x1E:
            fails.append(
                f"row 22: 0x31 返回的旧值=0x{t_old:02X} 应为 0x1E（返回值没回传或参数没生效）")
        if t_bad != 0x00:
            fails.append(
                f"row 22: 非法值 0 的返回值=0x{t_bad:02X} 应为 0（范围校验没拦住）")
        if t_kept != 0x3C:
            fails.append(
                f"row 22: 拒绝之后阈值=0x{t_kept:02X} 应为 0x3C（不该被非法值改写）")
    return fails


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--actual", required=True, help="本次跑出来的行（grep '^row'）")
    ap.add_argument("--expect", default="tests/expect/boot-screen.txt")
    ap.add_argument("--update", action="store_true", help="用本次输出重写基准文件")
    args = ap.parse_args()

    actual = parse_rows(args.actual)
    if not actual:
        print("FAIL: 没有解析到任何 row 行")
        return 1

    if args.update:
        with open(args.expect, "w", encoding="utf-8") as fh:
            for idx in sorted(actual):
                fh.write(f"row {idx:2d}|{normalize(actual[idx])}\n")
        print(f"baseline updated: {args.expect} ({len(actual)} rows)")
        return 0

    expect = parse_rows(args.expect)
    fails = []

    if set(actual) != set(expect):
        fails.append(
            f"行数/行号不一致：实际 {sorted(actual)} vs 基准 {sorted(expect)}"
            "（多一行会顶掉第 25 行，少一行说明有输出没打出来）")

    for idx in sorted(set(actual) & set(expect)):
        a = normalize(actual[idx])
        e = normalize(expect[idx])
        if a != e:
            fails.append(f"row {idx} 不一致\n    实际: {a}\n    基准: {e}")

    fails += check_ranges(actual)

    if fails:
        print(f"FAIL ({len(fails)} 项)")
        for f in fails:
            print("  - " + f)
        return 1

    print(f"PASS  {len(actual)} 行结构比对 + 区间断言全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
