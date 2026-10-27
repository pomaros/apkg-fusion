# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
"""Captured candidate comparison shares operation ordering without readiness claims."""
import ctypes as C
import subprocess
import tempfile
import unittest
from support import *
from test_channel import CHANNEL, CP, CS, catalog, entry, seed, read_channel, ChannelRow
from test_operations import install, op
from test_inspection import info, inspect, formatted
from test_client import CLI, WORKER
from test_channel_clients import published, install_archive
from test_http_channel import serve

class Candidate(C.Structure):
    _fields_ = [('state', C.c_int), ('installed', C.c_void_p), ('readiness', C.c_int)]

lib.apkg_channel_compare.argtypes = [C.c_void_p, C.c_void_p, C.c_uint64, C.POINTER(Candidate), C.c_void_p]
lib.apkg_channel_compare.restype = C.c_int
lib.apkg_candidate_state_name.argtypes = [C.c_int]
lib.apkg_candidate_state_name.restype = C.c_char_p
lib.apkg_client_channel_refresh.argtypes = [C.c_char_p, C.c_char_p, C.c_char_p, C.c_uint64,
    C.c_void_p, C.POINTER(C.c_void_p), C.POINTER(C.c_void_p), C.c_void_p]
lib.apkg_client_channel_refresh.restype = C.c_int
lib.apkg_client_folder.argtypes = [C.c_char_p, C.c_char_p, C.POINTER(C.c_void_p), C.c_void_p]
lib.apkg_client_folder.restype = C.c_int
lib.apkg_client_run.argtypes = [C.c_char_p, C.c_char_p, C.c_void_p]
lib.apkg_client_run.restype = C.c_int


def desc(name='demo', version='1.0', revision=1, authority=AUTH, target=TARGET, content=b'hello\n'):
    body = json.loads(descriptor(name=name, target=target))['body']
    body.update(version=version, revision=str(revision), authority=authority)
    body['files'][0].update(data=content.hex(), sha256=hashlib.sha256(content).hexdigest(), size=str(len(content)))
    return wire('descriptor', body)


def source(endpoint, descriptors, sequence=1):
    data = catalog([entry(d) for d in descriptors], sequence)
    digest = hashlib.sha256(data).hexdigest()
    directory = endpoint / 'snapshots' / digest
    directory.mkdir(parents=True, exist_ok=True)
    (directory / 'channel.json').write_bytes(data)
    (directory / 'channel.signature.json').write_bytes(sign(data, CP, CS, 'channel'))
    (endpoint / 'current').write_text(digest + '\n')
    return digest


def refresh(root, endpoint):
    s, ch = C.c_void_p(), C.c_void_p()
    code = lib.apkg_client_channel_refresh(str(root).encode(), str(endpoint).encode(),
        CHANNEL.encode(), 0, None, C.byref(s), C.byref(ch), None)
    return code, s, ch


def compare(s, ch, index=0):
    view = Candidate()
    code = lib.apkg_channel_compare(s, ch, index, C.byref(view), None)
    return code, view


def states(s, ch, count):
    result = {}
    for i in range(count):
        row = C.POINTER(ChannelRow)()
        assert lib.apkg_channel_entry_at(ch, i, C.byref(row), None) == 0
        code, view = compare(s, ch, i)
        assert code == 0, code
        assert view.readiness == 0
        result[bytes(row.contents.descriptor_digest.bytes).hex()] = (lib.apkg_candidate_state_name(view.state).decode(), view)
    return result


def fixture(root):
    state(root, policy=seed())
    code, ctx = open_ctx(root, 1)
    assert code == 0
    try:
        for data in [desc(), desc('fork'), desc('opaque', 'opaque'), desc('pre', '1.0-test1'),
                desc('maxrev', revision=2**64-2), desc('z'*255)]:
            install(ctx, data)
    finally:
        lib.apkg_close(ctx)


class Updates(unittest.TestCase):
    def tearDown(self): lib.apkg_test_fail_alloc(0)

    def test_qualified_comparison_and_ambiguous_name_actions_never_choose_first_owner(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp); root = d / 'root'; endpoint = d / 'channel'
            fixture(root)
            body = json.loads((root / '.apkg/state').read_bytes())['body']
            foreign = desc(authority='ef'*16, version='9.0')
            body['installed'].insert(0, dict(descriptor=json.loads(foreign), signature=json.loads(sign(foreign))))
            body['policy']['body']['authorities'].append(dict(id='ef'*16,
                keys=[dict(algorithm='ed25519', public_key=PK.hex())]))
            before = wire('root', body); (root / '.apkg/state').write_bytes(before)
            source(endpoint, [desc(version='2.0')])
            code, s, ch = refresh(root, endpoint); self.assertEqual(code, 0)
            code, comparison = compare(s, ch)
            self.assertEqual((code, comparison.state), (0, 3))
            lib.apkg_snapshot_free(s); lib.apkg_channel_free(ch)
            report = subprocess.run([str(CLI), 'channel-updates', str(root), str(endpoint), CHANNEL], capture_output=True, text=True)
            self.assertEqual(report.returncode, 0, report.stdout + report.stderr)
            row = json.loads(report.stdout)['entries'][0]
            self.assertEqual((row['installed_authority'], row['installed_version']), (AUTH, '1.0'))
            self.assertEqual(info(root)[0], 14)
            folder = C.c_void_p()
            self.assertEqual(lib.apkg_client_folder(str(root).encode(), b'demo', C.byref(folder), None), 14)
            self.assertFalse(folder)
            self.assertEqual(lib.apkg_client_run(str(root).encode(), b'demo', None), 14)
            code, ctx = open_ctx(root, 1); self.assertEqual(code, 0)
            for operation in ('remove', 'repair'):
                code, plan = op(ctx, operation)
                self.assertEqual(code, 14); self.assertFalse(plan)
            code, snap = snapshot(ctx); self.assertEqual(code, 0)
            code, inspection = inspect(snap, 0); self.assertEqual(code, 0)
            self.assertEqual(formatted(inspection, 'authority'), (0, 'ef'*16))
            lib.apkg_inspection_free(inspection)
            data = desc(version='2.0'); code, pkg = package(ctx, data, sign(data)); self.assertEqual(code, 0)
            plan = C.c_void_p()
            self.assertEqual(lib.apkg_plan_upgrade(ctx, snap, pkg, C.byref(plan), None), 14)
            self.assertFalse(plan)
            lib.apkg_package_free(pkg); lib.apkg_snapshot_free(snap); lib.apkg_close(ctx)
            self.assertEqual((root / '.apkg/state').read_bytes(), before)
            self.assertFalse(Path(str(root) + '.apkg-transaction').exists())
            # A candidate with no exact qualified owner cannot pick either row.
            source(endpoint, [desc(authority='12'*16)], sequence=2)
            code, s, ch = refresh(root, endpoint); self.assertEqual(code, 0)
            code, comparison = compare(s, ch)
            self.assertEqual(code, 14); self.assertFalse(comparison.installed)
            lib.apkg_channel_free(ch); lib.apkg_snapshot_free(s)
            # Duplicate exact identities also refuse, even with identical bytes.
            body['installed'].append(body['installed'][1])
            (root / '.apkg/state').write_bytes(wire('root', body))
            source(endpoint, [desc(version='2.0')], sequence=3)
            code, s, ch = refresh(root, endpoint); self.assertEqual(code, 0)
            self.assertEqual(compare(s, ch)[0], 14)
            lib.apkg_snapshot_free(s); lib.apkg_channel_free(ch)

    def test_trusted_other_author_is_still_a_different_owner(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp); root = d / 'root'; endpoint = d / 'channel'
            fixture(root)
            body = json.loads((root / '.apkg/state').read_bytes())['body']
            body['policy']['body']['authorities'].append(dict(id='ef'*16,
                keys=[dict(algorithm='ed25519', public_key=PK.hex())]))
            (root / '.apkg/state').write_bytes(wire('root', body))
            source(endpoint, [desc(authority='ef'*16, version='2.0')])
            result = subprocess.run([str(CLI), 'channel-updates', str(root), str(endpoint), CHANNEL], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            row = json.loads(result.stdout)['entries'][0]
            self.assertEqual(row['author_trust'], 'accepted')
            self.assertEqual(row['comparison'], 'other-authority')
            self.assertEqual(row['authority'], 'ef'*16)
            self.assertEqual(row['installed_authority'], AUTH)
            self.assertEqual(row['installed_target'], TARGET)
            result = subprocess.run([str(WORKER), str(root), str(endpoint), '16', '7', CHANNEL], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            fields = result.stdout.splitlines()[1].split('\t')
            self.assertEqual(fields[3], 'other-authority')
            self.assertEqual(fields[7], '0')

    def test_ordering_exact_content_unknown_versions_revisions_and_full_identity(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp); root = d / 'root'; endpoint = d / 'channel'
            fixture(root)
            cases = [(desc(), 'current'), (desc(version='2.0'), 'newer'),
                (desc(version='0.9'), 'older'), (desc(revision=2), 'newer'),
                (desc('fork', content=b'changed'), 'content-conflict'),
                (desc(version='1.0.0'), 'content-conflict'),
                (desc(version='1.0.0', revision=2), 'newer'),
                (desc('opaque', 'opaque'), 'current'),
                (desc('opaque', 'opaque', 2), 'indeterminate'),
                (desc('pre', '1.0-rc1'), 'indeterminate'),
                (desc('pre', '1.0'), 'newer'),
                (desc('maxrev', revision=2**64-1), 'newer'),
                (desc(authority='ef'*16, version='2.0'), 'other-authority'),
                (desc(target='arm-aros-v11-native', version='2.0'), 'other-target'),
                (desc('a'*255, 'v'*255, 2**64-1), 'not-installed'),
                (desc('z'*255, '2.0'), 'newer'),
                (desc('z'*254+'y'), 'not-installed')]
            source(endpoint, [data for data, _ in cases])
            before = (root / '.apkg/state').read_bytes()
            code, s, ch = refresh(root, endpoint)
            self.assertEqual(code, 0)
            actual = states(s, ch, len(cases))
            for data, expected in cases:
                state_name, view = actual[hashlib.sha256(data).hexdigest()]
                self.assertEqual(state_name, expected)
                self.assertEqual(bool(view.installed), expected != 'not-installed')
            self.assertEqual(compare(s, ch, 999)[0], 5)
            self.assertEqual(compare(None, ch)[0], 1)
            lib.apkg_channel_free(ch); lib.apkg_snapshot_free(s)
            self.assertEqual((root / '.apkg/state').read_bytes(), before)
            self.assertFalse((endpoint / 'archives').exists())

    def test_cross_root_changed_policy_identity_and_replay_floors_refuse(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp); root = d / 'root'; endpoint = d / 'channel'
            fixture(root); source(endpoint, [desc(version='2.0')])
            before = (root / '.apkg/state').read_bytes()
            code, s, ch = refresh(root, endpoint); self.assertEqual(code, 0)
            other = d / 'clone'; other.mkdir(); (other / '.apkg').mkdir()
            (other / '.apkg/state').write_bytes(before)
            code, ctx = open_ctx(other); code, clone = snapshot(ctx)
            self.assertEqual(compare(clone, ch)[0], 8)
            lib.apkg_snapshot_free(clone); lib.apkg_close(ctx)
            base = json.loads(before)['body']
            for mutate, expected in [
                    (lambda b: b.update(policy=json.loads(seed(authors=False))), 8),
                    (lambda b: b.update(root_id='22'*16), 8),
                    (lambda b: b.update(channel_heads=[dict(id=CHANNEL, sequence='2', snapshot_sha256='ab'*32)]), 8),
                    (lambda b: b.update(channel_heads=[dict(id=CHANNEL, sequence='1', snapshot_sha256='ab'*32)]), 14)]:
                body = json.loads(json.dumps(base)); mutate(body)
                (root / '.apkg/state').write_bytes(wire('root', body))
                code, ctx = open_ctx(root); code, changed = snapshot(ctx)
                self.assertEqual(code, 0)
                self.assertEqual(compare(changed, ch)[0], expected)
                lib.apkg_snapshot_free(changed); lib.apkg_close(ctx)
            # The original captured observation remains immutable.
            self.assertEqual(compare(s, ch)[0], 0)
            lib.apkg_snapshot_free(s); lib.apkg_channel_free(ch)

    def test_cli_and_actual_discovery_worker_share_filters_and_exact_selections(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp); root = d / 'root'; endpoint = d / 'channel'
            fixture(root)
            candidates = [desc(), desc(version='2.0'), desc(version='0.9'),
                desc('fork', content=b'fork'), desc('opaque', 'opaque', 2),
                desc(target='arm-aros-v11-native'), desc(authority='ef'*16), desc('new')]
            source(endpoint, candidates)
            before = (root / '.apkg/state').read_bytes()
            with serve(endpoint) as (url, server):
                cli = subprocess.run([str(CLI), 'channel-updates', str(root), url, CHANNEL], capture_output=True, text=True)
                self.assertEqual(cli.returncode, 0, cli.stdout + cli.stderr)
                report = json.loads(cli.stdout)
                native = subprocess.run([str(WORKER), str(root), url, '16', '7', CHANNEL], capture_output=True, text=True)
                self.assertEqual(native.returncode, 0, native.stdout + native.stderr)
                lines = native.stdout.splitlines()
                self.assertEqual(lines[0], 'generation ' + report['root_generation'])
                rows = {r['selection']: r for r in report['entries']}
                for line in lines[1:]:
                    selected, name, version, comparison, old_version, old_revision, known, reviewable, mask = line.split('\t')
                    r = rows[selected]
                    self.assertEqual((name, version, comparison), (r['name'], r['version'], r['comparison']))
                    self.assertEqual(old_version, r['installed_version'] or '')
                    self.assertEqual(int(known), int(r['installed_revision'] is not None))
                    if int(known): self.assertEqual(old_revision, r['installed_revision'])
                    expected_review = r['author_trust'] == 'accepted' and r['target_matches'] and comparison in ('current', 'newer', 'not-installed')
                    self.assertEqual(int(reviewable), int(expected_review))
                    self.assertEqual(bool(int(mask) & 2), comparison in ('newer', 'content-conflict', 'indeterminate', 'other-authority', 'other-target'))
                    self.assertEqual(bool(int(mask) & 4), comparison == 'not-installed')
                    self.assertEqual(r['plan_readiness'], 'not-checked')
                self.assertEqual(len(lines)-1, len(rows))
                self.assertFalse(any('/archives/' in path for path in server.requests))
            self.assertEqual((root / '.apkg/state').read_bytes(), before)
            self.assertEqual(report['root_snapshot_sha256'], hashlib.sha256(before).hexdigest())

    def test_real_newer_selection_executes_one_common_plan_and_refreshes_to_current(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp); endpoint, archives = published(d); root = d / 'root'
            state(root, policy=seed()); code, ctx = open_ctx(root, 1)
            install_archive(ctx, archives['provider', '1.2']); lib.apkg_close(ctx)
            result = subprocess.run([str(CLI), 'channel-updates', str(root), str(endpoint), CHANNEL], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            report = json.loads(result.stdout)
            selected = next(r['selection'] for r in report['entries'] if r['name']=='provider' and r['comparison']=='newer')
            result = subprocess.run([str(WORKER), str(root), str(endpoint), '1', '7', selected], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(CLI), 'channel-updates', str(root), str(endpoint), CHANNEL], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            report = json.loads(result.stdout)
            versions = {r['version']: r['comparison'] for r in report['entries'] if r['name']=='provider'}
            self.assertEqual(versions, {'1.2':'older', '2.0':'current'})
            self.assertEqual(json.loads((root/'.apkg/state').read_bytes())['body']['channel_heads'][0]['sequence'], '4')

    def test_refresh_allocation_failures_clear_both_outputs_and_retry_without_writes(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp); root = d / 'root'; endpoint = d / 'channel'
            state(root, policy=seed()); source(endpoint, [desc()])
            before = (root / '.apkg/state').read_bytes()
            for nth in range(1, 241):
                lib.apkg_test_fail_alloc(nth)
                code, s, ch = refresh(root, endpoint)
                lib.apkg_test_fail_alloc(0)
                self.assertIn(code, (0, 4), nth)
                if code: self.assertFalse(s); self.assertFalse(ch)
                lib.apkg_snapshot_free(s); lib.apkg_channel_free(ch)
            self.assertEqual((root / '.apkg/state').read_bytes(), before)
