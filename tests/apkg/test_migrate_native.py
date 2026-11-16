# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tomasz Staniak
"""apkg migrate check/copy: the native inspection and copy of a classic root.

The roots are the ones released arospkg 0.7.4 wrote on mainline AROS
(tests/fixtures/migration/captured-0.7.4-v1)."""
import hashlib
import json
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from support import ROOT

CLI = Path(os.environ.get('APKG_CLI', ROOT / 'build/apkg'))
CAPTURED = ROOT / 'tests/fixtures/migration/captured-0.7.4-v1'


def run(*args):
    return subprocess.run([str(CLI), 'migrate', *map(str, args)], capture_output=True, text=True)


def tree(path):
    """Every node with its content hash and mode."""
    rows = {}
    for base, dirs, files in os.walk(path):
        for name in dirs + files:
            full = Path(base) / name
            rel = str(full.relative_to(path))
            mode = full.lstat().st_mode & 0o777
            rows[rel] = (mode, None if full.is_dir() else hashlib.sha256(full.read_bytes()).hexdigest())
    return rows


class NativeMigration(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.base = Path(self.tmp.name).resolve()
        self.source = self.base / 'original'
        shutil.copytree(CAPTURED / 'managed-upgraded', self.source)
        self.copy = self.base / 'copy'

    def tearDown(self):
        self.tmp.cleanup()

    def refused(self, result, code, words):
        self.assertEqual(result.returncode, code, result.stderr)
        self.assertIn(words, result.stderr)
        self.assertIn('migration refused', result.stderr)

    def record(self, name='mini'):
        return self.source / 'db/installed' / (name + '.json')

    def edit(self, change, name='mini'):
        data = json.loads(self.record(name).read_bytes())
        change(data)
        self.record(name).write_text(json.dumps(data))

    def test_check_reports_each_package_and_changes_nothing(self):
        before = tree(self.source)
        result = run('check', self.source, '--json')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(tree(self.source), before)
        document = json.loads(result.stdout)
        self.assertEqual(document['format'], 'apkg-legacy-inspection-1')
        body = document['body']
        self.assertIsNone(body['destination'])
        self.assertFalse(body['quiescent_operator_assertion'])
        mini, minilib = body['packages']
        self.assertEqual((mini['name'], mini['version'], mini['revision'], mini['arch'], mini['abi'], mini['reason']),
                         ('mini', '1.1', '3', 'x86_64', 'v1', 'explicit'))
        # The user changed Mini.cfg, added notes.txt and replaced the icon.
        self.assertEqual(mini['files'], dict(changed='1', missing='0', recorded='3', unchanged='2', unrecorded='1'))
        self.assertEqual(mini['icon'], 'installed-changed')
        self.assertEqual(mini['authentication'], 'none-recorded')
        self.assertEqual(mini['record_sha256'], hashlib.sha256(self.record().read_bytes()).hexdigest())
        self.assertEqual(mini['previous']['registry'], 'SYS:MigA/db/previous/mini.json')
        self.assertEqual((minilib['name'], minilib['reason'], minilib['icon']), ('minilib', 'dependency', 'none'))
        self.assertEqual(minilib['previous'], dict(archive=None, registry=None))
        files = [p for p in self.source.rglob('*') if p.is_file()]
        self.assertEqual(body['inventory']['files'], str(len(files)))
        self.assertEqual(body['inventory']['bytes'], str(sum(p.stat().st_size for p in files)))
        self.assertEqual(body['environment']['state'], 'not-inspected')
        plain = run('check', self.source)
        self.assertEqual(plain.returncode, 0)
        self.assertIn('mini 1.1 revision 3', plain.stdout)
        self.assertIn('Nothing was changed.', plain.stdout)

    def test_missing_recorded_file_and_delivered_icon_are_reported(self):
        (self.source / 'mini/Mini').unlink()
        icon = self.source / 'mini.info'
        self.edit(lambda r: r['icon'].update(sha256=hashlib.sha256(icon.read_bytes()).hexdigest()))
        mini = json.loads(run('check', self.source, '--json').stdout)['body']['packages'][0]
        self.assertEqual(mini['files']['missing'], '1')
        self.assertEqual(mini['icon'], 'installed-delivered')
        icon.unlink()
        mini = json.loads(run('check', self.source, '--json').stdout)['body']['packages'][0]
        self.assertEqual(mini['icon'], 'installed-missing')

    def test_copy_is_complete_and_the_original_is_only_read(self):
        os.chmod(self.source / 'mini/Mini.cfg', 0o600)
        os.utime(self.source / 'mini/Mini', (1700000000, 1700000000))
        before = tree(self.source)
        result = run('copy', self.source, self.copy, '--quiescent')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(tree(self.source), before)
        after = tree(self.copy)
        moved = {('.apkg-legacy/' + k if k == 'db' or k.startswith('db/') else k): v for k, v in before.items()}
        receipt = self.copy / '.apkg-legacy/inspection.json'
        self.assertEqual({k: v for k, v in after.items() if k not in ('.apkg-legacy', '.apkg-legacy/inspection.json')}, moved)
        self.assertEqual(int((self.copy / 'mini/Mini').stat().st_mtime), 1700000000)
        raw = receipt.read_bytes()
        self.assertIn(hashlib.sha256(raw).hexdigest(), result.stdout)
        document = json.loads(raw)
        self.assertEqual(raw, json.dumps(document, sort_keys=True, separators=(',', ':'), ensure_ascii=False).encode())
        self.assertEqual(document['body']['destination'], str(self.copy))
        self.assertEqual(document['body']['source'], str(self.source))
        self.assertTrue(document['body']['quiescent_operator_assertion'])
        self.assertEqual(document['body']['packages'],
                         json.loads(run('check', self.source, '--json').stdout)['body']['packages'])
        self.assertIn('not yet registered', result.stdout)
        # A copy is not a classic root any more, and is never written twice.
        self.refused(run('check', self.copy), 17, 'already converted')
        self.refused(run('copy', self.source, self.copy, '--quiescent'), 14, 'destination exists')

    def test_copy_json_answer(self):
        result = run('copy', self.source, self.copy, '--quiescent', '--json')
        self.assertEqual(result.returncode, 0, result.stderr)
        answer = json.loads(result.stdout)
        self.assertEqual(answer['status'], 'copied')
        self.assertEqual(answer['inspection_sha256'],
                         hashlib.sha256((self.copy / '.apkg-legacy/inspection.json').read_bytes()).hexdigest())

    def test_copy_needs_the_quiescence_statement_and_a_new_place(self):
        self.refused(run('copy', self.source, self.copy), 17, '--quiescent')
        self.assertFalse(self.copy.exists())
        self.refused(run('copy', self.source, self.source / 'inner', '--quiescent'), 1, 'inside the root')
        self.refused(run('copy', self.source, self.base / 'absent/copy', '--quiescent'), 5, 'does not exist')
        self.assertEqual(run('copy', self.source).returncode, 1)
        self.assertEqual(run('check', self.source, '--quiescent').returncode, 1)

    def test_unfinished_classic_work_refuses_before_anything_is_copied(self):
        cases = [
            (lambda: (self.source / 'db/lock.owner').write_text('x'), 7, 'lock'),
            (lambda: (self.source / 'db/replace.json').write_text('{}'), 9, 'replace.json'),
            (lambda: (self.source / 'db/transactions/1-mini').mkdir(parents=True), 9, 'db/transactions'),
            (lambda: (self.source / 'cache/self-update.json').write_text('{}'), 9, 'self-update'),
            (lambda: (self.source / '.apkg').mkdir(), 17, 'already converted'),
        ]
        for make, code, words in cases:
            with self.subTest(words):
                shutil.rmtree(self.source)
                shutil.copytree(CAPTURED / 'managed-upgraded', self.source)
                make()
                self.refused(run('copy', self.source, self.copy, '--quiescent'), code, words)
                self.assertFalse(self.copy.exists())

    def test_interrupted_install_captured_from_0_7_4_refuses(self):
        other = self.base / 'interrupted'
        shutil.copytree(CAPTURED / 'interrupted-install', other)
        self.refused(run('check', other), 9, 'db/transactions')

    def test_records_this_reader_cannot_interpret_refuse(self):
        cases = [
            (lambda r: r.update(mystery=1), 2, 'fields this reader does not know'),
            (lambda r: r.update(schema=2), 2, 'unsupported record schema'),
            (lambda r: r.pop('contents'), 2, 'no file baseline'),
            (lambda r: r.update(dir='other'), 2, 'do not identify one package'),
            (lambda r: r.update(depends=['absent >= 1.0']), 15, 'dependency is not installed'),
            (lambda r: r.update(conflicts=['minilib'], depends=[]), 14, 'names an installed package'),
            (lambda r: r['contents'].append(dict(path='mini.CFG', sha256='0' * 64, size=1)), 2, 'one file twice'),
            (lambda r: r['contents'].append(dict(path='../x', sha256='0' * 64, size=1)), 2, 'invalid file baseline'),
        ]
        original = self.record().read_bytes()
        for change, code, words in cases:
            with self.subTest(words):
                self.record().write_bytes(original)
                self.edit(change)
                self.refused(run('check', self.source), code, words)
        self.record().write_text('{"schema":1,"schema":1}')
        self.refused(run('check', self.source), 1, 'not readable JSON')
        self.record().write_bytes(original)
        self.record().rename(self.source / 'db/installed/renamed.json')
        self.refused(run('check', self.source), 1, 'not its package name')

    def test_nodes_that_cannot_be_migrated_are_named(self):
        (self.source / 'mini/link').symlink_to('Mini')
        self.refused(run('copy', self.source, self.copy, '--quiescent'), 2, 'mini/link')
        self.assertFalse(self.copy.exists())
        (self.source / 'mini/link').unlink()
        (self.source / 'mini/zażółć.txt').write_text('x')
        self.refused(run('check', self.source), 2, 'cannot represent')

    def test_classic_environment_copies(self):
        envarc = self.base / 'envarc'; env = self.base / 'env'
        envarc.mkdir(); env.mkdir()
        (envarc / 'copies.json').write_text('{ "copies": ["C:apkg"] }')
        (env / 'self-update-result.json').write_text('{}')
        result = run('check', self.source, '--json', '--legacy-envarc', envarc, '--legacy-env', env)
        self.assertEqual(result.returncode, 0, result.stderr)
        found = json.loads(result.stdout)['body']['environment']
        self.assertEqual(found['state'], 'inspected')
        self.assertEqual(bytes.fromhex(found['envarc']['copies_record']), (envarc / 'copies.json').read_bytes())
        self.assertTrue(found['env']['unread_result'])
        self.assertEqual(found['env']['source'], 'copy')
        partial = json.loads(run('check', self.source, '--json', '--legacy-envarc', envarc).stdout)
        self.assertEqual(partial['body']['environment']['state'], 'partial')
        (envarc / 'self-update.json').write_text('{}')
        self.refused(run('check', self.source, '--legacy-envarc', envarc), 9, 'self-update')
        (envarc / 'self-update.json').unlink()
        (envarc / 'apkg.new').write_text('x')
        self.refused(run('check', self.source, '--legacy-envarc', envarc), 9, 'partial write')
        (envarc / 'apkg.new').unlink()
        (env / 'self-update.lock').write_text('x')
        self.refused(run('check', self.source, '--legacy-envarc', envarc, '--legacy-env', env), 7, 'holds its lock')
        self.refused(run('check', self.source, '--legacy-env', self.base / 'absent'), 5, 'not a physical directory')

    def test_no_installed_package_is_nothing_to_migrate(self):
        for path in (self.source / 'db/installed').iterdir():
            path.unlink()
        self.refused(run('check', self.source), 5, 'nothing to migrate')
        shutil.rmtree(self.source / 'db')
        self.refused(run('check', self.source), 5, 'not a root of the classic client')

    def test_a_larger_root_than_one_engine_document_holds(self):
        # 9000 files are more than one 100000-token engine inventory holds.
        extra = self.source / 'mini/data'
        for d in range(30):
            (extra / str(d)).mkdir(parents=True)
            for f in range(300):
                (extra / str(d) / (str(f) + '.txt')).write_text(str(d * 300 + f))
        result = run('copy', self.source, self.copy, '--quiescent', '--json')
        self.assertEqual(result.returncode, 0, result.stderr)
        document = json.loads((self.copy / '.apkg-legacy/inspection.json').read_bytes())
        self.assertEqual(document['body']['packages'][0]['files']['unrecorded'], '9001')
        self.assertEqual((self.copy / 'mini/data/29/299.txt').read_text(), '8999')


if __name__ == '__main__':
    unittest.main()
