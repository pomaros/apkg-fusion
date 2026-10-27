# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
"""An exported marker cannot hide a partial or semantically wrong target run."""
import json
import hashlib
from pathlib import Path
import runpy
import subprocess
import sys
import tempfile
import unittest

verify = runpy.run_path(str(Path(__file__).resolve().parents[1] / 'aros/verify_rehearsal.py'))['verify']


class RehearsalEvidence(unittest.TestCase):
    def test_preparation_uses_real_signed_fixtures_without_shipping_secrets(self):
        import test_client as clients
        script = Path(__file__).resolve().parents[1] / 'aros/prepare_rehearsal.py'
        with tempfile.TemporaryDirectory() as temporary:
            out = Path(temporary) / 'prepared'
            command = [sys.executable, str(script), '--output', str(out)]
            command.extend(['--namespace', 'Isolated2', '--runtime-dir', 'RAM:Isolated2'])
            # Host executables are inert stand-ins for explicit target inputs;
            # preparation copies these files and must never execute the helpers.
            for option in ('--cli', '--gui', '--launch', '--norequesters'):
                command.extend([option, str(clients.CLI)])
            result = subprocess.run(command, capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            prepared = json.loads((out / 'prepared.json').read_text())
            self.assertEqual(len(prepared['steps']), 25)
            self.assertEqual(prepared['native_execution'], 'not-performed')
            payload = out / 'payload'
            observed = {str(p.relative_to(payload)): hashlib.sha256(p.read_bytes()).hexdigest()
                        for p in payload.rglob('*') if p.is_file()}
            self.assertEqual(observed, prepared['files'])
            secrets = [(out / 'host' / name).read_bytes()
                       for name in ('author-key', 'channel-key')]
            for p in payload.rglob('*'):
                if p.is_file():
                    self.assertFalse(any(secret in p.read_bytes() for secret in secrets), str(p))
            script_text = (payload / 'APKGTrial/Trial').read_text()
            self.assertIn('BEGIN refuse-tamper', script_text)
            self.assertIn('If NOT $APKGRC EQ 10 VAL', script_text)
            self.assertIn('>RAM:Isolated2TrialRoot/trial/Drawer/.hidden', script_text)
            self.assertNotIn('SYS:APKGTrial', script_text)
            self.assertEqual(prepared['report'], 'RAM:Isolated2TrialReport.txt')
            state = json.loads((payload / 'APKGTrial/root-state').read_text())
            self.assertEqual(state['body']['sources'][0]['endpoint'], 'RAM:Isolated2TrialChannel')
            before = dict(observed)
            refused = subprocess.run(command, capture_output=True, text=True, timeout=120)
            self.assertNotEqual(refused.returncode, 0)
            self.assertEqual({str(p.relative_to(payload)): hashlib.sha256(p.read_bytes()).hexdigest()
                              for p in payload.rglob('*') if p.is_file()}, before)

    def fixture(self):
        info = dict(name='trial', version='1.2', revision=1, arch='aarch64',
                    abi='v1', signature='verified', authortrust='accepted', installed=True)
        rows = [
            ('version', 0, 'apkg-fusion 0.1.0-alpha (common engine, contract 1)'),
            ('info-v1', 0, json.dumps(info)), ('refuse-tamper', 10, ''),
            ('still-v1', 0, json.dumps(info)),
            ('info-v2', 0, json.dumps(dict(info, version='2.0'))),
            ('edit-config', 0, 'personal-configuration'),
            ('verify', 0, 'Signature: verified\nAuthor trust: accepted\nchanged 1\nmissing 0\nunknown 0\nok Drawer/program\nchanged Drawer/.hidden [configuration]'),
            ('config', 0, 'personal-configuration'),
            ('list-final', 0, 'nothing installed'),
            ('kept-config', 0, 'personal-configuration')]
        prepared = dict(format='apkg-aros-rehearsal-1', nonce='fresh-run',
                        steps=[dict(label=name, expected=rc) for name, rc, _ in rows])
        report = 'APKG-REHEARSAL fresh-run\n' + ''.join(
            f'BEGIN {name} rc={rc}\n{body}\n' for name, rc, body in rows) + 'PASS fresh-run\n'
        return prepared, report

    def test_complete_report_and_platform_line_endings(self):
        prepared, report = self.fixture()
        self.assertEqual(verify(prepared, report)['result'], 'PASS')
        self.assertEqual(verify(prepared, report.replace('\n', '\r\n'))['result'], 'PASS')

    def test_false_pass_and_tamper_acceptance_are_rejected(self):
        prepared, report = self.fixture()
        for bad in (report.replace('BEGIN refuse-tamper rc=10', 'BEGIN refuse-tamper rc=0'),
                    report.replace('BEGIN still-v1 rc=0', 'BEGIN info-v1 rc=0'),
                    report.replace('BEGIN config rc=0\npersonal-configuration\n', ''),
                    report.replace('PASS fresh-run', 'PASS stale-run'),
                    'APKG-REHEARSAL fresh-run\nPASS fresh-run\n',
                    report + 'unaccounted output\n',
                    report.replace('PASS fresh-run', 'FAIL install rc=10\nPASS fresh-run')):
            with self.subTest(report=bad), self.assertRaises(ValueError):
                verify(prepared, bad)

    def test_return_codes_do_not_replace_content_checks(self):
        prepared, report = self.fixture()
        for old, new in [('"version": "2.0"', '"version": "1.2"'),
                         ('"signature": "verified"', '"signature": "invalid"'),
                         ('personal-configuration', 'default-configuration'),
                         ('nothing installed', 'trial'),
                         ('missing 0', 'missing 1'),
                         ('common engine, contract 1', 'inherited engine')]:
            with self.subTest(field=old), self.assertRaises(ValueError):
                verify(prepared, report.replace(old, new))


if __name__ == '__main__':
    unittest.main()
