# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
"""Typed interactive discovery preserves identities without legacy UI rows."""
import ctypes as C
import os
import subprocess
import tempfile
import unittest
from support import *
from test_channel import CHANNEL, CP, CS, ChannelRow, catalog, entry, seed
from test_channel_clients import apply, published, selection
from test_http_channel import serve
import test_client as clients

lib.apkg_client_channel_open.argtypes = [C.c_char_p, C.c_char_p, C.c_char_p,
    C.c_uint64, C.c_void_p, C.POINTER(C.c_void_p), C.c_void_p]
lib.apkg_client_channel_open.restype = C.c_int
lib.apkg_client_channel_selection.argtypes = [C.c_void_p, C.c_uint64,
    C.POINTER(C.c_void_p), C.c_void_p]
lib.apkg_client_channel_selection.restype = C.c_int


def browse(root, endpoint, floor=0, identity=CHANNEL):
    out = C.c_void_p()
    code = lib.apkg_client_channel_open(str(root).encode(), str(endpoint).encode(),
        identity.encode(), floor, None, C.byref(out), None)
    return code, out


def choose(channel, index):
    out = C.c_void_p()
    code = lib.apkg_client_channel_selection(channel, index, C.byref(out), None)
    text = C.string_at(out).decode() if out else None
    lib.apkg_client_text_free(out)
    return code, text


def fixture(endpoint, names, sequence=1, target=TARGET, version='1.0', revision=1):
    items = []
    for name in names:
        body = json.loads(descriptor(name=name, target=target))['body']
        body.update(version=version, revision=str(revision))
        items.append(entry(wire('descriptor', body)))
    data = catalog(items, sequence)
    digest = hashlib.sha256(data).hexdigest()
    directory = endpoint / 'snapshots' / digest
    directory.mkdir(parents=True)
    (directory / 'channel.json').write_bytes(data)
    (directory / 'channel.signature.json').write_bytes(sign(data, CP, CS, 'channel'))
    (endpoint / 'current').write_text(digest + '\n')
    return digest


class Browser(unittest.TestCase):
    def tearDown(self):
        lib.apkg_test_fail_alloc(0)

    def test_full_names_owned_views_exact_selection_and_readonly_http(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            endpoint, root = d / 'channel', d / 'root'
            names = ['a' * 255, 'a' * 254 + 'b']
            digest = fixture(endpoint, names)
            before = state(root, policy=seed())
            with serve(endpoint) as (url, server):
                code, channel = browse(root, url)
                self.assertEqual(code, 0)
                # The temporary read context is already closed by open().
                found = set()
                for index in range(2):
                    row = C.POINTER(ChannelRow)()
                    self.assertEqual(lib.apkg_channel_entry_at(channel, index, C.byref(row), None), 0)
                    found.add(raw(row.contents.name).decode())
                    code, selected = choose(channel, index)
                    self.assertEqual(code, 0)
                    self.assertEqual(selected, CHANNEL + '/' + digest + '/' + bytes(row.contents.descriptor_digest.bytes).hex())
                    self.assertEqual(row.contents.trust, 1)
                    self.assertEqual(row.contents.target_matches, 1)
                self.assertEqual(found, set(names))
                self.assertEqual(choose(channel, 2), (5, None))
                lib.apkg_channel_free(channel)
                self.assertFalse(any('/archives/' in path for path in server.requests))
                runner = Path(os.environ.get('APKG_CHANNEL_MEMORY', ROOT / 'build/test_channel_memory'))
                result = subprocess.run([str(runner), str(root), url, CHANNEL, '--browse'], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual((root / '.apkg/state').read_bytes(), before)
            self.assertFalse(Path(str(root) + '.apkg-objects').exists())
            self.assertFalse(Path(str(root) + '.apkg-archives').exists())

    def test_selection_executes_common_dependency_plan_and_refuses_changed_head(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            endpoint, _ = published(d)
            root = d / 'root'
            state(root, policy=seed())
            code, channel = browse(root, endpoint)
            self.assertEqual(code, 0)
            selected = None
            for index in range(4):
                row = C.POINTER(ChannelRow)()
                self.assertEqual(lib.apkg_channel_entry_at(channel, index, C.byref(row), None), 0)
                if raw(row.contents.name) == b'app':
                    code, selected = choose(channel, index)
                    self.assertEqual(code, 0)
            lib.apkg_channel_free(channel)
            self.assertEqual(selected, selection(endpoint))
            self.assertEqual(apply(root, endpoint, selected), (0, 1))
            self.assertTrue((root / 'app').is_dir())
            self.assertTrue((root / 'provider').is_dir())
            # Browse facts stay readable, but cannot authorize a new head.
            other = d / 'other-root'
            before = state(other, policy=seed())
            fixture(endpoint, ['changed'], sequence=5)
            self.assertEqual(apply(other, endpoint, selected)[0], 8)
            self.assertEqual((other / '.apkg/state').read_bytes(), before)

    def test_unknown_author_and_other_target_remain_visible_facts(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            endpoint, root = d / 'channel', d / 'root'
            fixture(endpoint, ['demo'], target='arm-aros-v11-native')
            before = state(root, policy=seed(authors=False))
            code, channel = browse(root, endpoint)
            self.assertEqual(code, 0)
            row = C.POINTER(ChannelRow)()
            self.assertEqual(lib.apkg_channel_entry_at(channel, 0, C.byref(row), None), 0)
            self.assertEqual(row.contents.signature, 1)
            self.assertEqual(row.contents.trust, 2)  # APKG_TRUST_REJECTED: no author grant
            self.assertEqual(row.contents.target_matches, 0)
            self.assertEqual(choose(channel, 0)[0], 0)
            lib.apkg_channel_free(channel)
            self.assertEqual((root / '.apkg/state').read_bytes(), before)

    def test_errors_clear_outputs_and_allocation_failure_is_retryable(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            endpoint, root = d / 'channel', d / 'root'
            fixture(endpoint, ['demo'])
            before = state(root, policy=seed())
            self.assertEqual(browse(root, endpoint, identity='invalid')[0], 1)
            self.assertEqual(browse(root, endpoint, floor=2)[0], 8)
            self.assertEqual(choose(None, 0), (1, None))
            code, channel = browse(root, endpoint)
            self.assertEqual(code, 0)
            lib.apkg_test_fail_alloc(1)
            self.assertEqual(choose(channel, 0), (4, None))
            lib.apkg_test_fail_alloc(0)
            self.assertEqual(choose(channel, 0)[0], 0)
            lib.apkg_channel_free(channel)
            for nth in range(1, 101):
                lib.apkg_test_fail_alloc(nth)
                code, channel = browse(root, endpoint)
                lib.apkg_test_fail_alloc(0)
                self.assertIn(code, (0, 4), nth)
                if code:
                    self.assertFalse(channel)
                lib.apkg_channel_free(channel)
            self.assertEqual((root / '.apkg/state').read_bytes(), before)

    def test_pre_cancelled_discovery_clears_output_without_transport(self):
        token = C.c_void_p()
        self.assertEqual(lib.apkg_cancel_create(C.byref(token)), 0)
        try:
            lib.apkg_cancel_request(token)
            observer = Observer(EVENT_FN(), None, token)
            out = C.c_void_p(1)
            code = lib.apkg_client_channel_open(b'/absent/root',
                b'http://127.0.0.1:1/unreachable', CHANNEL.encode(), 0,
                C.byref(observer), C.byref(out), None)
            self.assertEqual(code, 16)
            self.assertFalse(out)
        finally:
            lib.apkg_cancel_free(token)

    def test_actual_discovery_worker_copies_full_rows_and_owns_all_text(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            endpoint, root = d / 'channel', d / 'root'
            names = ['a' * 255, 'a' * 254 + 'b']
            version, revision = 'v' * 255, 18446744073709551615
            digest = fixture(endpoint, names, version=version, revision=revision)
            before = state(root, policy=seed())
            with serve(endpoint) as (url, server):
                result = subprocess.run([str(clients.WORKER), str(root), url,
                    '9', '7', CHANNEL], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                rows = [line.split('\t') for line in result.stdout.splitlines()]
                self.assertEqual({r[1] for r in rows}, set(names))
                for r in rows:
                    self.assertTrue(r[0].startswith(CHANNEL + '/' + digest + '/'))
                    self.assertEqual(r[2:], [version, str(revision), '1', '1'])
                self.assertFalse(any('/archives/' in p for p in server.requests))
            self.assertEqual((root / '.apkg/state').read_bytes(), before)

    def test_native_discovery_selection_drives_actual_install_worker(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            endpoint, _ = published(d)
            root = d / 'root'
            state(root, policy=seed())
            with serve(endpoint) as (url, server):
                result = subprocess.run([str(clients.WORKER), str(root), url,
                    '9', '7', CHANNEL], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                rows = [line.split('\t') for line in result.stdout.splitlines()]
                selected = next(r[0] for r in rows if r[1] == 'app')
                self.assertEqual(selected, selection(endpoint))
                self.assertFalse(any('/archives/' in p for p in server.requests))
                result = subprocess.run([str(clients.WORKER), str(root), url,
                    '1', '7', selected], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertTrue((root / 'app').is_dir())
                self.assertTrue((root / 'provider').is_dir())

    def test_native_discovery_unknown_facts_allocation_failure_and_cancel(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            endpoint, root = d / 'channel', d / 'root'
            fixture(endpoint, ['demo'], target='arm-aros-v11-native')
            before = state(root, policy=seed(authors=False))
            with serve(endpoint) as (url, server):
                for choice in (9, 10, 11, 9):
                    start = len(server.requests)
                    result = subprocess.run([str(clients.WORKER), str(root), url,
                        str(choice), '7', CHANNEL], capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    if choice == 9:
                        self.assertEqual(result.stdout.strip().split('\t')[1:], ['demo', '1.0', '1', '2', '0'])
                    else:
                        self.assertEqual(result.stdout, '')
                    if choice == 11:
                        self.assertEqual(len(server.requests), start)
            # Local discovery emits no transfer events: the first AllocVec
            # failure is the packed model itself, rather than a progress copy.
            result = subprocess.run([str(clients.WORKER), str(root), str(endpoint),
                '10', '7', CHANNEL], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            fixture(endpoint, [], sequence=2)
            result = subprocess.run([str(clients.WORKER), str(root), str(endpoint),
                '9', '7', CHANNEL], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(result.stdout, '')
            self.assertEqual((root / '.apkg/state').read_bytes(), before)
