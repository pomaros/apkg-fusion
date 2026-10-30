# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
"""Real common publisher fixture; no library mocks or copied crypto implementation."""
import hashlib
import json
import struct
from pathlib import Path
import subprocess
import sys

repository, directory, engine = map(Path, sys.argv[1:4])
rich = '--rich' in sys.argv[4:]

def run(*args):
    result = subprocess.run(list(map(str, args)), capture_output=True, text=True)
    if result.returncode:
        raise SystemExit(result.stderr or result.stdout)
    return result.stdout

channel = directory / 'channels' / 'common'
channel_key = directory / 'channel-key'
run(engine, 'keygen', channel_key)
for index in (1, 2):
    source = directory / f'Drawer{index}'
    source.mkdir()
    (source / '.hidden').write_bytes(b'configuration')
    (source / 'Empty').mkdir()
    manifest = (
        'id = "same-name"\nversion = "1.2"\nrevision = 3\nsummary = "Portal common test"\n'
        'category = "util"\nlicense = "MIT"\nkind = "data"\narch = "x86_64"\n'
        f'abi = "v11"\nsubdir = "{source.name}"\ndepends = []\ndepends_checked = true\n')
    if rich:
        manifest = manifest.replace('Portal common test', 'Portal <script>alert(1)</script> & test')
        manifest += 'config = [".hidden"]\n'
    source.with_name(source.name + '.arospkg.toml').write_text(manifest)
    key = directory / f'author-key{index}'
    run(engine, 'keygen', key)
    archive = directory / f'archive{index}.pkg'
    run(sys.executable, repository / 'tools/apkg-build', source, '--engine', engine,
        '--output', archive, '--sign', key, '--authority', str(index) * 32, '--runtime', 'native')
    observations = []
    if rich:
        # The inherited TOML bridge limits revisions to 31 bits. Exercise the
        # common contract's full uint64 range with an independently rewritten
        # fixture envelope, signed by the real engine and checked at commit.
        raw = archive.read_bytes()
        position = 8
        metadata = []
        for _ in range(2):
            length = int.from_bytes(raw[position:position + 4], 'big')
            position += 4
            name = raw[position:position + length].decode()
            position += length + 1
            length = int.from_bytes(raw[position:position + 4], 'big')
            position += 4
            metadata.append((name, json.loads(raw[position:position + length])))
            position += length
        descriptor = metadata[0][1]
        descriptor['body']['revision'] = str(2**64 - 1)
        encode = lambda value: json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(',', ':')).encode()
        description = directory / f'descriptor{index}.json'
        description.write_bytes(encode(descriptor))
        metadata[1] = ('signature.json', json.loads(run(engine, 'sign-object', key, 'descriptor', description)))
        members = b''.join(struct.pack('>I', len(name)) + name.encode() + b'\0' +
                           struct.pack('>I', len(encode(value))) + encode(value) for name, value in metadata)
        tail = members + raw[position:]
        archive.write_bytes(b'PKG\1' + struct.pack('>I', len(tail) + 8) + tail)
        # An author's signed self-report is not independent review authority.
        body = descriptor['body']
        claim = dict(format='apkg-json-1', requires=[], schema='1', type='attestation', body=dict(
            **{field: body[field] for field in ('authority', 'name', 'version', 'revision', 'target')},
            descriptor_sha256=hashlib.sha256(encode(descriptor)).hexdigest(), issuer=body['authority'],
            kind='review', result='passed', sequence='1', issued_at='1', supersedes=None,
            environment=None, summary='Author self-report', evidence='https://example.invalid/self-report'))
        folder = directory / f'claim{index}'
        folder.mkdir()
        (folder / 'attestation.json').write_bytes(encode(claim))
        (folder / 'attestation.signature.json').write_text(run(engine, 'sign-object', key,
            'attestation', folder / 'attestation.json').strip())
        observations = ['--attestation', folder]

    run(sys.executable, repository / 'tools/apkg-channel', archive, '--engine', engine,
        '--directory', channel, '--id', 'cd' * 16, '--sign', channel_key, *observations)
