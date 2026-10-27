# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
"""Released command spelling rehearsed on an imported copy, never its source."""
import json
import os
import re
import subprocess
import tempfile
import unittest
from pathlib import Path
from support import *
import test_client as clients
from test_channel import CHANNEL, seed
from test_file_channel import channel_key, publish
import test_migration as migration_tests
migration = migration_tests.migration
from test_payload import archive
from test_sources import confirmed


def legacy(root, *args, binary=None):
    if binary:
        command = [str(binary), '--root', str(root), *map(str, args)]
    else:
        command = [str(clients.CLI), 'legacy', '--root', str(root), *map(str, args)]
    return subprocess.run(command, capture_output=True, text=True, timeout=60)


def imported(d):
    helper = migration_tests.MigrationCopies('test_pinned_fixture_copy_commit_operations_and_offline_rollback')
    source, root, mapping, policy_file, artifact = helper.setup_fixture(d)
    policy_file.write_bytes(seed())
    before = migration.inventory(source)
    plan = helper.prepare(source, root, mapping, policy_file)
    result = helper.helper('commit', root, '--confirm', plan['approval'])
    assert result.returncode == 0, result.stdout + result.stderr
    return source, root, artifact, before


def offer(d, endpoint, artifact, version, name='map-demo'):
    body = json.loads((artifact / 'descriptor.json').read_bytes())['body']
    body.update(name=name, version=version)
    body['files'][0]['data'] = b'updated program\n'.hex()
    body['files'][0]['sha256'] = hashlib.sha256(b'updated program\n').hexdigest()
    body['files'][0]['size'] = str(len(b'updated program\n'))
    data = wire('descriptor', body)
    package = archive(d / ('package-' + name + '-' + version), data)
    key = d / 'channel-key'
    if not key.exists(): channel_key(key)
    result = publish(package, endpoint, key)
    assert result.returncode == 0, result.stdout + result.stderr


class LegacyCLI(unittest.TestCase):
    def test_imported_copy_runs_list_info_upgrade_verify_install_remove_and_preserves_original(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp); source, root, artifact, before = imported(d)
            result = legacy(root, 'list'); self.assertEqual((result.returncode, result.stdout), (0, 'map-demo\n'))
            result = legacy(root, 'list', '--json'); self.assertEqual(result.returncode, 0, result.stderr)
            rows = json.loads(result.stdout)
            self.assertEqual(rows['hidden'], 0); self.assertEqual(rows['rows'][0]['id'], 'map-demo')
            self.assertEqual(rows['rows'][0]['revision'], 2)
            self.assertEqual(rows['rows'][0]['signature_scope'], 'descriptor')
            result = legacy(root, 'info', 'map-demo'); info = json.loads(result.stdout)
            self.assertEqual(info['name'], 'map-demo'); self.assertEqual(info['version'], '1.0.0-rc1')
            endpoint = d / 'channel'; offer(d, endpoint, artifact, '2.0')
            result = confirmed('source-add', root, 'fixture', endpoint, CHANNEL)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            copied = (root / '.apkg/state').read_bytes()
            preview = legacy(root, 'upgrade', 'map-demo', '--dry-run', '--fetch', '--json')
            self.assertEqual(preview.returncode, 0, preview.stderr)
            self.assertEqual(json.loads(preview.stdout)['type'], 'plan')
            self.assertEqual((root / '.apkg/state').read_bytes(), copied)
            refused = legacy(root, 'upgrade', 'map-demo')
            self.assertEqual(refused.returncode, 5, refused.stdout + refused.stderr)
            digest = re.search(r'--confirm ([0-9a-f]{64})', refused.stderr).group(1)
            self.assertEqual((root / '.apkg/state').read_bytes(), copied)
            wrong = legacy(root, 'upgrade', 'map-demo', '--confirm', '0' * 64)
            self.assertEqual(wrong.returncode, 5)
            result = legacy(root, 'upgrade', 'map-demo', '--confirm', digest)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            current = json.loads(legacy(root, 'info', 'map-demo').stdout)
            self.assertEqual(current['version'], '2.0')
            unchanged = legacy(root, 'upgrade', 'map-demo', '--dry-run', '--fetch', '--json')
            self.assertEqual(unchanged.returncode, 0, unchanged.stderr)
            self.assertEqual(json.loads(unchanged.stdout), dict(compatibility='apkg-legacy-compat-1', outcome='unchanged'))
            self.assertEqual((root / 'map-demo/settings.cfg').read_bytes(), b'theme=user\n')
            self.assertEqual((root / 'map-demo/notes.txt').read_bytes(), b'personal\n')
            result = legacy(root, 'verify', 'map-demo')
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('changed settings.cfg [configuration]', result.stdout)
            offer(d, endpoint, artifact, '1.0', name='helper')
            result = legacy(root, 'install', 'helper', '--yes')
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(legacy(root, 'remove', 'helper', '--yes').returncode, 0)
            self.assertEqual(legacy(root, 'remove', 'map-demo', '--yes').returncode, 0)
            self.assertEqual(legacy(root, 'list').stdout, 'nothing installed\n')
            self.assertEqual(legacy(root, 'info', 'map-demo').returncode, 10)
            self.assertEqual(legacy(root, 'doctor', '--retry').returncode, 0)
            self.assertEqual(migration.inventory(source), before)
            self.assertEqual((root / '.apkg-legacy/db/installed/map-demo.json').read_bytes(), (source / 'db/installed/map-demo.json').read_bytes())

    def test_unknown_options_and_unmigrated_roots_refuse_before_writes(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp); source, root, artifact, before = imported(d)
            copied = migration.inventory(root)
            for words in (('install', 'new', '--index', 'unsigned.json', '--yes'),
                          ('install', 'new', '--dry-run'), ('install', 'new', '--yes', '--json'),
                          ('run', 'map-demo', '--help-app'), ('upgrade', '--all', '--yes'),
                          ('list', '--fetch'), ('remove', '../escape', '--yes'),
                          ('remove', 'map-demo', '--confirm'), ('remove', 'map-demo', '--yes', '--dry-run')):
                with self.subTest(words=words): self.assertEqual(legacy(root, *words).returncode, 5)
            self.assertEqual(migration.inventory(root), copied)
            self.assertEqual(legacy(source, 'remove', 'map-demo', '--yes').returncode, 10)
            self.assertEqual(migration.inventory(source), before)

    def test_install_exposes_the_native_compatibility_alias(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            result = subprocess.run(['make', 'install', 'DESTDIR=' + str(d), 'PREFIX=/opt/apkg'], cwd=ROOT, capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            alias = d / 'opt/apkg/bin/apkg-legacy'
            self.assertTrue(alias.is_symlink())
            result = subprocess.run([str(alias), '--version'], capture_output=True, text=True)
            self.assertEqual((result.returncode, result.stdout), (0, 'apkg-fusion legacy compatibility 1\n'))

    def test_explicit_legacy_binary_alias_matches_prefix_and_runtime_errors_are_ten(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp); source, root, artifact, before = imported(d)
            alias = d / 'apkg-legacy'; alias.symlink_to(clients.CLI.resolve())
            a = legacy(root, 'list', '--json'); b = legacy(root, 'list', '--json', binary=alias)
            self.assertEqual((a.returncode, a.stdout), (b.returncode, b.stdout))
            self.assertEqual(legacy(root, 'upgrade', 'absent', '--yes').returncode, 10)
            self.assertEqual(legacy(root, 'remove', 'absent', '--yes').returncode, 10)
            self.assertEqual(legacy(root, 'install', 'absent', '--yes').returncode, 10)
            self.assertEqual(migration.inventory(source), before)

if __name__ == '__main__': unittest.main()
