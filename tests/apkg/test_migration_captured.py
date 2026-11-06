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

    def prepare(self, d, case, legacy_root):
        base = Path(d); source = base / 'original'
        shutil.copytree(globals()['CAPTURED'] / case, source)
        mapping = base / 'mapping.json'
        packages = self.artifacts(base, source) if (source / 'db/installed/mini.json').exists() else []
        mapping.write_bytes(canonical(dict(format='apkg-legacy-import-1', legacy_root=legacy_root, packages=packages)))
        seed = base / 'policy.json'; seed.write_bytes(policy())
        before = migration.inventory(source)
        result = subprocess.run([sys.executable, str(ROOT / 'tools/apkg_migrate.py'), '--apkg', str(CLI), 'prepare',
                                 str(source), str(base / 'copy'), str(mapping), str(seed), V1, '--quiescent'],
                                capture_output=True, text=True)
        return result, source, before, base / 'copy'

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

    def test_interrupted_install_is_refused_and_preserved(self):
        with tempfile.TemporaryDirectory() as d:
            result, source, before, copy_root = self.prepare(d, 'interrupted-install', 'SYS:MigB')
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('legacy transactions require recovery', result.stdout + result.stderr)
            self.assertEqual(migration.inventory(source), before)
            self.assertFalse(copy_root.exists())


if __name__ == '__main__':
    unittest.main()
