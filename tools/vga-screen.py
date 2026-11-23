#!/usr/bin/env python3
"""Render a raw VGA text-mode dump (0xB8000, 4000 bytes) as an 80x25 screen.

Usage: python3 tools/vga-screen.py <dump.bin>

Each cell is 2 bytes (character, attribute). Rows that are entirely blank are
skipped so the output matches what the user actually sees on screen.
"""
import sys

COLS = 80
ROWS = 25


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    with open(sys.argv[1], "rb") as fh:
        data = fh.read()

    if len(data) < COLS * ROWS * 2:
        sys.exit("dump is %d bytes, expected %d" % (len(data), COLS * ROWS * 2))

    print("===== VGA text screen (via gdb memory read) =====")
    for row in range(ROWS):
        cells = []
        for col in range(COLS):
            ch = data[(row * COLS + col) * 2]
            attr = data[(row * COLS + col) * 2 + 1]
            if ch < 0x20 or ch > 0x7e:
                ch = 0x20
            # attribute 0x00 means the BIOS never wrote here: treat as blank
            cells.append(chr(ch) if attr else " ")
        line = "".join(cells).rstrip()
        if line:
            print("row %2d| %s" % (row, line))
    print("=" * 49)


if __name__ == "__main__":
    main()
