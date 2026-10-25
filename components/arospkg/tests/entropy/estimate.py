#!/usr/bin/env python3
"""Min-entropy of raw jitter samples from `probe RAW`.

Most-common-value estimate (NIST SP 800-90B, 6.3.1): with p the frequency of
the most common value and an upper 99% confidence bound on it, the
min-entropy per sample is -log2(p_upper). Reported for the whole delta and
for its low 8 and 4 bits. It is a single simple estimator, used here to
check the source's credit (1 bit per 16 samples), not to certify it.

    estimate.py raw.bin
"""
import math, struct, sys
from collections import Counter

data = open(sys.argv[1], "rb").read()
v = struct.unpack("<%dQ" % (len(data) // 8), data)
n = len(v)

def mcv(xs):
    c = Counter(xs)
    p = c.most_common(1)[0][1] / n
    pu = min(1.0, p + 2.576 * math.sqrt(p * (1 - p) / (n - 1)))
    return -math.log2(pu), len(c)

print(f"samples {n}; delta min {min(v)} median {sorted(v)[n//2]} max {max(v)}")
for name, xs in (("full", v), ("low 8 bits", [x & 255 for x in v]), ("low 4 bits", [x & 15 for x in v])):
    h, k = mcv(xs)
    print(f"{name:11s} min-entropy {h:6.3f} bits/sample over {k} distinct values")
h, _ = mcv(v)
print(f"credited: {1/16:.4f} bits/sample; measured/credited = {h/(1/16):.0f}x")
