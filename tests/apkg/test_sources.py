# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
import json
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from support import *
import test_client as clients
from test_author import author_fixture, key_fixture, build
from test_channel import CHANNEL, CP, grant
from test_file_channel import channel_key, publish

OTHER = 'ef' * 16
AUTH2 = '12' * 16
PK2, SK2 = keypair(bytes([3]) * 32)

def policy_two(second_author=False):
    authorities = [grant(AUTH, PK)] + ([grant(AUTH2, PK2)] if second_author else [])
    return wire('policy', dict(authorities=sorted(authorities, key=lambda a: a['id']),
                               channels=[grant(CHANNEL, CP), grant(OTHER, CP)]))

def cli(*args):
    return subprocess.run([str(clients.CLI), *map(str, args)], capture_output=True, text=True, timeout=60)

def confirmed(*args):
    first = cli(*args)
    match = re.search(r'--confirm ([0-9a-f]{64})', first.stderr)
    if not match:
        return first
    return cli(*args, '--confirm', match.group(1))

def body(root): return json.loads((root / '.apkg/state').read_bytes())['body']

class Sources:
    def __init__(self, d, second_author=False):
        self.d = d; key_fixture(d / 'author-key'); channel_key(d / 'channel-key')
        (d / 'author2-key').write_text('Pkg-Secret-Key: 1\nSeed: ' + (bytes([3]) * 32).hex() + '\nPublic: ' + PK2.hex() + '\n')
        (d / 'author2-key').chmod(0o600)
        self.main = d / 'main'; self.extra = d / 'extra'; self.n = 0
        self.root = d / 'root'; state(self.root, policy=policy_two(second_author))
    def offer(self, where, name, version, data, depends=None, author=None):
        self.n += 1; folder = self.d / ('src%d' % self.n); folder.mkdir()
        source = folder / 'Drawer'; author_fixture(source, name=name, version=version, depends=depends or [])
        (source / 'program').write_bytes(data); archive = folder / 'p.pkg'
        extra = ('--authority', AUTH2) if author else ()
        result = build(source, archive, self.d / ('author2-key' if author else 'author-key'), *extra)
        assert result.returncode == 0, result.stderr
        result = publish(archive, where, self.d / 'channel-key', CHANNEL if where == self.main else OTHER)
        assert result.returncode == 0, result.stderr
    def add_both(self, test):
        for name, where, identity in (('main', self.main, CHANNEL), ('extra', self.extra, OTHER)):
            result = confirmed('source-add', self.root, name, where, identity)
            test.assertEqual(result.returncode, 0, result.stdout + result.stderr)

class RootSources(unittest.TestCase):
    def test_add_lists_in_order_and_refuses_untrusted_or_duplicate_sources(self):
        with tempfile.TemporaryDirectory() as d:
            s = Sources(Path(d)); s.offer(s.main, 'app', '1.0', b'a'); s.offer(s.extra, 'tool', '1.0', b't')
            before = (s.root / '.apkg/state').read_bytes()
            preview = cli('source-add', s.root, 'main', s.main, CHANNEL)
            self.assertNotEqual(preview.returncode, 0); self.assertIn('Add source main', preview.stdout)
            self.assertEqual((s.root / '.apkg/state').read_bytes(), before)
            s.add_both(self)
            listing = cli('source-list', s.root); self.assertEqual(listing.returncode, 0)
            self.assertEqual([line.split('\t')[1] for line in listing.stdout.splitlines() if '\t' in line], ['main', 'extra'])
            self.assertEqual(body(s.root)['installed'], [])
            # A correctly signed channel whose identity the policy does not grant.
            untrusted = s.d / 'untrusted'
            self.assertEqual(publish(s.d / 'src1' / 'p.pkg', untrusted, s.d / 'channel-key', '01' * 16).returncode, 0)
            refused = cli('source-add', s.root, 'third', untrusted, '01' * 16)
            self.assertEqual(refused.returncode, 12, refused.stdout + refused.stderr)
            self.assertNotEqual(cli('source-add', s.root, 'fourth', s.main, OTHER).returncode, 0)
            self.assertEqual(cli('source-add', s.root, 'main', s.extra, OTHER).returncode, 14)
            self.assertEqual(cli('source-add', s.root, 'bad/name', s.main, CHANNEL).returncode, 1)
            self.assertEqual(len(body(s.root)['sources']), 2)

    def test_newest_across_sources_wins_and_only_the_used_floor_advances(self):
        with tempfile.TemporaryDirectory() as d:
            s = Sources(Path(d)); s.offer(s.main, 'client', '1.2', b'old'); s.offer(s.extra, 'client', '2.0', b'new'); s.add_both(self)
            result = confirmed('source-install', s.root, 'client'); self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual((s.root / 'client/Drawer/program').read_bytes(), b'new')
            self.assertEqual([h['id'] for h in body(s.root)['channel_heads']], [OTHER])
            again = confirmed('source-install', s.root, 'client'); self.assertEqual(again.returncode, 0)
            self.assertIn('outcome=0', again.stdout)

    def test_pkgmanager_worker_installs_by_name_from_the_source_list(self):
        with tempfile.TemporaryDirectory() as d:
            s = Sources(Path(d)); s.offer(s.main, 'client', '1.2', b'old'); s.offer(s.extra, 'client', '2.0', b'new'); s.add_both(self)
            result = subprocess.run([str(clients.WORKER), str(s.root), 'client', '41'], capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('status 0 outcome 1 previews 1', result.stdout)
            self.assertEqual((s.root / 'client/Drawer/program').read_bytes(), b'new')
            s.offer(s.extra, 'client', '3.0', b'upgraded')
            result = subprocess.run([str(clients.WORKER), str(s.root), 'client', '41'], capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('status 0 outcome 1 previews 1', result.stdout)
            self.assertEqual((s.root / 'client/Drawer/program').read_bytes(), b'upgraded')

    def test_worker_source_controls_match_cli_and_preserve_state_on_refusal(self):
        def worker(s, name, mode, choice=1, *extra):
            result = subprocess.run([str(clients.WORKER), str(s.root), name,
                                     str(mode), str(choice), *map(str, extra)],
                                    capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            return result.stdout
        with tempfile.TemporaryDirectory() as d:
            s = Sources(Path(d)); s.offer(s.main, 'client', '1.0', b'one')
            before = (s.root / '.apkg/state').read_bytes()
            for choice, code in ((0, 16), (2, 16), (3, 4), (4, 16)):
                out = worker(s, 'main', 42, choice, s.main, CHANNEL)
                self.assertIn('status %d outcome 0' % code, out)
                self.assertEqual((s.root / '.apkg/state').read_bytes(), before)
            out = worker(s, 'main', 42, 1, s.main, CHANNEL)
            self.assertIn('status 0 outcome 1 previews 1', out)
            current = (s.root / '.apkg/state').read_bytes()
            out = worker(s, '', 44)
            self.assertEqual(out.split('\n', 1)[1], cli('source-list', s.root).stdout.removesuffix('status=0 outcome=0\n'))
            self.assertEqual((s.root / '.apkg/state').read_bytes(), current)
            self.assertIn('status 14', worker(s, 'main', 42, 1, s.main, CHANNEL))
            self.assertIn('status 12', worker(s, 'untrusted', 42, 1, s.main, '01' * 16))
            self.assertEqual((s.root / '.apkg/state').read_bytes(), current)
            shutil.rmtree(s.main)  # Listing/removal need no available channel.
            for choice, code in ((0, 16), (2, 16), (3, 4), (4, 16)):
                self.assertIn('status %d' % code, worker(s, 'main', 43, choice))
                self.assertEqual((s.root / '.apkg/state').read_bytes(), current)
            self.assertIn('status 4', worker(s, '', 44, 3))
            self.assertIn('status 16', worker(s, '', 44, 4))
            self.assertIn('status 0 outcome 1 previews 1', worker(s, 'main', 43))
            self.assertNotIn('sources', body(s.root))
            self.assertIn('status 5', worker(s, 'main', 43))

    def test_an_equal_version_comes_from_the_earlier_source(self):
        with tempfile.TemporaryDirectory() as d:
            s = Sources(Path(d)); s.offer(s.main, 'client', '1.2', b'from main'); s.offer(s.extra, 'client', '1.2', b'from extra'); s.add_both(self)
            result = confirmed('source-install', s.root, 'client'); self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual((s.root / 'client/Drawer/program').read_bytes(), b'from main')

    def test_dependencies_resolve_across_sources_and_record_both_floors(self):
        with tempfile.TemporaryDirectory() as d:
            s = Sources(Path(d)); s.offer(s.main, 'app', '1.0', b'app', depends=['provider >= 1.1'])
            s.offer(s.extra, 'provider', '1.2', b'provider'); s.add_both(self)
            result = confirmed('source-install', s.root, 'app'); self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(sorted(r['descriptor']['body']['name'] for r in body(s.root)['installed']), ['app', 'provider'])
            self.assertEqual(sorted(h['id'] for h in body(s.root)['channel_heads']), sorted([CHANNEL, OTHER]))

    def test_unreadable_source_two_authors_and_unknown_name_refuse_without_change(self):
        with tempfile.TemporaryDirectory() as d:
            s = Sources(Path(d), second_author=True); s.offer(s.main, 'client', '1.0', b'one')
            s.offer(s.extra, 'client', '1.1', b'other author', author=True); s.add_both(self)
            before = (s.root / '.apkg/state').read_bytes()
            self.assertEqual(cli('source-install', s.root, 'client').returncode, 14)
            self.assertEqual(cli('source-install', s.root, 'missing').returncode, 5)
            shutil.rmtree(s.extra)
            self.assertNotEqual(cli('source-install', s.root, 'client').returncode, 0)
            self.assertEqual((s.root / '.apkg/state').read_bytes(), before)

    def test_remove_keeps_installed_packages_and_list_changes_are_not_rolled_back(self):
        with tempfile.TemporaryDirectory() as d:
            s = Sources(Path(d)); s.offer(s.main, 'client', '1.0', b'one'); s.offer(s.extra, 'tool', '1.0', b't'); s.add_both(self)
            self.assertEqual(confirmed('source-install', s.root, 'client').returncode, 0)
            result = confirmed('source-remove', s.root, 'main'); self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual([x['name'] for x in body(s.root)['sources']], ['extra'])
            self.assertTrue((s.root / 'client/Drawer/program').exists())
            history = next(p for p in s.root.parent.glob(s.root.name + '.apkg-history.*')
                           if (p / 'new-state').exists() and (p / 'new-state').read_bytes() == (s.root / '.apkg/state').read_bytes())
            self.assertEqual(cli('rollback', s.root, history).returncode, 17)
            self.assertEqual(cli('source-remove', s.root, 'main').returncode, 5)
            self.assertEqual(confirmed('source-remove', s.root, 'extra').returncode, 0)
            self.assertNotIn('sources', body(s.root))

if __name__ == '__main__':
    unittest.main()
