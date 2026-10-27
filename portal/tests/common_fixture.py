# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
"""Real common publisher fixture; no library mocks or copied crypto implementation."""
import json
from pathlib import Path
import subprocess
import sys

repository, directory, engine = map(Path, sys.argv[1:])

def run(*args):
    return subprocess.run(list(map(str, args)), check=True, capture_output=True, text=True).stdout

channel = directory / 'channels' / 'common'
channel_key = directory / 'channel-key'
run(engine, 'keygen', channel_key)
for index in (1, 2):
    source = directory / f'Drawer{index}'
    source.mkdir()
    (source / '.hidden').write_bytes(b'configuration')
    (source / 'Empty').mkdir()
    source.with_name(source.name + '.arospkg.toml').write_text(
        'id = "same-name"\nversion = "1.2"\nrevision = 3\nsummary = "Portal common test"\n'
        'category = "util"\nlicense = "MIT"\nkind = "data"\narch = "x86_64"\n'
        f'abi = "v11"\nsubdir = "{source.name}"\ndepends = []\ndepends_checked = true\n')
    key = directory / f'author-key{index}'
    run(engine, 'keygen', key)
    archive = directory / f'archive{index}.pkg'
    run(sys.executable, repository / 'tools/apkg-build', source, '--engine', engine,
        '--output', archive, '--sign', key, '--authority', str(index) * 32, '--runtime', 'native')
    run(sys.executable, repository / 'tools/apkg-channel', archive, '--engine', engine,
        '--directory', channel, '--id', 'cd' * 16, '--sign', channel_key)
