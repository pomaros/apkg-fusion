# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
"""The legacy-copy rehearsal over roots written by released arospkg 0.7.4.

tests/fixtures/migration/captured-0.7.4-v1 holds two package roots that the
released x86_64 ABIv1 apkg 0.7.4 wrote on mainline AROS (see the README
there): one installed, edited and upgraded, one with an install stopped
after its drawer was published. The signed artifacts are built here from the
archives the root itself cached, so their files are the delivered bytes."""
import io
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile
from support import *
from test_client import CLI
from test_operations import payload

import importlib.util
spec = importlib.util.spec_from_file_location('apkg_migration', ROOT / 'tools/apkg_migrate.py')
migration = importlib.util.module_from_spec(spec); spec.loader.exec_module(migration)

CAPTURED = ROOT / 'tests/fixtures/migration/captured-0.7.4-v1'
V1 = 'x86_64-aros-v1-native'


def canonical(value): return json.dumps(value, sort_keys=True, separators=(',', ':')).encode()


def delivered(archive, subdir):
    with zipfile.ZipFile(io.BytesIO(archive.read_bytes())) as z:
        return {n[len(subdir) + 1:]: z.read(n) for n in z.namelist()
                if n.startswith(subdir + '/') and not n.endswith('/')}


class CapturedRoots(unittest.TestCase):
    def artifacts(self, base, source):
        rows = []
        for name in ('mini', 'minilib'):
            record = json.loads((source / 'db/installed' / (name + '.json')).read_bytes())
            files = delivered(source / 'cache' / (name + '.zip'), record['subdir'])
            self.assertEqual(sorted(files), sorted(r['path'] for r in record['contents']))
            data = json.loads(descriptor(name=name, target=V1,
                                         files=[payload(p, files[p].decode()) for p in sorted(files)]))
            data['body'].update(version=record['version'], revision=str(record.get('revision', 0)))
            if name == 'mini':
                data['body']['relations'] = dict(conflicts=[], recommends=[], replaces=[], depends=[
                    dict(authority=AUTH, min_revision='0', min_version='1.0', name='minilib', target=V1)])
                data['body']['requires_system'] = [dict(id='muimaster.library', min_version='0', type='library')]
            packed = canonical(data)
            folder = base / ('artifact-' + name); folder.mkdir()
            (folder / 'descriptor.json').write_bytes(packed); (folder / 'signature.json').write_bytes(sign(packed))
            rows.append(dict(name=name, artifact=str(folder)))
        return rows

    def prepare(self, d, case, legacy_root, icons=None, env=None, change=None):
        base = Path(d); source = base / 'original'
        shutil.copytree(globals()['CAPTURED'] / case, source)
        if change:
            change(source)
        mapping = base / 'mapping.json'
        packages = self.artifacts(base, source) if (source / 'db/installed/mini.json').exists() else []
        rows = dict(format='apkg-legacy-import-1', legacy_root=legacy_root, packages=packages)
        if icons:
            rows['icons'] = icons
        mapping.write_bytes(canonical(rows))
        seed = base / 'policy.json'; seed.write_bytes(policy())
        before = migration.inventory(source)
        extra = ['--legacy-env', str(env)] if env else []
        result = subprocess.run([sys.executable, str(ROOT / 'tools/apkg_migrate.py'), '--apkg', str(CLI), 'prepare',
                                 str(source), str(base / 'copy'), str(mapping), str(seed), V1, '--quiescent', *extra],
                                capture_output=True, text=True)
        return result, source, before, base / 'copy'

    def commit(self, copy_root, approval):
        return subprocess.run([sys.executable, str(ROOT / 'tools/apkg_migrate.py'), '--apkg', str(CLI), 'commit',
                               str(copy_root), '--confirm', approval], capture_output=True, text=True)

    def test_installed_drawer_icon_is_refused_with_the_original_kept(self):
        # Every arospkg package with a drawer icon records it as an installed
        # icon at <root>/<id>.info. The current profile refuses that placement.
        with tempfile.TemporaryDirectory() as d:
            result, source, before, copy_root = self.prepare(d, 'managed-upgraded', 'SYS:MigA')
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('owned root icon placement is not mapped', result.stdout + result.stderr)
            self.assertEqual(migration.inventory(source), before)
            self.assertFalse(copy_root.exists())

    def test_without_the_icon_the_edited_upgraded_root_imports(self):
        # The same root with only the icon ownership taken out of the copy of
        # the record: everything else the released client wrote is accepted.
        with tempfile.TemporaryDirectory() as d:
            work = Path(d) / 'captured'; shutil.copytree(CAPTURED, work)
            record = work / 'managed-upgraded/db/installed/mini.json'
            data = json.loads(record.read_bytes()); data['icon'] = dict(state='none', sha256='', size=0, member='')
            record.write_bytes(json.dumps(data, indent=2).encode())
            saved = globals()['CAPTURED']; globals()['CAPTURED'] = work
            try:
                result, source, before, copy_root = self.prepare(d, 'managed-upgraded', 'SYS:MigA')
            finally:
                globals()['CAPTURED'] = saved
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            receipt = json.loads(result.stdout)
            mini = next(o for o in receipt['observations'] if o['name'] == 'mini')
            self.assertEqual(mini['history']['archive']['bytes'], 'retained-unverified')
            done = subprocess.run([sys.executable, str(ROOT / 'tools/apkg_migrate.py'), '--apkg', str(CLI), 'commit',
                                   str(copy_root), '--confirm', receipt['approval']], capture_output=True, text=True)
            self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
            installed = {e['descriptor']['body']['name']: e for e in json.loads((copy_root / '.apkg/state').read_bytes())['body']['installed']}
            self.assertEqual(installed['mini']['reason'], 'explicit')
            self.assertEqual(installed['minilib']['reason'], 'dependency')
            for kept in ('mini/Mini.cfg', 'mini/notes.txt', 'mini.info'):
                self.assertEqual((copy_root / kept).read_bytes(), (source / kept).read_bytes(), kept)
            self.assertEqual((source / 'mini/Mini.cfg').read_bytes(), b'user=edited by the user\n')
            self.assertEqual(migration.inventory(source), before)

    def test_installed_icon_kept_unowned_by_explicit_mapping(self):
        # The released record as written, with its installed icon that the user
        # replaced: an explicit mapping keeps the icon as unowned user state.
        with tempfile.TemporaryDirectory() as d:
            result, source, before, copy_root = self.prepare(d, 'managed-upgraded', 'SYS:MigA', icons='unowned')
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            receipt = json.loads(result.stdout)
            mini = next(o for o in receipt['observations'] if o['name'] == 'mini')
            self.assertEqual(mini['icon'], 'installed; changed since installation; kept as unowned user state')
            self.assertEqual(receipt['legacy_environment'], 'not-inspected')
            done = self.commit(copy_root, receipt['approval'])
            self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
            state = json.loads((copy_root / '.apkg/state').read_bytes())
            self.assertNotIn('mini.info', json.dumps(state))
            self.assertEqual((copy_root / 'mini.info').read_bytes(), (source / 'mini.info').read_bytes())
            self.assertEqual(migration.inventory(source), before)

    def test_unknown_icon_policy_is_refused(self):
        with tempfile.TemporaryDirectory() as d:
            result, source, before, copy_root = self.prepare(d, 'managed-upgraded', 'SYS:MigA', icons='owned')
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('invalid icon mapping policy', result.stderr)
            self.assertFalse(copy_root.exists())

    def test_unfinished_replacement_is_refused_and_preserved(self):
        # db/replace.json as arospkg 0.6.0 and later write it before the new
        # package of a replacement is installed (relations_io.c write_replace).
        record = b'{ "schema":1, "new":"mini", "old":["oldmini"] }\n'
        with tempfile.TemporaryDirectory() as d:
            result, source, before, copy_root = self.prepare(
                d, 'managed-upgraded', 'SYS:MigA', icons='unowned',
                change=lambda root: (root / 'db/replace.json').write_bytes(record))
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('unfinished legacy replacement (db/replace.json) requires recovery', result.stderr)
            self.assertEqual(migration.inventory(source), before)
            self.assertEqual((source / 'db/replace.json').read_bytes(), record)
            self.assertFalse(copy_root.exists())

    def test_self_update_journal_in_the_environment_is_refused(self):
        with tempfile.TemporaryDirectory() as d:
            env = Path(d) / 'envarc-arospkg'; env.mkdir()
            # The layout of su_journal_text in arospkg's selfup.c.
            journal = (b'{\n  "format":1,\n  "from":"0.7.4",\n  "to":"0.7.5",\n  "root":"SYS:MigA",\n'
                       b'  "managed_to":"",\n  "files": [\n    { "path":"C:apkg", "from":"' + b'a' * 64 +
                       b'", "to":"' + b'b' * 64 + b'" }\n  ]\n}\n')
            (env / 'self-update.json').write_bytes(journal)
            result, source, before, copy_root = self.prepare(d, 'managed-upgraded', 'SYS:MigA', icons='unowned', env=env)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('unfinished self-update (ENVARC:arospkg/self-update.json)', result.stderr)
            self.assertEqual((env / 'self-update.json').read_bytes(), journal)
            self.assertEqual(migration.inventory(source), before)
            self.assertFalse(copy_root.exists())

    def test_copies_record_is_retained_not_converted(self):
        with tempfile.TemporaryDirectory() as d:
            env = Path(d) / 'envarc-arospkg'; env.mkdir()
            # The layout of the copies record written by arospkg's selfup.c.
            copies = b'{\n  "schema":1,\n  "copies": [\n    { "path":"C:apkg", "drawer":"SYS:Utilities/arospkg" }\n  ]\n}\n'
            (env / 'copies.json').write_bytes(copies)
            result, source, before, copy_root = self.prepare(d, 'managed-upgraded', 'SYS:MigA', icons='unowned', env=env)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(json.loads(result.stdout)['legacy_environment'], 'inspected')
            body = json.loads((copy_root / '.apkg-legacy/import.json').read_bytes())['body']
            self.assertEqual(body['legacy_environment']['copies'], 'retained-not-converted')
            self.assertEqual(bytes.fromhex(body['legacy_environment']['copies_record']), copies)

    def test_interrupted_install_is_refused_and_preserved(self):
        with tempfile.TemporaryDirectory() as d:
            result, source, before, copy_root = self.prepare(d, 'interrupted-install', 'SYS:MigB')
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('legacy transactions require recovery', result.stdout + result.stderr)
            self.assertEqual(migration.inventory(source), before)
            self.assertFalse(copy_root.exists())


RECORDS = ROOT / 'tests/fixtures/migration/legacy-records'


class RecordsOfEveryLine(unittest.TestCase):
    # Records as each arospkg release line wrote them (see the README there).
    def test_every_line_is_read_with_unowned_icons(self):
        paths = sorted(RECORDS.glob('*/*.json'))
        self.assertGreaterEqual(len(paths), 9)
        for path in paths:
            raw = path.read_bytes(); record = migration.decode(raw)
            evidence = migration.package_evidence(record, raw, 'unowned')
            self.assertEqual(bytes.fromhex(evidence['body']['record']), raw, path.name)
            if record.get('icon', {}).get('state') == 'installed':
                with self.assertRaisesRegex(migration.Refused, 'owned root icon placement is not mapped'):
                    migration.package_evidence(record, raw)

    def test_the_0_1_line_has_no_subdir_and_is_kept_as_written(self):
        raw = (RECORDS / 'aros/openloco-0.1-line.json').read_bytes(); record = migration.decode(raw)
        self.assertFalse({'subdir', 'revision', 'abi', 'previous'} & set(record))
        migration.package_evidence(record, raw)


if __name__ == '__main__':
    unittest.main()
