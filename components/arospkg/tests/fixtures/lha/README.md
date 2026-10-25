<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# Compressed LHA test fixture

`packed-lh5.lha` contains `big.txt`, 306000 bytes of generated ASCII text.
The original test input and these fixture changes are distributed under the
repository's MIT licence. The archive contains no encoder code.

Encoder: [jca02266/lha](https://github.com/jca02266/lha/tree/16619b066b189ef289bb8b07b37d1c38d550da99),
commit `16619b066b189ef289bb8b07b37d1c38d550da99`.
Method: `-lh5-`; generic level-0 header; timestamp 1980-01-01 UTC.
Generic headers omit the generating machine's user and group identifiers.

Archive SHA-256:
`eec84257c7650f431dfceab1b7753f8824b95759eed7fa62c1687e6651c029cc`.

`tests/make-lha-fixtures.py` verifies that digest before copying the archive
into the test workspace and generates the expected output independently.
`tests/test_lha.c` compares every extracted byte. Routine tests require no
LHA encoder, so an extract-only `lha` command is sufficient.

To regenerate, build the pinned encoder with `autoreconf -fi`, `./configure`
and `make`. In an empty temporary directory, generate the input:

```python
from pathlib import Path
import os

p = Path("big.txt")
p.write_bytes(b"".join(
    f"line {i:05d} of a file that compresses, but not flat\n".encode("ascii")
    for i in range(6000)
))
p.chmod(0o644)
os.utime(p, (315532800, 315532800))
```

Run the built encoder as `TZ=UTC /path/to/lha aqo5g0 packed-lh5.lha big.txt`.
Check the resulting digest and compare `lha pq packed-lh5.lha` byte for byte
with `big.txt` before replacing the fixture.
