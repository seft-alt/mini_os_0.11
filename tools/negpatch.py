#!/usr/bin/env python3
"""Exact single-occurrence text patch, used by tools/neg_*.sh (mutation tests).

Usage:
    python3 tools/negpatch.py <file> <old> <new>

Exit code:
    0  replaced exactly one occurrence
    2  old text not found          (source drifted: the mutation is stale)
    3  old text appears >1 times   (ambiguous: pick a longer anchor)
    1  bad usage / io error

Why in the repo: the neg scripts used to call /tmp/negpatch.py, a file that only
existed on one machine's /tmp. Any reboot (or moving to another VM) turned all
19 mutation tests into "PATCH-FAILED" without saying so. Keeping it in tools/
makes the negative group reproducible.

Line endings: the source tree mixes LF and (rarely) CRLF, while the `old` strings
written inside bash scripts are LF. So each pattern is tried both as-is and with
its newline style flipped before giving up.
"""
import sys


def patch_bytes(data, old, new):
    """Return (new_data, status) with status in {'ok','miss','ambiguous'}."""
    variants = []
    variants.append((old, new))
    crlf_old = old.replace("\r\n", "\n").replace("\n", "\r\n")
    crlf_new = new.replace("\r\n", "\n").replace("\n", "\r\n")
    if crlf_old != old:
        variants.append((crlf_old, crlf_new))

    for o, n in variants:
        ob = o.encode("utf-8")
        cnt = data.count(ob)
        if cnt == 1:
            return data.replace(ob, n.encode("utf-8")), "ok"
        if cnt > 1:
            return data, "ambiguous"
    return data, "miss"


def main():
    if len(sys.argv) != 4:
        sys.stderr.write(__doc__)
        return 1
    path, old, new = sys.argv[1], sys.argv[2], sys.argv[3]

    try:
        with open(path, "rb") as fh:
            data = fh.read()
    except OSError as exc:
        sys.stderr.write("cannot read %s: %s\n" % (path, exc))
        return 1

    out, status = patch_bytes(data, old, new)
    if status == "ok":
        with open(path, "wb") as fh:
            fh.write(out)
        return 0
    if status == "ambiguous":
        sys.stderr.write("AMBIGUOUS: more than one match in %s\n" % path)
        return 3
    sys.stderr.write("NOT-FOUND: anchor not in %s (source drifted?)\n" % path)
    return 2


if __name__ == "__main__":
    sys.exit(main())
