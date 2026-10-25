#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tomasz Staniak <https://tomaszstaniak.com>
# Copyright (c) 2026 John Knipper
"""Build the LHA fixtures the host test uses.

The interesting ones cannot be made with the `lha` tool, because it would
never write them: a member called `ram:evil.txt` (absolute in the AROS sense,
and a real archive in AROS Archives does exactly this) and one that escapes
with `..`. So the level-1 headers are assembled here, byte by byte, with
stored (-lh0-) data. The compressed fixture is pinned from a real encoder;
its provenance and regeneration instructions are in fixtures/lha/README.md.
"""
import hashlib
from pathlib import Path
import shutil
import struct

def level1(name: bytes, data: bytes, method=b"-lh0-") -> bytes:
    # base header: everything from offset 2 to its end, then the 2-byte
    # "size of the next extended header" field, which is 0 when there is none.
    body = (method + struct.pack("<II", len(data), len(data)) +
            struct.pack("<I", 0) +            # timestamp
            bytes([0x20, 1, len(name)]) + name +
            struct.pack("<H", 0) +            # CRC, unchecked by our reader
            b"A" +                            # OS id
            struct.pack("<H", 0))             # no extended headers
    hsize = len(body)
    checksum = sum(body) & 0xFF
    return bytes([hsize, checksum]) + body + data

def write(path, *members):
    with open(path, "wb") as f:
        for m in members:
            name, data = m[0], m[1]
            f.write(level1(name, data, m[2]) if len(m) > 2 else level1(name, data))
        f.write(b"\0")                        # end of archive

write("/tmp/arospkg-lha-plain.lha",
      (b"Drawer\xffhello.txt", b"hello from lha\n"),
      (b"Drawer.info", b"I" * 64))
# an empty directory member (-lhd-), as jca02266's LHa writes for an empty
# drawer; OpenLoco's objects/ is one, and the game wants it to exist
write("/tmp/arospkg-lha-emptydir.lha",
      (b"Drawer\xffhello.txt", b"hello from lha\n"),
      (b"Drawer\xffobjects\xff", b"", b"-lhd-"))
write("/tmp/arospkg-lha-absolute.lha",
      (b"ram:evil.txt", b"should never be written\n"),
      (b"Drawer\xffok.txt", b"fine\n"))
write("/tmp/arospkg-lha-parent.lha",
      (b"..\xff..\xffevil.txt", b"should never be written\n"))
with open("/tmp/arospkg-lha-notanarchive", "wb") as f:
    f.write(b"this is not an archive at all, just some bytes\n")

# A pinned archive from an independent encoder makes the decompression test
# repeatable on hosts whose lha command only extracts. A missing or changed
# fixture fails setup; it must never leave a stale /tmp archive to be tested.
fixture = Path(__file__).resolve().parent / "fixtures/lha/packed-lh5.lha"
expected_sha256 = "eec84257c7650f431dfceab1b7753f8824b95759eed7fa62c1687e6651c029cc"
data = fixture.read_bytes()
if hashlib.sha256(data).hexdigest() != expected_sha256:
    raise SystemExit(f"{fixture}: compressed fixture SHA-256 mismatch")
shutil.copyfile(fixture, "/tmp/arospkg-lha-packed.lha")
# Each line is 51 bytes, including its newline: 306000 bytes in all.
expected = b"".join(
    f"line {i:05d} of a file that compresses, but not flat\n".encode("ascii")
    for i in range(6000)
)
Path("/tmp/arospkg-lha-packed.expected").write_bytes(expected)
