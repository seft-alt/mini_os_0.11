#!/usr/bin/env python3
"""Minimal non-interactive SSH helper (paramiko) for the mini-os dev VM.

Usage:
    set SSHX_HOST / SSHX_PORT / SSHX_USER / SSHX_PASS
    python sshx.py run "<shell command>"
    python sshx.py put <local> <remote>
    python sshx.py get <remote> <local>
    python sshx.py mkdir <remote dir>

Keep this file pure ASCII.
"""
import os
import sys

import paramiko


def connect():
    host = os.environ.get("SSHX_HOST", "192.168.203.100")
    port = int(os.environ.get("SSHX_PORT", "22"))
    user = os.environ.get("SSHX_USER", "seft")
    password = os.environ.get("SSHX_PASS")
    if not password:
        sys.exit("SSHX_PASS is required")
    cli = paramiko.SSHClient()
    cli.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    cli.connect(host, port=port, username=user, password=password,
                timeout=10, banner_timeout=10, auth_timeout=10,
                look_for_keys=False, allow_agent=False)
    return cli


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    cmd = sys.argv[1]
    cli = connect()
    try:
        if cmd == "run":
            stdin, out, err = cli.exec_command(sys.argv[2], timeout=None, get_pty=False)
            text = out.read().decode("utf-8", "replace")
            errtext = err.read().decode("utf-8", "replace")
            code = out.channel.recv_exit_status()
            sys.stdout.write(text)
            if errtext:
                sys.stdout.write("--- stderr ---\n" + errtext)
            sys.exit(code)
        elif cmd == "put":
            sftp = cli.open_sftp()
            sftp.put(sys.argv[2], sys.argv[3])
            sftp.close()
        elif cmd == "get":
            sftp = cli.open_sftp()
            sftp.get(sys.argv[2], sys.argv[3])
            sftp.close()
        elif cmd == "mkdir":
            sftp = cli.open_sftp()
            try:
                sftp.stat(sys.argv[2])
            except IOError:
                sftp.mkdir(sys.argv[2])
            sftp.close()
        else:
            sys.exit(__doc__)
    finally:
        cli.close()


if __name__ == "__main__":
    main()
