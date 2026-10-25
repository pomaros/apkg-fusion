#!/usr/bin/env python3
"""Build the LHA fixtures the host test uses.

The interesting ones cannot be made with the `lha` tool, because it would
never write them: a member called `ram:evil.txt` (absolute in the AROS sense,
and a real archive in AROS Archives does exactly this) and one that escapes
with `..`. So the level-1 headers are assembled here, byte by byte, with
stored (-lh0-) data. The compressed fixture does come from the system tool,
because the point of that one is to exercise a real encoder.
"""
import os, shutil, struct, subprocess, sys, tempfile

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

# A compressed archive from a real encoder: exactly 300 000 bytes of text,
# which does not fit in one Huffman block, so the decoder has to cross a block
# boundary -- the case that broke the first version of it.
#
# Homebrew's `lha` only extracts. aros-python3 built one that packs; try
# whatever is around and skip the fixture if nothing can create an archive.
tool = None
for cand in (shutil.which("lha"),
             os.path.expanduser("~/Work/AROS-dev/aros-python3/tools/lha")):
    if cand and os.path.exists(cand):
        probe = tempfile.mkdtemp()
        open(os.path.join(probe, "t"), "w").write("t")
        r = subprocess.run([cand, "aq", os.path.join(probe, "p.lha"), "t"],
                           cwd=probe, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if r.returncode == 0 and os.path.exists(os.path.join(probe, "p.lha")):
            tool = cand
        shutil.rmtree(probe)
        if tool:
            break
if not tool:
    print("no lha that can CREATE an archive on this host: -lh5- fixture skipped",
          file=sys.stderr)
    sys.exit(0)
tmp = tempfile.mkdtemp()
with open(os.path.join(tmp, "big.txt"), "w") as f:
    for i in range(6000):                      # 6000 x 50 bytes = 300000 exactly
        f.write(("line %05d of a file that compresses, but not flat" % i).ljust(49) + "\n")
out = "/tmp/arospkg-lha-packed.lha"
if os.path.exists(out):
    os.remove(out)
subprocess.run([tool, "aq", out, "big.txt"], cwd=tmp, check=True,
               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
shutil.rmtree(tmp)
