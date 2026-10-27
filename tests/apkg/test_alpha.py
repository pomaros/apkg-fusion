# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
"""FUSION-ALPHA host scenario: one clean workspace, end to end.

Author an application and its dependency from TOML, build and sign them,
publish through a real channel served over HTTP, discover through the CLI and
the actual PkgManager worker, install by name from the root's source list,
keep a personal configuration through an upgrade, survive a tampered archive
and a crash, repair, roll back offline and remove. Host evidence only: no AROS
execution, power-loss durability or distribution qualification is implied."""
import ctypes as C
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from support import *
import test_client as clients
from test_author import author_fixture, key_fixture, build
from test_channel import CHANNEL, seed
from test_file_channel import channel_key, publish, snapshot_document
from test_http_channel import serve
from test_channel_clients import CONFIRM

lib.apkg_client_source_install.argtypes = [C.c_char_p, C.c_char_p, C.c_int, CONFIRM, C.c_void_p,
                                           C.c_void_p, C.POINTER(C.c_void_p), C.c_void_p]
lib.apkg_client_source_install.restype = C.c_int

CRASH = """
import ctypes as C, sys
from support import lib
from test_channel_clients import CONFIRM
lib.apkg_client_source_install.argtypes = [C.c_char_p, C.c_char_p, C.c_int, CONFIRM, C.c_void_p,
                                           C.c_void_p, C.POINTER(C.c_void_p), C.c_void_p]
lib.apkg_test_fault(4, 2)
out = C.c_void_p()
lib.apkg_client_source_install(sys.argv[1].encode(), b'app', 0, CONFIRM(lambda u, p, t: 1), None, None, C.byref(out), None)
sys.exit(98)
"""

def cli(*args):
    return subprocess.run([str(clients.CLI), *map(str, args)], capture_output=True, text=True, timeout=120)

def confirmed(*args):
    first = cli(*args)
    match = re.search(r'--confirm ([0-9a-f]{64})', first.stderr)
    return (first, None) if not match else (first, cli(*args, '--confirm', match.group(1)))

def body(root): return json.loads((root / '.apkg/state').read_bytes())['body']
def versions(root): return {r['descriptor']['body']['name']: r['descriptor']['body']['version'] for r in body(root)['installed']}

class Alpha(unittest.TestCase):
    def tearDown(self): lib.apkg_test_fault(0, 0)

    def release(self, name, version, program, depends=None):
        folder = self.d / (name + '-' + version); folder.mkdir()
        source = folder / name.capitalize()
        author_fixture(source, name=name, version=version, depends=depends or [])
        (source / 'program').write_bytes(program)
        archive = folder / 'package.pkg'
        result = build(source, archive, self.d / 'author-key'); self.assertEqual(result.returncode, 0, result.stderr)
        result = publish(archive, self.endpoint, self.d / 'channel-key'); self.assertEqual(result.returncode, 0, result.stderr)

    def archive_of(self, name, version):
        for item in snapshot_document(self.endpoint)['body']['entries']:
            b = item['descriptor']['body']
            if b['name'] == name and b['version'] == version:
                return self.endpoint / 'archives' / (item['archive']['sha256'] + '.pkg')
        raise AssertionError('not published')

    def test_clean_workspace_alpha_scenario(self):
        with tempfile.TemporaryDirectory() as d:
            self.d = d = Path(d); self.endpoint = d / 'channel'
            key_fixture(d / 'author-key'); channel_key(d / 'channel-key')
            root = d / 'root'; app = root / 'app' / 'App'
            self.release('provider', '1.2', b'provider')
            self.release('app', '1.2', b'app one', depends=['provider >= 1.1'])
            policy_file = d / 'policy.json'; policy_file.write_bytes(seed())
            result = cli('init', root, TARGET, policy_file, '--yes'); self.assertEqual(result.returncode, 0, result.stderr)
            with serve(self.endpoint) as (url, server):
                # Discovery through both client paths.
                updates = cli('channel-updates', root, url, CHANNEL); self.assertEqual(updates.returncode, 0, updates.stderr)
                self.assertIn('"app"', updates.stdout)
                worker = subprocess.run([str(clients.WORKER), str(root), url, '30', '0', CHANNEL], capture_output=True, text=True, timeout=60)
                self.assertEqual(worker.returncode, 0, worker.stdout + worker.stderr); self.assertIn('row app', worker.stdout)
                # The root's source list, then install by name with its dependency.
                preview, added = confirmed('source-add', root, 'main', url, CHANNEL)
                self.assertIn('Add source main', preview.stdout); self.assertEqual(added.returncode, 0, added.stderr)
                declined = cli('source-install', root, 'app'); self.assertNotEqual(declined.returncode, 0)
                self.assertEqual(versions(root), {})
                _, installed = confirmed('source-install', root, 'app'); self.assertEqual(installed.returncode, 0, installed.stdout + installed.stderr)
                self.assertEqual(versions(root), {'app': '1.2', 'provider': '1.2'})
                (app / '.hidden').write_bytes(b'personal setting')
                # Upgrade keeps the personal configuration.
                self.release('app', '2.0', b'app two', depends=['provider >= 1.1'])
                _, upgraded = confirmed('source-install', root, 'app'); self.assertEqual(upgraded.returncode, 0, upgraded.stdout + upgraded.stderr)
                self.assertEqual((app / 'program').read_bytes(), b'app two'); self.assertEqual((app / '.hidden').read_bytes(), b'personal setting')
                upgraded_state = (root / '.apkg/state').read_bytes()
                # A tampered archive refuses without change.
                self.release('app', '3.0', b'app three', depends=['provider >= 1.1'])
                archive = self.archive_of('app', '3.0'); original = archive.read_bytes()
                archive.write_bytes(original[:-1] + bytes([original[-1] ^ 1]))
                tampered = cli('source-install', root, 'app')  # refused while preparing, before any confirmation
                self.assertNotEqual(tampered.returncode, 0); self.assertNotIn('--confirm', tampered.stderr)
                self.assertEqual((root / '.apkg/state').read_bytes(), upgraded_state)
                archive.write_bytes(original)
                # A crash after the commit decision recovers to the whole new release.
                # A separate process, not fork(): the HTTP server thread runs here.
                crash = subprocess.run([sys.executable, '-c', CRASH, str(root)], cwd=Path(__file__).parent,
                                       capture_output=True, text=True, timeout=120)
                self.assertEqual(crash.returncode, 99, crash.stdout + crash.stderr)
                self.assertEqual(cli('source-install', root, 'app').returncode, 9)
                recovered = cli('recover', root); self.assertEqual(recovered.returncode, 0, recovered.stdout + recovered.stderr)
                self.assertEqual(versions(root)['app'], '3.0'); self.assertEqual((app / '.hidden').read_bytes(), b'personal setting')
            # Offline from here: the channel is gone.
            shutil.rmtree(self.endpoint)
            current = (root / '.apkg/state').read_bytes()
            history = next(p for p in root.parent.glob(root.name + '.apkg-history.*')
                           if (p / 'decision').exists() and (p / 'new-state').read_bytes() == current)
            _, rolled = confirmed('rollback', root, history); self.assertEqual(rolled.returncode, 0, rolled.stdout + rolled.stderr)
            self.assertEqual(versions(root)['app'], '2.0'); self.assertEqual((app / 'program').read_bytes(), b'app two')
            self.assertEqual((app / '.hidden').read_bytes(), b'personal setting')
            (app / 'program').write_bytes(b'damaged')
            _, repaired = confirmed('repair', root, 'app'); self.assertEqual(repaired.returncode, 0, repaired.stdout + repaired.stderr)
            self.assertEqual((app / 'program').read_bytes(), b'app two')
            self.assertEqual((app / '.hidden').read_bytes(), b'personal setting')
            _, removed = confirmed('remove', root, 'app'); self.assertEqual(removed.returncode, 0, removed.stdout + removed.stderr)
            self.assertNotIn('app', versions(root)); self.assertFalse((app / 'program').exists())
            self.assertEqual((app / '.hidden').read_bytes(), b'personal setting')
            self.assertIn('provider', versions(root))

if __name__ == '__main__':
    unittest.main()
