# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
import json
import io
from contextlib import redirect_stdout
from pathlib import Path
import runpy
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch
from test_file_channel import artifact, channel_key, publish, head
from support import ROOT
import test_client as clients

HELPER = ROOT / 'tools/apkg-portal-commit'


class PortalCommitRecovery(unittest.TestCase):
    def test_real_process_death_before_and_after_first_visibility_commit_is_retryable(self):
        with tempfile.TemporaryDirectory() as temporary:
            temporary = Path(temporary)
            archive = artifact(temporary)
            key = channel_key(temporary / 'channel-key')
            source = temporary / 'source'
            built = publish(archive, source, key)
            self.assertEqual(built.returncode, 0, built.stderr)
            digest = head(source)
            for boundary in ('before', 'after'):
                with self.subTest(boundary=boundary):
                    endpoint = temporary / ('live-' + boundary)
                    args = dict(engine=str(clients.CLI), endpoint=str(endpoint), staging=str(source),
                                publisher='synthetic-publisher', snapshot=digest, expected='-')
                    script = '''
import json,os,runpy,sys
from types import SimpleNamespace
module=runpy.run_path(sys.argv[1],run_name='interruption_fixture')
producer=module['producer']; original=producer['replace_current']
def stop(*args):
 if sys.argv[3]=='after': original(*args)
 os._exit(99)
producer['replace_current']=stop
module['commit'](SimpleNamespace(**json.loads(sys.argv[2])))
'''
                    stopped = subprocess.run([sys.executable, '-c', script, str(HELPER), json.dumps(args), boundary],
                                             capture_output=True, text=True, timeout=60)
                    self.assertEqual(stopped.returncode, 99, stopped.stderr)
                    self.assertTrue((endpoint / '.portal-owner.json').is_file())
                    self.assertEqual((endpoint / 'current').exists(), boundary == 'after')
                    command = [sys.executable, str(HELPER)]
                    for name, value in args.items():
                        command.extend(['--' + name, value])
                    retry = subprocess.run(command, capture_output=True, text=True, timeout=60)
                    self.assertEqual(retry.returncode, 0, retry.stderr)
                    answer = json.loads(retry.stdout)
                    self.assertEqual(answer['result'], 'published' if boundary == 'before' else 'unchanged')
                    self.assertEqual(head(endpoint), digest)
                    document = json.loads((source / 'snapshots' / digest / 'channel.json').read_bytes())
                    archive_digest = document['body']['entries'][0]['archive']['sha256']
                    self.assertEqual((endpoint / 'archives' / (archive_digest + '.pkg')).read_bytes(), archive.read_bytes())

    def test_archive_write_and_final_flush_failures_preserve_retryable_outcomes(self):
        with tempfile.TemporaryDirectory() as temporary:
            temporary = Path(temporary)
            archive = artifact(temporary)
            key = channel_key(temporary / 'channel-key')
            source = temporary / 'source'
            self.assertEqual(publish(archive, source, key).returncode, 0)
            digest = head(source)
            for boundary in ('archive', 'final-flush'):
                with self.subTest(boundary=boundary):
                    helper = runpy.run_path(str(HELPER), run_name='publication_io_fixture')
                    producer = helper['producer']
                    endpoint = temporary / ('live-' + boundary)
                    args = SimpleNamespace(engine=str(clients.CLI), endpoint=str(endpoint), staging=str(source),
                                           publisher='synthetic-publisher', snapshot=digest, expected='-')
                    if boundary == 'archive':
                        def fail_archive(*unused):
                            raise OSError('injected archive write failure')
                        with patch.dict(producer, {'retain_archive': fail_archive}):
                            with self.assertRaises(OSError):
                                helper['commit'](args)
                        self.assertFalse((endpoint / 'current').exists())
                        self.assertFalse((endpoint / '.portal-owner.json').exists())
                    else:
                        globals_ = producer['replace_current'].__globals__
                        flush = globals_['flush_dir']
                        def fail_final(path):
                            if Path(path) == endpoint and (endpoint / 'current').exists():
                                raise OSError('injected final directory flush failure')
                            return flush(path)
                        with patch.dict(globals_, {'flush_dir': fail_final}):
                            with self.assertRaises(producer['Refusal']):
                                helper['commit'](args)
                            command = ['apkg-portal-commit']
                            for name, value in vars(args).items():
                                command.extend(['--' + name, value])
                            output = io.StringIO()
                            with patch.object(sys, 'argv', command), redirect_stdout(output):
                                helper['main']()
                            self.assertEqual(json.loads(output.getvalue())['result'], 'uncertain')
                        self.assertEqual(head(endpoint), digest)
                    answer = helper['commit'](args)
                    self.assertEqual(answer['result'], 'published' if boundary == 'archive' else 'unchanged')
                    self.assertEqual(head(endpoint), digest)

    def test_inherited_unregistered_common_channel_requires_explicit_migration(self):
        with tempfile.TemporaryDirectory() as temporary:
            temporary = Path(temporary)
            archive = artifact(temporary)
            key = channel_key(temporary / 'channel-key')
            source = temporary / 'source'
            self.assertEqual(publish(archive, source, key).returncode, 0)
            before = (source / 'current').read_bytes()
            helper = runpy.run_path(str(HELPER), run_name='registration_fixture')
            args = SimpleNamespace(engine=str(clients.CLI), endpoint=str(source), staging=str(source),
                                   publisher='new-account', snapshot=head(source), expected='-')
            with self.assertRaises(helper['Refusal']) as caught:
                helper['commit'](args)
            self.assertEqual(caught.exception.code, 'conflict')
            self.assertEqual((source / 'current').read_bytes(), before)
            self.assertFalse((source / '.portal-owner.json').exists())
