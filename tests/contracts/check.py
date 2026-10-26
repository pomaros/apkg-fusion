#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
"""Draft contract checks only; never links a fake into the product."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
FIXTURES = ROOT / 'tests/fixtures/contracts'


def canonical(value):
    def check(v, depth=1):
        if depth > 32:
            raise ValueError('depth')
        if v is None or isinstance(v, (str, bool)):
            if isinstance(v, str):
                v.encode('utf-8')  # no lone surrogates
        elif isinstance(v, list):
            for x in v:
                check(x, depth + 1)
        elif isinstance(v, dict):
            for key, x in v.items():
                if not isinstance(key, str):
                    raise ValueError('key')
                key.encode('utf-8')
                check(x, depth + 1)
        else:
            raise ValueError('numeric or unsupported value')
    check(value)
    return json.dumps(value, ensure_ascii=False, sort_keys=True,
                      separators=(',', ':'), allow_nan=False).encode('utf-8')


def read_canonical(data):
    def pairs(items):
        out = {}
        for k, v in items:
            if k in out:
                raise ValueError('duplicate')
            out[k] = v
        return out
    def no_number(_):
        raise ValueError('numeric token')
    obj = json.loads(data.decode('utf-8'), object_pairs_hook=pairs,
                     parse_int=no_number, parse_float=no_number,
                     parse_constant=no_number)
    if canonical(obj) != data:
        raise ValueError('noncanonical')
    return obj


def u64(text):
    if not isinstance(text, str) or not text or len(text) > 20:
        raise ValueError('uint64')
    if any(c not in '0123456789' for c in text) or len(text) > 1 and text[0] == '0':
        raise ValueError('uint64')
    value = int(text)
    if value > (1 << 64) - 1:
        raise ValueError('uint64 overflow')
    return value


def transcript(data, domain, public_key):
    if domain not in (1, 2, 3) or len(public_key) != 32:
        raise ValueError('signature framing')
    return (b'APKG-SIG\x00\x01' + bytes([domain]) + len(data).to_bytes(8, 'big')
            + hashlib.sha256(data).digest() + public_key)


def expect_rejected(fn, value):
    try:
        fn(value)
    except (ValueError, UnicodeError):
        return
    raise AssertionError(f'accepted invalid value: {value!r}')


def wire_checks():
    vector = json.loads((FIXTURES / 'wire-vectors.json').read_text())
    data = bytes.fromhex(vector['canonical_hex'])
    assert canonical(vector['object']) == data
    assert read_canonical(data) == vector['object']
    assert hashlib.sha256(data).hexdigest() == vector['sha256']
    key = bytes.fromhex(vector['public_key'])
    message = transcript(data, 1, key)
    assert len(message) == 83
    assert message.hex() == vector['transcript_hex']
    assert int.from_bytes(message[11:19], 'big') == len(data)
    assert message[19:51] == hashlib.sha256(data).digest()
    assert message[51:] == key
    assert len({transcript(data, n, key) for n in (1, 2, 3)}) == 3
    assert transcript(data + b' ', 1, key) != message
    assert transcript(data, 1, bytes(reversed(key))) != message
    bad = [b'{"a":"1","a":"2"}', b'{ "a":"1"}', b'{"z":null,"a":null}',
           b'{"a":1}', b'{"a":1.0}', b'{"a":NaN}', b'{"a":"\\u00e9"}',
           b'{"a":"\\ud800"}', b'{}\n', b'\xef\xbb\xbf{}', b'{"a":"\xff"}']
    for value in bad:
        expect_rejected(read_canonical, value)
    for value in ('', '01', '-1', '+1', '1.0', '1e2', '18446744073709551616', '١'):
        expect_rejected(u64, value)
    assert u64('0') == 0
    assert u64('18446744073709551615') == (1 << 64) - 1
    print('Wire framing: golden bytes/transcript, domain/key/content binding and 19 rejection cases passed; no signature verification')


def corpus_source():
    versions = json.loads((FIXTURES / 'versions.json').read_text())['cases']
    targets = json.loads((FIXTURES / 'targets.json').read_text())['cases']
    q = json.dumps
    lines = ['#include "upgrade.h"', '#include "pkg_target.h"', '#include <assert.h>',
             'int main(void) { char why[256]; struct pkg_target a,b;']
    for c in versions:
        a, b = c['from'], c['to']
        lines.append(f'assert(version_compare({q(a["version"])},{q(b["version"])}) == {c["inherited_compare"]});')
        lines.append(f'assert((upgrade_allowed({q(a["version"])},{a["revision"]},{q(b["version"])},{b["revision"]},why,sizeof why)==0) == {int(c["automatic_upgrade"])});')
    for c in targets:
        lines.append(f'assert((pkg_target_parse({q(c["left"])},&a,why,sizeof why)==0) == {int(c["left_valid"])});')
        lines.append(f'assert((pkg_target_parse({q(c["right"])},&b,why,sizeof why)==0) == {int(c["right_valid"])});')
        lines.append(f'assert(pkg_target_matches(&a,&b) == {int(c["matches"])});')
    lines += ['return 0;', '}']
    return '\n'.join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    wire_checks()
    cc = shlex.split(os.environ.get('CC', 'cc'))
    flags = ['-std=c99', '-Wall', '-Wextra', '-Werror', '-Idocs/fusion/api']
    if args.sanitize:
        flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    def run(command):
        subprocess.run(command, cwd=ROOT, check=True)
    with tempfile.TemporaryDirectory(prefix='apkg-contract-') as folder:
        temp = Path(folder)
        run(cc + flags + ['tests/contracts/consumer.c', 'tests/contracts/fake.c', '-o', str(temp / 'consumer')])
        run([str(temp / 'consumer')])
        run(cc + flags + ['-c', 'tests/contracts/plan_client.c', '-o', str(temp / 'plan_client.o')])
        (temp / 'corpus.c').write_text(corpus_source())
        run(cc + flags + ['-Iinclude', '-Icomponents/arospkg/src/libpkg', str(temp / 'corpus.c'),
                         'components/arospkg/src/libpkg/upgrade.c', 'src/pkg_target.c', '-o', str(temp / 'corpus')])
        run([str(temp / 'corpus')])
    print('API draft: consumer executed with test double; plan client compiled; 54 inherited corpus assertions passed')


if __name__ == '__main__':
    main()
