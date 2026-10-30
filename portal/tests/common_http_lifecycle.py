# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
"""Consume the real loopback portal with the common CLI and PkgManager worker.

The second invocation runs after Kestrel is stopped and checks removal and
the preserved user files independently. All keys and roots are synthetic.
"""
import json
from pathlib import Path
import re
import subprocess
import sys
import urllib.error
import urllib.request


def run(*args, expected=0):
    result = subprocess.run(list(map(str, args)), capture_output=True, text=True, timeout=60)
    assert result.returncode == expected, (result.returncode, result.stdout, result.stderr)
    return result.stdout


def approved(engine, *args):
    preview = run(engine, *args, expected=16)
    digest = re.search(r'Approved plan: ([0-9a-f]{64})', preview)
    assert digest, preview
    return run(engine, *args, '--confirm', digest[1])


def body(root):
    return json.loads((root / '.apkg/state').read_bytes())['body']


phase, engine, work = sys.argv[1:4]
engine, work = Path(engine), Path(work)
if phase == 'install':
    source, endpoint, snapshot, digest = sys.argv[4:]
    source = Path(source)
    channel = 'cd' * 16
    authority = '1' * 32
    public = lambda key: json.loads(run(engine, 'key-public', source / key))['public_key']
    # Key interpretation belongs to the common executable.
    author, channel_key = public('author-key1'), public('channel-key')
    assert re.fullmatch('[0-9a-f]{64}', author), author
    assert re.fullmatch('[0-9a-f]{64}', channel_key), channel_key
    grant = lambda identity, key: dict(id=identity, keys=[dict(algorithm='ed25519', public_key=key)])
    policy = dict(format='apkg-json-1', requires=[], schema='1', type='policy',
                  body=dict(authorities=[grant(authority, author)], channels=[grant(channel, channel_key)]))
    seed = work / 'policy.json'
    seed.write_text(json.dumps(policy, sort_keys=True, separators=(',', ':')))
    roots = [work / name for name in ('cli-root', 'worker-root')]
    for root in roots:
        run(engine, 'init', root, 'x86_64-aros-v11-native', seed, '--yes')
    untrusted = work / 'untrusted'
    empty = dict(policy, body=dict(authorities=[], channels=[grant(channel, channel_key)]))
    untrusted_seed = work / 'untrusted-policy.json'
    untrusted_seed.write_text(json.dumps(empty, sort_keys=True, separators=(',', ':')))
    run(engine, 'init', untrusted, 'x86_64-aros-v11-native', untrusted_seed, '--yes')
    before = (untrusted / '.apkg/state').read_bytes()
    run(engine, 'channel-install', untrusted, endpoint, channel, snapshot, digest, expected=12)
    assert (untrusted / '.apkg/state').read_bytes() == before
    assert not (untrusted / 'same-name').exists()
    approved(engine, 'channel-install', roots[0], endpoint, channel, snapshot, digest)
    worker = engine.parent / 'test_pm_common'
    assert worker.is_file(), 'build the actual worker adapter with make build/test_pm_common'
    output = run(worker, roots[1], endpoint, '1', '7', '/'.join((channel, snapshot, digest)))
    assert 'status=0 outcome=1; message ownership balanced' in output, output
    assert body(roots[0])['installed'] == body(roots[1])['installed']
    assert body(roots[0])['channel_heads'] == body(roots[1])['channel_heads']
    for root in roots:
        record = body(root)['installed'][0]['descriptor']['body']
        assert record['authority'] == authority
        assert record['revision'] == str(2**64 - 1)
        drawer = root / 'same-name/Drawer1'
        assert (drawer / '.hidden').read_bytes() == b'configuration'
        assert (drawer / 'Empty').is_dir()
        assert list((drawer / 'Empty').iterdir()) == []
        verified = run(engine, 'verify', root, 'same-name')
        assert 'changed 0\nmissing 0\nunknown 0' in verified, verified
        (drawer / '.hidden').write_bytes(b'personal configuration')
        (drawer / 'Personal.txt').write_bytes(b'personal file')
    (work / 'endpoint').write_text(endpoint)
    print('HTTP publication consumed by CLI and actual PkgManager worker; trust refusal and uint64 identity passed')
elif phase == 'remove':
    endpoint = (work / 'endpoint').read_text()
    try:
        urllib.request.urlopen(endpoint + '/current', timeout=2)
    except (urllib.error.URLError, TimeoutError):
        pass
    else:
        raise AssertionError('the portal must be stopped before offline removal')
    for root in (work / 'cli-root', work / 'worker-root'):
        approved(engine, 'remove', root, 'same-name')
        assert body(root)['installed'] == []
        package = root / 'same-name'
        remaining = {p.relative_to(package).as_posix(): p.read_bytes()
                     for p in package.rglob('*') if p.is_file()}
        assert remaining == {'Drawer1/.hidden': b'personal configuration',
                             'Drawer1/Personal.txt': b'personal file'}, remaining
        assert not Path(str(root) + '.apkg-transaction').exists()
        assert not list(root.parent.glob(root.name + '.apkg-prepare.*'))
        assert not (package / 'Drawer1/Empty').exists()
    print('Offline removal passed; exact preserved files, empty registry and no pending preparation/transaction')
else:
    raise AssertionError('unknown lifecycle phase')
