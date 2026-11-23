#!/usr/bin/env python3
"""在 ring3 跑着的时候盯租约表：证明"页归任务之后，时间不再动它的 ttl"。

Usage: python3 tools/lease-watch.py <sock> <leases_addr> <ticks_addr> [次数] [间隔ms]

为什么需要它：`page_lease_open` 给内核替别人借的页定了一个 ttl（500 tick），
意思是"我忘了关，你就替我收"。可 ring3 一旦放开 IF，用户态跑久了 tick 一路涨，
这个兜底就会**在别人正跑着的页上**把 U/S 收走。所以 exec 在 spawn 之后要把
这几笔租约 handoff 到任务槽位上、改成 PAGE_LEASE_NO_TTL（0xFFFFFFFF）：
从此回收的唯一触发点是"槽位死了"，时间不再是理由。

这个工具就是那条断言的取证方式（全在 ring3 采样内完成）：
  1. 至少有一笔租约的 ttl 是 0xFFFFFFFF（handoff 生效）；
  2. 同一笔（同 owner+phys）被采到 ≥2 次、ttl 一次都没变；
  3. 同一段时间里 tasks[1].ticks 在涨 —— 说明时钟真的在走，它就是不碰这页。
三条都满足才打印 `VERDICT ok`，否则 `VERDICT fail <原因>`。

leases 表布局（struct page_lease）：owner 0 / phys 4 / refs 8 / ttl 12，
16 字节一笔，共 8 笔。
"""
import json
import socket
import sys
import time


class Qmp:
    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.s.settimeout(5.0)
        self.s.connect(path)
        self._read()
        self.cmd("qmp_capabilities")

    def _read(self):
        buf = b""
        while True:
            try:
                chunk = self.s.recv(4096)
            except socket.timeout:
                return json.loads(buf.decode() or "{}")
            if not chunk:
                return json.loads(buf.decode() or "{}")
            buf += chunk
            try:
                return json.loads(buf.decode())
            except ValueError:
                continue

    def cmd(self, name, **args):
        msg = {"execute": name}
        if args:
            msg["arguments"] = args
        self.s.sendall((json.dumps(msg) + "\n").encode())
        return self._read()

    def hmp(self, line):
        return self.cmd("human-monitor-command", **{"command-line": line})

    def close(self):
        self.s.close()


def dump_words(q, addr, n):
    """xp /Ndw 一次取回 n 个双字（输出是十进制，四列一行）。"""
    r = q.hmp("xp /%ddw 0x%x" % (n, addr)).get("return", "")
    vals = []
    for ln in r.splitlines():
        if ":" not in ln:
            continue
        for tok in ln.split(":", 1)[1].split():
            try:
                vals.append(int(tok, 10))
            except ValueError:
                pass
    return vals


def cs_of(q):
    r = q.hmp("info registers").get("return", "")
    for ln in r.splitlines():
        f = ln.split()
        if len(f) >= 2 and f[0] == "CS":
            try:
                return int(f[1].lstrip("="), 16)
            except ValueError:
                return -1
    return -1


NO_TTL = 0xFFFFFFFF


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    sock = sys.argv[1]
    leases = int(sys.argv[2], 0)
    ticks = int(sys.argv[3], 0)
    n = int(sys.argv[4]) if len(sys.argv) > 4 else 200
    gap = (int(sys.argv[5]) if len(sys.argv) > 5 else 20) / 1000.0

    q = Qmp(sock)
    ring = 0
    seen = {}            # (owner, phys) -> [ttl0, tick0, tickN, n, changed]
    for _ in range(n):
        cs = cs_of(q)
        if cs >= 0 and (cs & 3) == 3:
            ring += 1
            vals = dump_words(q, leases, 32)
            t = dump_words(q, ticks, 1)
            tv = t[0] if t else -1
            for k in range(8):
                o, p, _r, ttl = vals[4 * k:4 * k + 4]
                if not p:
                    continue
                key = (o, p)
                if key not in seen:
                    seen[key] = [ttl, tv, tv, 1, 0]
                else:
                    rec = seen[key]
                    if ttl != rec[0]:
                        rec[4] = 1
                    rec[2] = tv
                    rec[3] += 1
        if ring >= 6:            # 采够了就停：ring3 那段只有一两秒
            break
        time.sleep(gap)
    q.close()

    print("RING %d" % ring)
    picked = None
    for (o, p), rec in seen.items():
        if rec[0] == NO_TTL:
            picked = (o, p, rec)
            break
    if ring < 2:
        print("VERDICT fail ring3 采样只有 %d 次，抓不到" % ring)
    elif not picked:
        print("VERDICT fail ring3 期间没有一笔租约是 NO_TTL（handoff 没生效？）")
    else:
        o, p, rec = picked
        print("SLOT owner=%d phys=0x%x ttl=0x%x samples=%d tick=%d..%d"
              % (o, p, rec[0], rec[3], rec[1], rec[2]))
        if rec[3] < 2:
            print("VERDICT fail 同一笔只看过一次，没法证明 ttl 不随时间走")
        elif rec[4]:
            print("VERDICT fail ttl 在采样期间变了（还有东西在扣它）")
        elif rec[2] <= rec[1]:
            print("VERDICT fail tick 在 ring3 期间没涨，时钟没进来，不变不算证据")
        else:
            print("VERDICT ok")


if __name__ == "__main__":
    main()
