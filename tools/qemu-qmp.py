#!/usr/bin/env python3
"""通过 QMP（JSON）执行一条 HMP 命令：不停 CPU、不用 gdb。

Usage: python3 tools/qemu-qmp.py /tmp/qmp.sock "pmemsave 0xb8000 4000 /tmp/vga.bin"

为什么不用 `-monitor unix:...`：那条路是 readline 前端，逐字符回显 + 行编辑，
从 socket 一次灌进去会被 echo/行编辑搅乱（实测 pmemsave 直接报
`invalid char 't' in expression`）。QMP 是纯 JSON 一问一答，稳。
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
        self._read()                       # 问候
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
                continue                   # 没发完，继续攒

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


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    q = Qmp(sys.argv[1])
    line = " ".join(sys.argv[2:]).split()
    # HMP 的 pmemsave 走字符串解析，路径里的 '/' 会被当成表达式（实测报
    # `invalid char 't'`）；QMP 自己有同名命令，直接用就不折腾了。
    if len(line) == 4 and line[0] in ("pmemsave", "memsave"):
        r = q.cmd(line[0], val=int(line[1], 0), size=int(line[2], 0),
                  filename=line[3])
    else:
        r = q.hmp(" ".join(line))
    q.close()
    ret = r.get("return", r)
    if isinstance(ret, str):
        sys.stdout.write(ret.replace("\r", ""))   # 原样输出，不做 JSON 转义
        if not ret.endswith("\n"):
            sys.stdout.write("\n")
    else:
        print(json.dumps(ret))


if __name__ == "__main__":
    main()
