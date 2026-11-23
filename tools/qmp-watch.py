#!/usr/bin/env python3
"""连续采样 ring3 现场：一趟 QMP 连接、循环读 CS 与若干内存字。

Usage: python3 tools/qmp-watch.py <sock> <addr(hex)> <次数> <需要几次 ring3> [间隔 ms] [额外地址...]

为什么不用反复起 qemu-qmp.py：每起一次进程要 ~100ms，40 次就是 4 秒，
ring3 那段忙等（一两秒）会被采样间隔直接跳过。这里一条连接反复问，
间隔降到几十毫秒，才抓得住"程序正跑在 ring3 的那一刻"。

额外地址（一般是 tasks[i].esp0）跟着一起读、一起打在同一行：
这样才能在"程序正跑在 ring3"的同一瞬间证明"TSS.esp0 就是某个槽位的 esp0"，
事后再去读那张表就已经被清空了（槽位在任务死的那一刻就 UNUSED 了）。

输出：每次采样一行 `S <cs> <addr值> [额外值...]`，最后一行
`SUMMARY 采样次数 ring3次数 首个ring3时的addr值`。
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


def word(q, addr):
    """xp /1dw 的输出是十进制（实测），取冒号后面那个数。"""
    r = q.hmp("xp /1dw 0x%x" % addr)
    t = r.get("return", "")
    try:
        return int(t.split(":")[1].strip(), 10)
    except (IndexError, ValueError):
        return -1


def cs_of(q):
    """`info registers` 里那一行是 "CS =0023 00000000 ffffffff ..."，
    第二段带 '=' 前缀，取它去掉 '=' 就是选择子。"""
    r = q.hmp("info registers")
    for ln in r.get("return", "").splitlines():
        f = ln.split()
        if len(f) >= 2 and f[0] == "CS":
            try:
                return int(f[1].lstrip("="), 16)
            except ValueError:
                return -1
    return -1


def main():
    if len(sys.argv) < 5:
        sys.exit(__doc__)
    sock, addr = sys.argv[1], int(sys.argv[2], 0)
    n, need = int(sys.argv[3]), int(sys.argv[4])
    gap = (int(sys.argv[5]) if len(sys.argv) > 5 else 20) / 1000.0
    extra = [int(a, 0) for a in sys.argv[6:]]

    q = Qmp(sock)
    seen = 0
    first = 0
    n_samp = 0
    for _ in range(n):
        cs = cs_of(q)
        esp0 = word(q, addr)
        cols = [word(q, a) for a in extra]
        n_samp += 1
        print("S %04x %d%s" % (cs, esp0, "".join(" %d" % c for c in cols)), flush=True)
        if cs >= 0 and (cs & 3) == 3:
            if not seen:
                first = esp0
            seen += 1
            if seen >= need:
                break
        time.sleep(gap)
    q.close()
    print("SUMMARY %d %d %d" % (n_samp, seen, first))


if __name__ == "__main__":
    main()
