#!/usr/bin/env python3
"""通过 QMP 的 `sendkey` 注入按键（不停 CPU、不用 gdb）。

Usage: python3 tools/qemu-sendkey.py /tmp/qmp.sock h e l l o

环境变量 KBD_GAP 控制键间隔（秒，默认 0.25）；QEMU 自己按下到抬起约 100ms。
"""
import json
import os
import socket
import sys
import time


def connect(path):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(5.0)
    s.connect(path)
    read(s)                                # 问候
    exec_cmd(s, "qmp_capabilities")
    return s


def read(s):
    buf = b""
    while True:
        try:
            chunk = s.recv(4096)
        except socket.timeout:
            return json.loads(buf.decode() or "{}")
        if not chunk:
            return json.loads(buf.decode() or "{}")
        buf += chunk
        try:
            return json.loads(buf.decode())
        except ValueError:
            continue


def exec_cmd(s, name, **args):
    msg = {"execute": name}
    if args:
        msg["arguments"] = args
    s.sendall((json.dumps(msg) + "\n").encode())
    return read(s)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    s = connect(sys.argv[1])
    gap = float(os.environ.get("KBD_GAP", "0.25"))
    for k in sys.argv[2:]:
        exec_cmd(s, "human-monitor-command",
                 **{"command-line": "sendkey %s" % k})
        time.sleep(gap)                    # 让 8042 有处理时间，别挤在一个 tick 里
    s.close()


if __name__ == "__main__":
    main()
