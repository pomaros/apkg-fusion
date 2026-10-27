# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
"""Signed author text stays distinct from missing facts and qualifications."""
import ctypes as C
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from support import *
from test_author import author_fixture, key_fixture, build, members
from test_client import cli, WORKER
import test_client as clients
from test_file_channel import channel_key, publish
from test_channel import CHANNEL, seed
from test_inspection import info
from test_operations import install

lib.apkg_presentation_field.argtypes = [Text, C.c_char_p, C.POINTER(C.c_void_p), C.c_void_p]
lib.apkg_presentation_field.restype = C.c_int


def canonical(value):
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(',', ':')).encode()


def field(record, name):
    raw = canonical(record) if record is not None else b''
    data = C.create_string_buffer(raw)
    out = C.c_void_p(123)
    code = lib.apkg_presentation_field(Text(C.cast(data, C.c_void_p), len(raw)),
                                     name.encode(), C.byref(out), None)
    value = C.string_at(out).decode() if out else None
    lib.apkg_client_text_free(out)
    return code, value


class Presentation(unittest.TestCase):
    def tearDown(self):
        lib.apkg_test_fail_alloc(0)

    def test_missing_empty_unicode_quotes_and_limits_are_distinct(self):
        self.assertEqual(field(None, 'summary'), (5, None))
        self.assertEqual(field({}, 'category'), (5, None))
        self.assertEqual(field({'category': ''}, 'category'), (0, ''))
        text = 'Crème "quoted" \\ path'
        self.assertEqual(field({'summary': text}, 'summary'), (0, text))
        self.assertEqual(field({'summary': 'é' * 2048}, 'summary'), (0, 'é' * 2048))
        self.assertEqual(field({'summary': 'x' * 4096}, 'summary'), (0, 'x' * 4096))
        for record in ({'summary': 'x' * 4097}, {'category': 'x' * 256},
                       {'summary': '\x1bcontrol'}, {'summary': 'line\nline'},
                       {'summary': 'zero\0tail'}, {'summary': 'é' * 2049},
                       {'review': 'approved'}, {'summary': None}):
            self.assertEqual(field(record, 'summary'), (1, None), record)
        self.assertEqual(field({}, 'review'), (1, None))

    def test_allocator_failures_clear_output_and_preserve_input(self):
        record = {'summary': 'a' * 1024, 'category': 'util'}
        for fault in range(1, 25):
            lib.apkg_test_fail_alloc(fault)
            try:
                code, value = field(record, 'summary')
            finally:
                lib.apkg_test_fail_alloc(0)
            self.assertIn(code, (0, 4), fault)
            self.assertEqual(value, record['summary'] if code == 0 else None)

    def test_real_authoring_publication_discovery_install_and_native_rows_keep_text(self):
        with tempfile.TemporaryDirectory() as temporary:
            d = Path(temporary)
            source = d / 'Drawer'
            manifest = author_fixture(source, kind='app', launch=True)
            metadata = dict(summary='Crème "quoted" \\ path', category='util/tools',
                            license='MIT', repository='https://example.invalid/source')
            text = manifest.read_text().replace('summary = "Author test"',
                'summary = ' + json.dumps(metadata['summary'], ensure_ascii=False))
            text = text.replace('category = "util"', 'category = "util/tools"')
            manifest.write_text(text + '\n[source]\nrepository = "https://example.invalid/source"\n')
            original = manifest.read_bytes()
            key = key_fixture(d / 'author-key')
            archive = d / 'package.pkg'
            built = build(source, archive, key)
            self.assertEqual(built.returncode, 0, built.stderr)
            packed = members(archive)
            self.assertEqual(json.loads(packed['descriptor.json'])['body']['presentation'], metadata)
            tampered = json.loads(packed['descriptor.json'])
            tampered['body']['presentation']['summary'] = 'different author text'
            bad, signature = d / 'tampered', d / 'signature'
            bad.write_bytes(canonical(tampered))
            signature.write_bytes(packed['signature.json'])
            refused = cli('verify-object', 'descriptor', bad, signature)
            self.assertEqual(refused.returncode, 10, refused.stdout + refused.stderr)
            endpoint = d / 'channel'
            published = publish(archive, endpoint, channel_key(d / 'channel-key'))
            self.assertEqual(published.returncode, 0, published.stderr)
            root = d / 'root'
            before = state(root, policy=seed())
            observed = cli('channel-open', root, endpoint, CHANNEL)
            self.assertEqual(observed.returncode, 0, observed.stderr)
            self.assertEqual(json.loads(observed.stdout)['entries'][0]['presentation'], metadata)
            native = subprocess.run([str(WORKER), str(root), str(endpoint), '16', '7', CHANNEL],
                                    capture_output=True, text=True)
            self.assertEqual(native.returncode, 0, native.stdout + native.stderr)
            self.assertEqual((root / '.apkg/state').read_bytes(), before)
            clients.OperationClients().commit('install', root, archive)
            for name, value in metadata.items():
                self.assertEqual(info(root, field=name), (0, value))
                answer = cli('info', root, 'demo', name)
                self.assertEqual(answer.returncode, 0, answer.stderr)
                self.assertEqual(answer.stdout, value + '\nstatus=0 outcome=0\n')
            legacy = cli('legacy', '--root', root, 'info', 'demo', '--json')
            self.assertEqual(legacy.returncode, 0, legacy.stderr)
            result = json.loads(legacy.stdout)
            self.assertEqual(result['summary'], metadata['summary'])
            self.assertEqual(result['category'], metadata['category'])
            self.assertEqual(result['unrecorded'], ['size'])
            # Complete the same real package lifecycle through both clients.
            worker_root = d / 'worker-root'
            state(worker_root, policy=seed())
            helper = clients.OperationClients()
            helper.worker(worker_root, archive, 0)
            helper.compare(root, worker_root)
            launched = cli('run', root, 'demo')
            self.assertEqual(launched.returncode, 2, launched.stdout + launched.stderr)
            self.assertIn('AROS execution is unavailable on this host', launched.stderr)
            launched = subprocess.run([str(WORKER), str(worker_root), 'demo', '12'],
                                      capture_output=True, text=True)
            self.assertEqual(launched.returncode, 0, launched.stdout + launched.stderr)
            for installed_root in (root, worker_root):
                (installed_root / 'demo/personal.txt').write_text('keep my data')
                (installed_root / 'demo/Drawer/.hidden').write_text('personal configuration')
                verified = cli('verify', installed_root, 'demo')
                self.assertEqual(verified.returncode, 0, verified.stdout + verified.stderr)
            helper.commit('remove', root, 'demo')
            helper.worker(worker_root, 'demo', 1)
            helper.compare(root, worker_root)
            for installed_root in (root, worker_root):
                self.assertEqual(json.loads((installed_root / '.apkg/state').read_bytes())['body']['installed'], [])
                self.assertFalse((installed_root / 'demo/Drawer/program').exists())
                self.assertEqual((installed_root / 'demo/personal.txt').read_text(), 'keep my data')
                self.assertEqual((installed_root / 'demo/Drawer/.hidden').read_text(), 'personal configuration')
                self.assertFalse(installed_root.with_name(installed_root.name + '.apkg-transaction').exists())
                self.assertEqual(info(installed_root), (5, None))
            self.assertEqual(manifest.read_bytes(), original)

    def test_old_descriptors_remain_unknown_and_empty_is_recorded(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / 'root'
            state(root, policy=policy())
            code, ctx = open_ctx(root, 1)
            self.assertEqual(code, 0)
            try:
                data = json.loads(descriptor())
                data['body']['presentation'] = {'summary': '', 'category': ''}
                install(ctx, canonical(data))
            finally:
                lib.apkg_close(ctx)
            self.assertEqual(info(root, field='summary'), (0, ''))
            self.assertEqual(info(root, field='category'), (0, ''))
            self.assertEqual(info(root, field='license'), (0, 'unknown'))
            legacy = cli('legacy', '--root', root, 'info', 'demo', '--json')
            result = json.loads(legacy.stdout)
            self.assertEqual(result['summary'], '')
            self.assertEqual(result['category'], '')
            self.assertEqual(result['unrecorded'], ['size'])

    def test_author_cannot_embed_review_status_or_exceed_display_bounds(self):
        with tempfile.TemporaryDirectory() as temporary:
            d = Path(temporary)
            for presentation in ({'review': 'approved'}, {'category': 'x' * 256},
                                 {'summary': 'safe\x1b[style'}, {'runtime': 'passed'}):
                document = json.loads(descriptor())
                document['body']['presentation'] = presentation
                data = canonical(document)
                path, signature = d / 'descriptor', d / 'signature'
                path.write_bytes(data)
                signature.write_bytes(sign(data))
                result = cli('verify-object', 'descriptor', path, signature)
                self.assertNotEqual(result.returncode, 0)
