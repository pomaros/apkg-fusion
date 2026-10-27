# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
"""Recent publication facts are signed, target-aware, bounded and never inferred."""
import ctypes as C
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from support import *
import test_channel as channels
import test_client as clients
import test_file_channel as files
import test_library_browser as library
import test_author as author

class Options(C.Structure):
    _fields_ = [('builds', C.c_int), ('limit', C.c_uint64), ('since', Text)]
class RecentEntry(C.Structure):
    _fields_ = [('index', C.c_uint64), ('kind', C.c_int), ('date', Text), ('package', C.c_void_p)]
class RecentView(C.Structure):
    _fields_ = [('count', C.c_uint64), ('unknown', C.c_uint64), ('indeterminate', C.c_uint64),
                ('entries', C.POINTER(RecentEntry))]
lib.apkg_channel_recent.argtypes = [C.c_void_p, C.POINTER(Options), C.POINTER(C.c_void_p), C.c_void_p]
lib.apkg_channel_recent.restype = C.c_int
lib.apkg_recent_get.argtypes = [C.c_void_p]
lib.apkg_recent_get.restype = C.POINTER(RecentView)
lib.apkg_recent_free.argtypes = [C.c_void_p]
lib.apkg_recent_format.argtypes = [C.c_void_p, C.c_int, C.POINTER(C.c_void_p), C.c_void_p]
lib.apkg_recent_format.restype = C.c_int


def record(name, app=None, first=None, version='1.0', revision='1', target=TARGET):
    body = json.loads(descriptor(name=name, target=target))['body']
    body.update(version=version, revision=revision)
    item = channels.entry(wire('descriptor', body))
    publication = {}
    if app is not None: publication['app_first_added'] = app
    if first is not None: publication['first_added'] = first
    if publication: item['publication'] = publication
    return item


def catalog(items, day='2026-10-27'):
    items = sorted(items, key=lambda item: hashlib.sha256(wire('descriptor', item['descriptor']['body'])).digest())
    body = dict(entries=items, id=channels.CHANNEL, sequence='1')
    if day is not None: body['published_on'] = day
    return wire('channel', body)


def endpoint_at(directory, data):
    endpoint = directory / 'channel'
    digest = hashlib.sha256(data).hexdigest()
    folder = endpoint / 'snapshots' / digest
    folder.mkdir(parents=True)
    (folder / 'channel.json').write_bytes(data)
    (folder / 'channel.signature.json').write_bytes(sign(data, channels.CP, channels.CS, 'channel'))
    (endpoint / 'current').write_text(digest + '\n')
    return endpoint


def query(channel, builds=0, limit=25, since='', fault=0):
    data = since.encode()
    options = Options(builds, limit, Text(C.cast(C.c_char_p(data), C.c_void_p), len(data)))
    out = C.c_void_p(123)
    lib.apkg_test_fail_alloc(fault)
    try:
        code = lib.apkg_channel_recent(channel, C.byref(options), C.byref(out), None)
    finally:
        lib.apkg_test_fail_alloc(0)
    if code:
        assert not out
        return code, None
    text = C.c_void_p()
    code = lib.apkg_recent_format(out, 1, C.byref(text), None)
    result = json.loads(C.string_at(text)) if text else None
    lib.apkg_client_text_free(text)
    lib.apkg_recent_free(out)
    return code, result


class Recent(unittest.TestCase):
    def tearDown(self): lib.apkg_test_fail_alloc(0)

    def opened(self, root, data):
        state(root, policy=channels.seed())
        code, ctx = open_ctx(root)
        self.assertEqual(code, 0)
        code, channel = channels.read_channel(ctx, data)
        lib.apkg_close(ctx)
        self.assertEqual(code, 0)
        return channel

    def test_dates_targets_latest_builds_since_and_limits(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / 'root'
            data = catalog([record('old', '2020-01-01', '2020-01-01'),
                record('new', '2026-01-01', '2026-01-01'),
                record('new', '2026-01-01', '2026-01-01', version='2.0', revision='18446744073709551615'),
                record('port', '2019-01-01', '2026-09-01'), record('unknown'),
                record('foreign', '2026-10-01', '2026-10-01', target='aarch64-aros-v1-native')])
            channel = self.opened(root, data)
            before = (root / '.apkg/state').read_bytes()
            code, result = query(channel)
            self.assertEqual(code, 0)
            self.assertEqual([r['name'] for r in result['entries']], ['new', 'old'])
            self.assertEqual(result['entries'][0]['version'], '2.0')
            self.assertEqual(result['entries'][0]['revision'], '18446744073709551615')
            self.assertEqual(result['unknown_dates'], '1')
            self.assertEqual(result['freshness'], 'unknown')
            self.assertEqual(result['date_source'], 'channel-publisher')
            code, result = query(channel, builds=1, since='2026-01-01', limit=0)
            self.assertEqual([r['name'] for r in result['entries']], ['port', 'new'])
            self.assertEqual(result['entries'][0]['kind'], 'new-build')
            self.assertEqual(len(query(channel, builds=1, limit=1)[1]['entries']), 1)
            self.assertEqual(query(channel, since='2026-10-01')[1]['entries'], [])
            self.assertEqual((root / '.apkg/state').read_bytes(), before)
            lib.apkg_channel_free(channel)

    def test_unknown_versions_and_missing_dates_are_reported_without_guessing(self):
        with tempfile.TemporaryDirectory() as temporary:
            data = catalog([record('ambiguous', '2020-01-01', version='releaseX'),
                record('ambiguous', '2020-01-01', version='releaseY'), record('missing'),
                record('single', '2020-02-01', version='releaseX')], day=None)
            channel = self.opened(Path(temporary) / 'root', data)
            code, result = query(channel)
            self.assertEqual(code, 0)
            self.assertEqual(result['indeterminate'], '1')
            self.assertEqual(result['unknown_dates'], '1')
            self.assertEqual([r['name'] for r in result['entries']], ['single'])
            lib.apkg_channel_free(channel)

    def test_calendar_validation_unknown_fields_and_inconsistent_dates_refuse(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / 'root'; state(root, policy=channels.seed())
            code, ctx = open_ctx(root); self.assertEqual(code, 0)
            for day in ('', '2026-2-01', '2026-02-29', '1900-02-29', '0000-01-01',
                        '2026-04-31', '2026-13-01', '2026-01-00', '2026-01-01T00:00:00Z'):
                with self.subTest(day=day):
                    self.assertEqual(channels.read_channel(ctx, catalog([record('demo', day)], day=None))[0], 2)
                    self.assertEqual(channels.read_channel(ctx, catalog([], day=day))[0], 2)
            for day in ('2000-02-29', '2024-02-29', '0001-01-01'):
                code, channel = channels.read_channel(ctx, catalog([record('demo', day, day)]))
                self.assertEqual(code, 0); lib.apkg_channel_free(channel)
            bad = record('demo', '2026-10-20', '2026-10-19')
            self.assertEqual(channels.read_channel(ctx, catalog([bad]))[0], 2)
            self.assertEqual(channels.read_channel(ctx, catalog([record('demo', '2026-10-20')], day='2026-10-19'))[0], 2)
            bad = record('demo'); bad['publication'] = {'review': 'passed'}
            self.assertEqual(channels.read_channel(ctx, catalog([bad]))[0], 2)
            lib.apkg_close(ctx)

    def test_date_tampering_requires_a_new_channel_signature(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / 'root'; state(root, policy=channels.seed())
            code, ctx = open_ctx(root); self.assertEqual(code, 0)
            data = catalog([record('demo', '2020-01-01', '2020-01-01')])
            signature = sign(data, channels.CP, channels.CS, 'channel')
            changed = data.replace(b'2020-01-01', b'2021-01-01')
            self.assertEqual(channels.read_channel(ctx, changed, signature)[0], 10)
            code, channel = channels.read_channel(ctx, changed)
            self.assertEqual(code, 0); lib.apkg_channel_free(channel)
            lib.apkg_close(ctx)

    def test_allocation_failures_options_and_output_ownership(self):
        with tempfile.TemporaryDirectory() as temporary:
            channel = self.opened(Path(temporary) / 'root', catalog([record('demo', '2020-01-01')]))
            for fault in range(1, 50): self.assertIn(query(channel, fault=fault)[0], (0, 4))
            for args, expected in [({'builds':2},1), ({'limit':4097},3), ({'since':'2020-02-30'},1)]:
                self.assertEqual(query(channel, **args), (expected, None))
            out = C.c_void_p(123)
            self.assertEqual(lib.apkg_channel_recent(None, None, C.byref(out), None), 1)
            self.assertFalse(out)
            options = Options(0,25,Text(None,0)); recent = C.c_void_p()
            self.assertEqual(lib.apkg_channel_recent(channel,C.byref(options),C.byref(recent),None),0)
            for fault in range(1,20):
                text=C.c_void_p(123);lib.apkg_test_fail_alloc(fault)
                try: code=lib.apkg_recent_format(recent,1,C.byref(text),None)
                finally: lib.apkg_test_fail_alloc(0)
                self.assertIn(code,(0,4))
                if code:self.assertFalse(text)
                lib.apkg_client_text_free(text)
            lib.apkg_recent_free(recent);lib.apkg_channel_free(channel)

    def test_cli_worker_offline_profile_and_desktop_order_agree(self):
        with tempfile.TemporaryDirectory() as temporary:
            d=Path(temporary);root=d/'root';state(root,policy=channels.seed())
            data=catalog([record('old','2020-01-01','2020-01-01'),record('new','2026-01-01','2026-01-01'),record('unknown')])
            endpoint=endpoint_at(d,data)
            result=clients.cli('recent',root,endpoint,channels.CHANNEL,'--json')
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            expected=json.loads(result.stdout)
            worker=subprocess.run([str(clients.WORKER),str(root),str(endpoint),'51','0',channels.CHANNEL,'25','','1'],capture_output=True,text=True)
            self.assertEqual(worker.returncode,0,worker.stdout+worker.stderr)
            self.assertEqual(json.loads(worker.stdout.removeprefix('status 0\n')),expected)
            code,categories,rows=library.query(root,endpoint,4)
            self.assertEqual(code,0);self.assertEqual([r[1] for r in rows],['new','old'])
            copied=subprocess.run([str(clients.WORKER),str(root),str(endpoint),'50','4',channels.CHANNEL,'','','0'],capture_output=True,text=True)
            self.assertEqual(copied.returncode,0,copied.stdout+copied.stderr)
            self.assertIn('reported-on\t2026-10-27\n',copied.stdout)
            self.assertIn('news\tnew\t2026-01-01\t1\n',copied.stdout)
            self.assertIn('news\told\t2020-01-01\t2\n',copied.stdout)
            profile=d/'profile';self.assertEqual(clients.cli('source-cache',root,profile,endpoint,channels.CHANNEL).returncode,0)
            shutil.rmtree(endpoint)
            offline=clients.cli('source-recent',root,profile,'--json')
            self.assertEqual(offline.returncode,0,offline.stderr);self.assertEqual(json.loads(offline.stdout),expected)
            for extra,code in [(('--limit','-1'),1),(('--limit','4097'),3),(('--since','2026-02-29'),1),(('--unknown',),1)]:
                self.assertEqual(clients.cli('source-recent',root,profile,*extra).returncode,code)

    def test_real_publisher_preserves_first_dates_and_idempotence(self):
        with tempfile.TemporaryDirectory() as temporary:
            d=Path(temporary);key=author.key_fixture(d/'key');ckey=files.channel_key(d/'channel-key');endpoint=d/'channel'
            archives=[]
            for index,version in enumerate(('1.0','2.0')):
                folder=d/('source'+str(index));author.author_fixture(folder,version=version)
                archive=d/('version'+str(index)+'.pkg');result=author.build(folder,archive,key)
                self.assertEqual(result.returncode,0,result.stderr);archives.append(archive)
            def publish(archive,day):
                return subprocess.run(files.command(archive,endpoint,ckey)+['--published-on',day],capture_output=True,text=True)
            self.assertEqual(publish(archives[0],'2020-01-01').returncode,0)
            before=files.head(endpoint)
            result=publish(archives[0],'2026-01-01');self.assertEqual(result.returncode,0,result.stderr)
            self.assertFalse(json.loads(result.stdout)['changed']);self.assertEqual(files.head(endpoint),before)
            result=publish(archives[1],'2026-01-01');self.assertEqual(result.returncode,0,result.stderr)
            document=files.snapshot_document(endpoint)
            self.assertEqual(document['body']['published_on'],'2026-01-01')
            for item in document['body']['entries']:
                self.assertEqual(item['publication'],{'first_added':'2020-01-01','app_first_added':'2020-01-01'})
            source=d/'arm';manifest=author.author_fixture(source,version='2.0')
            manifest.write_text(manifest.read_text().replace('arch = "x86_64"','arch = "aarch64"').replace('abi = "v11"','abi = "v1"'))
            arm=d/'arm.pkg';result=author.build(source,arm,key)
            self.assertEqual(result.returncode,0,result.stderr)
            before=files.head(endpoint)
            result=publish(arm,'2025-01-01');self.assertNotEqual(result.returncode,0)
            self.assertEqual(files.head(endpoint),before)
            result=publish(arm,'2026-09-01');self.assertEqual(result.returncode,0,result.stderr)
            arm_entry=next(item for item in files.snapshot_document(endpoint)['body']['entries']
                if item['descriptor']['body']['target']=='aarch64-aros-v1-native')
            self.assertEqual(arm_entry['publication'],{'first_added':'2026-09-01','app_first_added':'2020-01-01'})
            before=files.head(endpoint)
            self.assertNotEqual(publish(archives[1],'2026-02-30').returncode,0)
            self.assertEqual(files.head(endpoint),before)

    def test_publisher_does_not_backfill_unknown_legacy_publication_dates(self):
        with tempfile.TemporaryDirectory() as temporary:
            d=Path(temporary);key=author.key_fixture(d/'key');ckey=files.channel_key(d/'channel-key');endpoint=d/'channel'
            artifacts=[]
            for index,version in enumerate(('1.0','2.0')):
                source=d/('source'+str(index));author.author_fixture(source,version=version)
                archive=d/('archive'+str(index)+'.pkg');result=author.build(source,archive,key)
                self.assertEqual(result.returncode,0,result.stderr);artifacts.append(archive)
            result=files.publish(artifacts[0],endpoint,ckey);self.assertEqual(result.returncode,0,result.stderr)
            document=files.snapshot_document(endpoint);document['body'].pop('published_on')
            for item in document['body']['entries']:item.pop('publication',None)
            data=wire('channel',document['body']);digest=hashlib.sha256(data).hexdigest()
            folder=endpoint/'snapshots'/digest;folder.mkdir()
            (folder/'channel.json').write_bytes(data)
            (folder/'channel.signature.json').write_bytes(sign(data,channels.CP,channels.CS,'channel'))
            (endpoint/'current').write_text(digest+'\n')
            result=files.publish(artifacts[1],endpoint,ckey);self.assertEqual(result.returncode,0,result.stderr)
            for item in files.snapshot_document(endpoint)['body']['entries']:
                self.assertNotIn('publication',item)
