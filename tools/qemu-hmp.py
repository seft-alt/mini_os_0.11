#!/usr/bin/env python3
"""发一条 HMP monitor 命令并打印返回（不依赖 gdb、不停 CPU）。

Usage: python3 tools/qemu-hmp.py /tmp/mon.sock "pmemsave 0xb8000 4000 /tmp/vga.bin"
"""
import socket
import sys
import time


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    path = sys.argv[1]
    cmd = " ".join(sys.argv[2:])
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(3.0)
    s.connect(path)
    time.sleep(0.15)
    try:
        s.recv(65536)                    # 吃掉问候
    except socket.timeout:
        pass
    s.sendall((cmd + "\n").encode())
    time.sleep(0.3)
    out = b""
    try:
        out = s.recv(65536)
    except socket.timeout:
        pass
    s.close()
    text = out.decode(errors="replace").replace("\r", "").strip()
    if text:
        print(text)


if __name__ == "__main__":
    main()
