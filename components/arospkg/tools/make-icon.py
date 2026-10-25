#!/usr/bin/env python3
"""Write PkgManager.info, a classic two-plane Workbench tool icon.

No icon existed for PkgManager; this draws a parcel so the Pi archive can be
started from Wanderer. Classic planar format because every AROS icon.library
reads it.  usage: make-icon.py OUT.info
"""
import struct, sys

W, H, DEPTH = 40, 32, 2
# pens: 0 background, 1 black, 2 white, 3 blue (Workbench defaults)
art = [[0] * W for _ in range(H)]
def rect(x0, y0, x1, y1, pen, fill=True):
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            if fill or y in (y0, y1) or x in (x0, x1):
                art[y][x] = pen
rect(6, 10, 33, 29, 3)            # box front
rect(6, 10, 33, 29, 1, fill=False)
for x in range(6, 34):            # lid
    art[4][x + 0] = 1 if x in (6, 33) else art[4][x]
rect(10, 4, 37, 9, 2)             # lid top
rect(10, 4, 37, 9, 1, fill=False)
for i in range(6):                # lid slant edges
    art[4 + i][10 - i if 10 - i >= 0 else 0] = 1
rect(18, 4, 21, 29, 2)            # tape
rect(18, 4, 21, 29, 1, fill=False)

def planes():
    out = b""
    words = (W + 15) // 16
    for p in range(DEPTH):
        for y in range(H):
            row = 0
            for x in range(words * 16):
                bit = (art[y][x] >> p) & 1 if x < W else 0
                row = (row << 1) | bit
            out += row.to_bytes(words * 2, "big")
    return out

def s(text):
    b = text.encode() + b"\0"
    return struct.pack(">L", len(b)) + b

gadget = struct.pack(">LhhhhHHHLLLLLHL", 0, 0, 0, W, H + 1, 4, 3, 1,
                     1, 0, 0, 0, 0, 0, 1)
tooltypes = ["DONOTWAIT"]
disk = struct.pack(">HH", 0xE310, 1) + gadget + struct.pack(
    ">BBLLllLLl", 3, 0, 0, 1, -0x80000000, -0x80000000, 0, 0, 65536)
image = struct.pack(">hhhhhLBBL", 0, 0, W, H, DEPTH, 1, 3, 0, 0)
data = disk + image + planes()
data += struct.pack(">L", (len(tooltypes) + 1) * 4) + b"".join(s(t) for t in tooltypes)
open(sys.argv[1], "wb").write(data)
