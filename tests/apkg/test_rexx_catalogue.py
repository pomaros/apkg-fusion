# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
"""ARexx catalogue answers use signed common observations, offline."""
import json
import ctypes as C
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from support import AUTH, TARGET, descriptor, hashlib, sign, state, wire, lib, open_ctx
from test_channel import CHANNEL, CP, CS, catalog, entry, seed, read_channel
from test_client import WORKER


def offer(name, version='1.0', target=TARGET, authority=AUTH):
    body = json.loads(descriptor(name=name, target=target))['body']
    body.update(version=version, authority=authority)
    return entry(wire('descriptor', body))


def publish(endpoint, entries):
    data = catalog(entries)
    digest = hashlib.sha256(data).hexdigest()
    folder = endpoint / 'snapshots' / digest
    folder.mkdir(parents=True)
    (folder / 'channel.json').write_bytes(data)
    (folder / 'channel.signature.json').write_bytes(sign(data, CP, CS, 'channel'))
    (endpoint / 'current').write_text(digest + '\n')


def query(root, endpoint, match='', change=None, mode=45, field=''):
    args = [str(WORKER), str(root), str(endpoint), str(mode),
            '1' if change else '0', CHANNEL, match]
    if mode == 56: args.append(field)
    process = subprocess.Popen(args,
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, text=True)
    if change:
        assert process.stdout.readline() == 'loaded\n'
        change()
    out, err = process.communicate('\n' if change else '', timeout=60)
    assert process.returncode == 0, out + err
    status, _, text = out.partition('\n')
    return int(status.split()[1]), text.rstrip('\n')


class RexxCatalogue(unittest.TestCase):
    def test_installed_info_keeps_precedence_over_catalogue_versions(self):
        from test_operations import install
        with tempfile.TemporaryDirectory() as tmp:
            d=Path(tmp); root=d/'root'; endpoint=d/'channel'
            state(root,policy=seed()); code,ctx=open_ctx(root,1); self.assertEqual(code,0)
            try: install(ctx,descriptor())
            finally: lib.apkg_close(ctx)
            publish(endpoint,[offer('demo','2.0'),offer('demo','3.0')])
            self.assertEqual(query(root,endpoint,'demo',mode=56,field='state'),(0,'installed'))
            self.assertEqual(query(root,endpoint,'demo',mode=56,field='version'),(0,'1.0'))

    def test_catalogue_info_preserves_presentation_full_revision_and_relations(self):
        with tempfile.TemporaryDirectory() as tmp:
            d=Path(tmp); root=d/'root'; endpoint=d/'channel'; state(root,policy=seed())
            body=json.loads(descriptor())['body']
            body['revision']=str(2**64-1)
            body['presentation']={'summary':'Crème "quoted"','category':''}
            publish(endpoint,[entry(wire('descriptor',body))])
            for field,expected in [('summary','Crème "quoted"'),('category',''),
                ('revision',str(2**64-1)),('size','13')]:
                self.assertEqual(query(root,endpoint,'demo',mode=56,field=field),(0,expected))

    def test_channel_info_allocation_failures_clear_output_and_text_owns_its_bytes(self):
        from test_operations import install
        lib.apkg_channel_info.argtypes=[C.c_void_p,C.c_uint64,C.c_char_p,C.POINTER(C.c_void_p),C.c_void_p]
        lib.apkg_channel_info.restype=C.c_int
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)/'root'; state(root,policy=seed())
            code,ctx=open_ctx(root,1); self.assertEqual(code,0)
            try: install(ctx,descriptor())
            finally: lib.apkg_close(ctx)
            code,ctx=open_ctx(root); self.assertEqual(code,0)
            code,ch=read_channel(ctx,catalog()); self.assertEqual(code,0)
            lib.apkg_close(ctx)
            seen=set()
            try:
                for fault in range(1,100):
                    out=C.c_void_p(1); lib.apkg_test_fail_alloc(fault)
                    code=lib.apkg_channel_info(ch,0,None,C.byref(out),None)
                    lib.apkg_test_fail_alloc(0); seen.add(code)
                    if code: self.assertEqual(code,4); self.assertFalse(out)
                    else: lib.apkg_client_text_free(out)
                self.assertEqual(seen,{0,4})
                # A channel entry has no installed-state knowledge, even here
                # where the root genuinely has this exact package installed.
                for field in (b'installedversion',b'installedrevision'):
                    out=C.c_void_p()
                    self.assertEqual(lib.apkg_channel_info(ch,0,field,C.byref(out),None),0)
                    self.assertEqual(C.string_at(out),b'unknown')
                    lib.apkg_client_text_free(out)
                out=C.c_void_p(1)
                self.assertEqual(lib.apkg_channel_info(ch,1,None,C.byref(out),None),5)
                self.assertFalse(out)
                self.assertEqual(lib.apkg_channel_info(ch,0,b'id',C.byref(out),None),0)
                lib.apkg_channel_free(ch); ch=None
                self.assertEqual(C.string_at(out),b'demo'); lib.apkg_client_text_free(out)
            finally:
                lib.apkg_test_fail_alloc(0); lib.apkg_channel_free(ch)

    def test_catalogue_info_reports_available_without_installed_or_runtime_claims(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp); root = d / 'root'; endpoint = d / 'channel'
            before = state(root,policy=seed()); name = 'n' * 255
            publish(endpoint,[offer(name)])
            code, text = query(root,endpoint,name,mode=56)
            self.assertEqual(code,0)
            fields = dict(line.split(' ',1) for line in text.splitlines())
            for field, expected in dict(id=name, state='available', installedversion='unknown',
                installedrevision='unknown', reason='not-applicable', adoption='not-applicable',
                rootid='unknown', snapshot='unknown', generation='unknown', rootstate='unknown',
                runtime='not-asserted', review='not-asserted', requirements='not-checked',
                payloadverification='not-checked', signature='verified', authortrust='accepted',
                compatibility='target-match', payloadbytes='6', files='1', directories='0').items():
                self.assertEqual(fields[field],expected,field)
            self.assertEqual(query(root,endpoint,name,mode=56,field='VERSION'),(0,'1.0'))
            self.assertEqual(query(root,endpoint,name,mode=56,field='invented'),(1,''))
            self.assertEqual(query(root,endpoint,'missing',mode=56),(5,''))
            self.assertEqual((root / '.apkg/state').read_bytes(),before)
            self.assertFalse((root / 'db').exists())

    def test_catalogue_info_refuses_ambiguous_versions_targets_and_authors(self):
        for variant in ('version','target','authority'):
            with self.subTest(variant=variant), tempfile.TemporaryDirectory() as tmp:
                d = Path(tmp); root = d / 'root'; endpoint = d / 'channel'
                state(root,policy=seed())
                other = offer('demo',**{'version':'2.0'} if variant=='version' else
                    {'target':'i386-aros-v0-native'} if variant=='target' else {'authority':'12'*16})
                publish(endpoint,[offer('demo'),other])
                self.assertEqual(query(root,endpoint,'demo',mode=56),(14,''))

    def test_catalogue_info_revalidates_roles_and_survives_offline_source(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp); root = d / 'root'; endpoint = d / 'channel'
            state(root,policy=seed()); publish(endpoint,[offer('demo')])
            self.assertEqual(query(root,endpoint,'demo',mode=56,field='state',
                change=lambda: shutil.rmtree(endpoint)),(0,'available'))
            publish(endpoint,[offer('demo')])
            self.assertEqual(query(root,endpoint,'demo',mode=56,
                change=lambda: state(root,2,policy=seed(channels=False))),(12,''))

    def test_catalogue_info_preserves_withdrawn_author_and_target_mismatch(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp); root = d / 'root'; endpoint = d / 'channel'
            state(root,policy=seed()); publish(endpoint,[offer('other',target='i386-aros-v0-native')])
            self.assertEqual(query(root,endpoint,'other',mode=56,field='compatibility'),(0,'target-mismatch'))
            self.assertEqual(query(root,endpoint,'other',mode=56,field='authortrust',
                change=lambda: state(root,2,policy=seed(authors=False))),(0,'rejected'))

    def test_current_target_names_deduplicate_and_filter_without_network_or_writes(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp); root = d / 'root'; endpoint = d / 'channel'
            before = state(root, policy=seed()); name = 'n' * 255
            publish(endpoint, [offer(name), offer(name, '2.0'), offer('other'),
                               offer('wrong-target', target='i386-aros-v0-native')])
            code, text = query(root, endpoint)
            self.assertEqual(code, 0); self.assertEqual(set(text.split()), {name, 'other'})
            self.assertEqual(query(root, endpoint, 'missing'), (0, ''))
            self.assertEqual(query(root, endpoint, 'nnn', lambda: shutil.rmtree(endpoint)), (0, name))
            self.assertEqual((root / '.apkg/state').read_bytes(), before)
            self.assertEqual(sorted(p.name for p in d.iterdir()), ['root'])

    def test_current_channel_trust_is_rechecked_for_retained_observation(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp); root = d / 'root'; endpoint = d / 'channel'
            state(root, policy=seed()); publish(endpoint, [offer('demo')])
            self.assertEqual(query(root, endpoint, change=lambda: state(root, 2, policy=seed(channels=False))), (12, ''))

    def test_different_authors_cannot_be_collapsed_into_one_unqualified_name(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp); root = d / 'root'; endpoint = d / 'channel'
            state(root, policy=seed())
            publish(endpoint, [offer('same'), offer('same', authority='12' * 16), offer('unique')])
            self.assertEqual(query(root, endpoint), (14, ''))
            self.assertEqual(query(root, endpoint, 'unique'), (0, 'unique'))

    def test_oversized_answer_refuses_whole_result_and_match_can_narrow_it(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp); root = d / 'root'; endpoint = d / 'channel'
            state(root, policy=seed())
            names = ['p%03d-' % i + 'x' * 250 for i in range(260)]
            publish(endpoint, [offer(name) for name in names])
            self.assertEqual(query(root, endpoint), (3, ''))
            self.assertEqual(query(root, endpoint, 'p000-'), (0, names[0]))

if __name__ == '__main__':
    unittest.main()
