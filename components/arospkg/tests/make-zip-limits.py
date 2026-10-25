#!/usr/bin/env python3
"""Archives for test_zip_limits: a member bigger than 0.5.0's 64 MiB limit, and
archives whose directory lies (wrong CRC, a size smaller or larger than the data,
a stored member whose sizes differ). Written to the directory named on the
command line."""
import os, struct, sys, zipfile, zlib

out = sys.argv[1]
os.makedirs(out, exist_ok=True)
big = 80 * 1024 * 1024

def path(n): return os.path.join(out, n)

# A program-sized member: highly compressible, so the test archive stays small.
with zipfile.ZipFile(path("big.zip"), "w", zipfile.ZIP_DEFLATED) as z:
    z.writestr("App/App", (b"AROS" * 1024) * (big // 4096))
    z.writestr("App/README", b"read me\n")
    z.writestr("App.info", b"I" * 64)
with zipfile.ZipFile(path("over.zip"), "w", zipfile.ZIP_DEFLATED) as z:
    z.writestr("App/App", b"\0" * (129 * 1024 * 1024))
with zipfile.ZipFile(path("total.zip"), "w", zipfile.ZIP_DEFLATED) as z:
    for i in range(3):
        z.writestr("App/part%d" % i, b"\0" * (100 * 1024 * 1024))
with zipfile.ZipFile(path("stored.zip"), "w", zipfile.ZIP_STORED) as z:
    z.writestr("App/data", os.urandom(300000))

def patch_cd(src, dst, field_off, value):
    """Rewrite one 32-bit field of the first central directory entry (and the
    local header, so the archive still looks consistent to a reader)."""
    b = bytearray(open(src, "rb").read())
    cd = b.find(b"PK\x01\x02")
    struct.pack_into("<I", b, cd + field_off, value)
    lh = b.find(b"PK\x03\x04")
    local = {16: 14, 20: 18, 24: 22}[field_off]
    struct.pack_into("<I", b, lh + local, value)
    open(dst, "wb").write(bytes(b))

with zipfile.ZipFile(path("one.zip"), "w", zipfile.ZIP_DEFLATED) as z:
    z.writestr("App/App", b"hello " * 50000)
size = len(b"hello " * 50000)
crc = zlib.crc32(b"hello " * 50000)
patch_cd(path("one.zip"), path("badcrc.zip"), 16, crc ^ 1)
patch_cd(path("one.zip"), path("smaller.zip"), 24, size - 1000)   # expands to more than declared
patch_cd(path("one.zip"), path("larger.zip"), 24, size + 1000)    # expands to less than declared
st = zipfile.ZipFile(path("stored.zip")).infolist()[0]
patch_cd(path("stored.zip"), path("storedbad.zip"), 24, st.file_size - 10)
