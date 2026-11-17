# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tomasz Staniak
"""apkg migrate register: the packages of a copied classic root become
unsigned entries of the common engine.

An unsigned entry has "signature": null, the reserved all-zero authority and
adoption evidence from arospkg-v1. It is recognised by that form alone; it is
never verified as a signature, never granted by a policy, never repaired and
never replaced by a signed package of the same name."""
import json
import os
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from support import ROOT
from test_author import author_fixture, key_fixture, build
from test_channel import seed, wire, grant, PK

CLI = Path(os.environ.get('APKG_CLI', ROOT / 'build/apkg'))
CAPTURED = ROOT / 'tests/fixtures/migration/captured-0.7.4-v1'
TARGET = 'x86_64-aros-v1-native'
ZERO = '0' * 32


def run(*args):
    return subprocess.run([str(CLI), *map(str, args)], capture_output=True, text=True)


def confirmed(*args):
    first = run(*args)
    match = re.search(r'--confirm ([0-9a-f]{64})', first.stderr)
    if not match:
        return first
    return run(*args, '--confirm', match.group(1))


class Registration(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.base = Path(self.tmp.name).resolve()
        self.source = self.base / 'original'
        shutil.copytree(CAPTURED / 'managed-upgraded', self.source)
        self.copy = self.base / 'copy'
        self.policy = self.base / 'policy.json'
        self.policy.write_bytes(seed())

    def tearDown(self):
        self.tmp.cleanup()

    def copied(self, initialize=True):
        result = run('migrate', 'copy', self.source, self.copy, '--quiescent')
        self.assertEqual(result.returncode, 0, result.stderr)
        if initialize:
            result = run('init', self.copy, TARGET, self.policy, '--yes')
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def registered(self):
        self.copied()
        result = confirmed('migrate', 'register', self.copy)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def state(self):
        return json.loads((self.copy / '.apkg/state').read_bytes())

    def edit(self, change, name='mini'):
        path = self.source / 'db/installed' / (name + '.json')
        data = json.loads(path.read_bytes())
        change(data)
        path.write_text(json.dumps(data))

    def test_preview_changes_nothing_and_names_the_missing_signature(self):
        self.copied()
        before = (self.copy / '.apkg/state').read_bytes()
        result = run('migrate', 'register', self.copy)
        self.assertEqual(result.returncode, 16, result.stderr)
        self.assertIn('Signature: none (migrated from the classic client, not authenticated)', result.stdout)
        self.assertIn('Publisher: none', result.stdout)
        self.assertNotIn('Signature: verified', result.stdout)
        self.assertEqual((self.copy / '.apkg/state').read_bytes(), before)

    def test_entries_have_the_unsigned_form(self):
        self.registered()
        rows = self.state()['body']['installed']
        self.assertEqual([r['descriptor']['body']['name'] for r in rows], ['mini', 'minilib'])
        for row in rows:
            self.assertIsNone(row['signature'])
            body = row['descriptor']['body']
            self.assertEqual(body['authority'], ZERO)
            self.assertEqual(body['target'], TARGET)
            self.assertTrue(all('blob' in f and 'data' not in f for f in body['files']))
            self.assertEqual(row['adoption']['evidence']['body']['source'], 'arospkg-v1')
        mini, lib = rows
        self.assertEqual(mini['descriptor']['body']['kind'], 'application')
        self.assertEqual(lib['descriptor']['body']['kind'], 'data')
        self.assertEqual(mini['descriptor']['body']['version'], '1.1')
        self.assertEqual(mini['descriptor']['body']['revision'], '3')
        self.assertEqual(mini['reason'], 'explicit')
        self.assertEqual(lib['reason'], 'dependency')
        self.assertEqual(mini['descriptor']['body']['relations']['depends'],
                         [dict(authority=ZERO, min_revision='0', min_version='1.0', name='minilib', target=TARGET)])
        # The record the classic client wrote is the evidence, byte for byte.
        record = (self.copy / '.apkg-legacy/db/installed/mini.json').read_bytes()
        self.assertEqual(bytes.fromhex(mini['adoption']['evidence']['body']['record']), record)
        # The file the user edited is theirs; the one the package never had is unowned.
        self.assertEqual(mini['adoption']['preserve'], ['Mini.cfg'])
        self.assertEqual(mini['adoption']['unowned'], ['notes.txt'])

    def test_registration_leaves_the_drawers_as_they_were(self):
        self.copied()
        before = {p.relative_to(self.copy): p.read_bytes()
                  for p in (self.copy / 'mini').rglob('*') if p.is_file()}
        self.registered_again = confirmed('migrate', 'register', self.copy)
        self.assertEqual(self.registered_again.returncode, 0, self.registered_again.stderr)
        after = {p.relative_to(self.copy): p.read_bytes()
                 for p in (self.copy / 'mini').rglob('*') if p.is_file()}
        self.assertEqual(before, after)

    def test_queries_say_none_not_verified(self):
        self.registered()
        self.assertEqual(run('info', self.copy, 'mini', 'signature').stdout.splitlines()[0], 'none')
        self.assertEqual(run('info', self.copy, 'mini', 'authortrust').stdout.splitlines()[0], 'unknown')
        listing = run('list', self.copy).stdout
        self.assertIn('Authority: none (migrated from the classic client)', listing)
        self.assertIn('no signature; not authenticated', listing)
        legacy = json.loads(run('legacy', '--root', self.copy, 'info', 'mini').stdout.splitlines()[0])
        self.assertEqual((legacy['signature'], legacy['authortrust'], legacy['kind']),
                         ('none', 'unknown', 'application'))

    def test_verify_compares_with_the_recorded_hashes(self):
        self.registered()
        result = run('verify', self.copy, 'mini')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('Signature: none (migrated from the classic client, not authenticated)', result.stdout)
        self.assertIn('ok Mini\n', result.stdout)
        self.assertIn('changed Mini.cfg', result.stdout)
        self.assertIn('unknown notes.txt', result.stdout)

    def test_repair_is_refused(self):
        self.registered()
        before = (self.copy / '.apkg/state').read_bytes()
        result = run('repair', self.copy, 'mini')
        self.assertEqual(result.returncode, 17, result.stdout + result.stderr)
        self.assertIn('cannot be repaired', result.stderr)
        self.assertEqual((self.copy / '.apkg/state').read_bytes(), before)

    def test_recorded_dependency_still_protects_removal(self):
        self.registered()
        self.assertEqual(run('remove', self.copy, 'minilib').returncode, 15)

    def test_removal_keeps_the_users_files(self):
        self.registered()
        result = confirmed('remove', self.copy, 'mini')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(sorted(p.name for p in (self.copy / 'mini').iterdir()), ['Mini.cfg', 'notes.txt'])
        self.assertEqual([r['descriptor']['body']['name'] for r in self.state()['body']['installed']], ['minilib'])
        result = confirmed('remove', self.copy, 'minilib')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def signed(self, name, version):
        work = self.base / ('signed-' + name)
        work.mkdir()
        drawer = work / 'Drawer'
        manifest = author_fixture(drawer, name=name, version=version, revision=1)
        manifest.write_text(manifest.read_text().replace('abi = "v11"', 'abi = "v1"'))
        (drawer / '.ameta').unlink()
        key_fixture(work / 'author-key')
        package = work / 'package.pkg'
        result = build(drawer, package, work / 'author-key')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return package

    def test_a_signed_package_of_the_same_name_does_not_replace_it(self):
        self.registered()
        package = self.signed('mini', '9.0')
        before = (self.copy / '.apkg/state').read_bytes()
        for command in ('upgrade', 'install'):
            result = confirmed(command, self.copy, package)
            self.assertIn(result.returncode, (12, 14), command + result.stdout + result.stderr)
            self.assertEqual((self.copy / '.apkg/state').read_bytes(), before)
        result = run('upgrade', self.copy, package)
        self.assertEqual(result.returncode, 12, result.stderr)
        self.assertIn('not authenticated', result.stderr)

    def test_no_policy_grants_the_reserved_authority(self):
        self.copied(initialize=False)
        self.policy.write_bytes(wire('policy', dict(authorities=[grant(ZERO, PK)])))
        result = run('init', self.copy, TARGET, self.policy, '--yes')
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((self.copy / '.apkg/state').exists())

    def test_a_signature_for_the_reserved_authority_is_refused(self):
        """A signed artifact cannot claim the reserved authority, whatever key signs it."""
        self.copied()
        work = self.base / 'claim'
        work.mkdir()
        drawer = work / 'Drawer'
        manifest = author_fixture(drawer, name='claim', version='1.0', revision=1)
        manifest.write_text(manifest.read_text().replace('abi = "v11"', 'abi = "v1"'))
        (drawer / '.ameta').unlink()
        key_fixture(work / 'author-key')
        result = build(drawer, work / 'package.pkg', work / 'author-key', '--authority', ZERO)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        result = confirmed('install', self.copy, work / 'package.pkg')
        self.assertEqual(result.returncode, 12, result.stdout + result.stderr)
        self.assertEqual(self.state()['body']['installed'], [])

    def tampered(self, old, new):
        path = self.copy / '.apkg/state'
        data = path.read_bytes()
        self.assertIn(old, data)
        path.write_bytes(data.replace(old, new))
        result = run('list', self.copy)
        return result

    def test_state_with_another_evidence_source_is_not_valid(self):
        self.registered()
        result = self.tampered(b'"source":"arospkg-v1"', b'"source":"manual"')
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn('invalid common root', result.stderr)

    def test_state_with_null_signature_and_a_real_authority_is_not_valid(self):
        self.registered()
        result = self.tampered(b'"authority":"' + ZERO.encode() + b'","files"',
                               b'"authority":"' + b'ab' * 16 + b'","files"')
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn('invalid common root', result.stderr)

    def test_adoption_set_refuses_an_unsigned_descriptor_in_another_form(self):
        self.copied()
        self.assertEqual(run('migrate', 'register', self.copy).returncode, 16)
        manifest = self.copy / '.apkg-legacy/adoption-set.json'
        data = manifest.read_bytes()
        bad = self.base / 'bad-set.json'
        bad.write_bytes(data.replace(b'"authority":"' + ZERO.encode() + b'","files"',
                                     b'"authority":"' + b'ab' * 16 + b'","files"'))
        result = run('adopt-set', self.copy, bad)
        self.assertEqual(result.returncode, 12, result.stdout + result.stderr)
        self.assertEqual(self.state()['body']['installed'], [])

    def test_needs_an_initialized_root_and_a_set_aside_registry(self):
        self.copied(initialize=False)
        result = run('migrate', 'register', self.copy)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('registration refused', result.stderr)
        self.assertIn('apkg init', result.stderr)
        fresh = self.base / 'fresh'
        self.assertEqual(run('init', fresh, TARGET, self.policy, '--yes').returncode, 0)
        result = run('migrate', 'register', fresh)
        self.assertEqual(result.returncode, 5, result.stderr)

    def test_second_registration_is_refused(self):
        self.registered()
        before = (self.copy / '.apkg/state').read_bytes()
        result = run('migrate', 'register', self.copy)
        self.assertEqual(result.returncode, 14, result.stdout + result.stderr)
        self.assertEqual((self.copy / '.apkg/state').read_bytes(), before)

    def test_a_record_for_another_system_is_refused(self):
        result = run('migrate', 'copy', self.source, self.copy, '--quiescent')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(run('init', self.copy, 'x86_64-aros-v11-native', self.policy, '--yes').returncode, 0)
        result = run('migrate', 'register', self.copy)
        self.assertEqual(result.returncode, 13, result.stderr)
        self.assertIn('another system', result.stderr)

    def test_launch_disagreement_is_reported_and_nothing_is_chosen(self):
        def change(record):
            record['launch']['mode'] = 'workbench'
        self.edit(change)
        self.copied()
        result = run('migrate', 'register', self.copy)
        self.assertEqual(result.returncode, 16, result.stderr)
        self.assertIn('launch data disagrees', result.stderr)
        self.assertIn('Neither is chosen', result.stderr)
        application = (self.copy / 'mini/application.json').read_bytes()
        self.assertEqual(application, (self.source / 'mini/application.json').read_bytes())

    def test_launch_data_outside_the_drawer_is_refused(self):
        def change(record):
            record['launch']['executable'] = '../other/Program'
        self.edit(change)
        self.copied()
        result = run('migrate', 'register', self.copy)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn('launch data', result.stderr)


if __name__ == '__main__':
    unittest.main()
