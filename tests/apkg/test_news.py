# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
"""Authenticated last-refresh comparisons survive caching without becoming source capabilities."""
import ctypes as C
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from support import *
import test_channel as channels
import test_recent as recent
import test_client as clients
import test_source_config as config
import test_channel_heads as heads

lib.apkg_channel_news.argtypes=[C.c_void_p,C.c_void_p,Text,Text,C.POINTER(C.c_void_p),C.c_void_p]
lib.apkg_channel_news.restype=C.c_int
lib.apkg_news_format.argtypes=[C.c_void_p,C.c_int,C.POINTER(C.c_void_p),C.c_void_p]
lib.apkg_news_format.restype=C.c_int
lib.apkg_news_free.argtypes=[C.c_void_p]
lib.apkg_source_config_save_pair.argtypes=[C.c_char_p,C.c_char_p,C.POINTER(ID),C.c_void_p,Text,Text,C.c_void_p]
lib.apkg_source_config_save_pair.restype=C.c_int


def document(items,sequence):
    body=json.loads(recent.catalog(items))['body'];body['sequence']=str(sequence)
    return wire('channel',body)


def put(endpoint,data):
    digest=hashlib.sha256(data).hexdigest();folder=endpoint/'snapshots'/digest
    folder.mkdir(parents=True,exist_ok=True)
    (folder/'channel.json').write_bytes(data)
    (folder/'channel.signature.json').write_bytes(sign(data,channels.CP,channels.CS,'channel'))
    (endpoint/'current').write_text(digest+'\n')


def captured(root,data):
    code,ctx=open_ctx(root);assert code==0
    code,s=snapshot(ctx);assert code==0
    code,ch=channels.read_channel(ctx,data);lib.apkg_close(ctx);assert code==0
    return s,ch


def query(s,ch,previous=None,signature=None,fault=0):
    previous=previous or b''
    signature=signature if signature is not None else sign(previous,channels.CP,channels.CS,'channel') if previous else b''
    a,b=C.create_string_buffer(previous),C.create_string_buffer(signature)
    out=C.c_void_p(123);lib.apkg_test_fail_alloc(fault)
    try:code=lib.apkg_channel_news(s,ch,Text(C.cast(a,C.c_void_p),len(previous)),Text(C.cast(b,C.c_void_p),len(signature)),C.byref(out),None)
    finally:lib.apkg_test_fail_alloc(0)
    if code:
        assert not out
        return code,None
    text=C.c_void_p();code=lib.apkg_news_format(out,1,C.byref(text),None)
    result=json.loads(C.string_at(text)) if text else None
    lib.apkg_client_text_free(text);lib.apkg_news_free(out)
    return code,result


def native(root,profile,endpoint=None,cancel=0,fault=0):
    args=[str(clients.WORKER),str(root),str(endpoint if endpoint else profile)]
    args+=['52',channels.CHANNEL,str(profile),str(cancel),str(fault)] if endpoint else ['53',str(fault)]
    result=subprocess.run(args,capture_output=True,text=True)
    assert result.returncode==0,result.stdout+result.stderr
    lines=result.stdout.splitlines();code=int(lines[0].split()[1]);saved=int(lines[1].split()[1]);summary=None;rows=[]
    for line in lines[2:]:
        fields=line.split('\t')
        if fields[0]=='summary':summary=dict(zip(('has_previous','count','applications','builds','updated','content_changed','recent','unknown_dates','indeterminate'),map(int,fields[1:])))
        if fields[0]=='change':
            rows.append(dict(kind=fields[1],authority=fields[2],name=fields[3],target=fields[4],version=fields[5],revision=fields[6],
                previous_version=fields[7] or None,previous_revision=fields[8] if fields[7] else None,
                earlier_publication=None if fields[9]=='-1' else fields[9]=='1',descriptor_sha256=fields[10],
                direction=('older' if fields[11]=='-1' else 'newer') if fields[1]=='updated' else None))
    return code,saved,summary,rows


class News(unittest.TestCase):
    def tearDown(self):lib.apkg_test_fail_alloc(0)

    def test_first_observation_is_recent_fallback_and_identical_heads_have_no_delta(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary)/'root';state(root,policy=channels.seed())
            data=document([recent.record('demo','2020-01-01','2020-01-01')],1)
            s,ch=captured(root,data)
            code,result=query(s,ch)
            self.assertEqual(code,0);self.assertFalse(result['has_previous'])
            self.assertIsNone(result['previous_sequence']);self.assertEqual(result['applications'],'0')
            self.assertEqual(result['recent'],'1');self.assertEqual(result['entries'][0]['kind'],'recent-publication')
            self.assertIsNone(result['entries'][0]['previous_revision']);self.assertIsNone(result['entries'][0]['earlier_publication'])
            code,result=query(s,ch,data)
            self.assertEqual(code,0);self.assertTrue(result['has_previous']);self.assertEqual(result['entries'],[])
            lib.apkg_snapshot_free(s);lib.apkg_channel_free(ch)

    def test_new_apps_target_builds_versions_and_same_release_changes_are_distinct(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary)/'root';state(root,policy=channels.seed())
            old=document([recent.record('same'),recent.record('upgrade'),recent.record('changed'),
                recent.record('port',target='aarch64-aros-v1-native')],1)
            changed=recent.record('changed');body=changed['descriptor']['body'];body['presentation']={'summary':'changed content'}
            changed['signature']=json.loads(sign(wire('descriptor',body)))
            new=document([recent.record('same'),recent.record('upgrade',version='2.0',revision='18446744073709551615'),changed,
                recent.record('port'),recent.record('added','2020-01-01','2020-01-01')],2)
            s,ch=captured(root,new);code,result=query(s,ch,old)
            self.assertEqual(code,0)
            self.assertEqual([result[k] for k in ('applications','builds','updated','content_changed')],['1']*4)
            rows={r['name']:r for r in result['entries']}
            self.assertEqual(rows['added']['kind'],'new-application');self.assertTrue(rows['added']['earlier_publication'])
            self.assertEqual(rows['port']['kind'],'new-build');self.assertIsNone(rows['port']['earlier_publication'])
            self.assertEqual(rows['upgrade']['previous_version'],'1.0');self.assertEqual(rows['upgrade']['revision'],'18446744073709551615')
            self.assertEqual(rows['upgrade']['direction'],'newer');self.assertIsNone(rows['changed']['direction'])
            self.assertEqual(rows['changed']['kind'],'same-release-change')
            lib.apkg_snapshot_free(s);lib.apkg_channel_free(ch)

    def test_tampering_untrusted_keys_forks_future_baselines_and_foreign_roots_refuse(self):
        with tempfile.TemporaryDirectory() as temporary:
            d=Path(temporary);root=d/'root';state(root,policy=channels.seed())
            old=document([recent.record('demo')],1);new=document([recent.record('demo',version='2.0')],2)
            s,ch=captured(root,new)
            self.assertEqual(query(s,ch,old.replace(b'1.0',b'9.0'),sign(old,channels.CP,channels.CS,'channel'))[0],10)
            self.assertEqual(query(s,ch,document([],2))[0],14)
            self.assertEqual(query(s,ch,document([],3))[0],8)
            pk,sk=keypair(bytes([7]*32))
            self.assertEqual(query(s,ch,old,sign(old,pk,sk,'channel'))[0],12)
            other=d/'other';state(other,policy=channels.seed());foreign,unused=captured(other,new)
            self.assertEqual(query(foreign,ch,old)[0],8)
            lib.apkg_snapshot_free(foreign);lib.apkg_channel_free(unused);lib.apkg_snapshot_free(s);lib.apkg_channel_free(ch)

    def test_indeterminate_groups_are_not_guessed_and_catalogue_downgrades_are_observations(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary)/'root';state(root,policy=channels.seed())
            old=document([recent.record('ambiguous',version='releaseX'),recent.record('down',version='2.0')],1)
            new=document([recent.record('ambiguous',version='releaseY'),recent.record('down',version='1.0')],2)
            s,ch=captured(root,new);code,result=query(s,ch,old)
            self.assertEqual(code,0);self.assertEqual(result['indeterminate'],'1')
            self.assertEqual([r['name'] for r in result['entries']],['down']);self.assertEqual(result['entries'][0]['direction'],'older')
            lib.apkg_snapshot_free(s);lib.apkg_channel_free(ch)

    def test_profile_refresh_persists_pairs_and_native_read_matches_cli_after_restart(self):
        with tempfile.TemporaryDirectory() as temporary:
            d=Path(temporary);root=d/'root';state(root,policy=channels.seed());endpoint=d/'channel';profile=d/'profile'
            old=document([recent.record('demo','2020-01-01','2020-01-01')],1)
            new=document([recent.record('demo','2020-01-01','2020-01-01',version='2.0'),recent.record('added')],2)
            put(endpoint,old)
            self.assertEqual(clients.cli('source-cache',root,profile,endpoint,channels.CHANNEL).returncode,0)
            self.assertEqual(profile.read_bytes()[12],1)
            put(endpoint,new);code,saved,summary,rows=native(root,profile,endpoint)
            self.assertEqual((code,saved),(0,1));self.assertEqual(profile.read_bytes()[12],2)
            before=(root/'.apkg/state').read_bytes();saved_bytes=profile.read_bytes();shutil.rmtree(endpoint)
            result=clients.cli('source-news',root,profile,'--json');self.assertEqual(result.returncode,0,result.stderr)
            report=json.loads(result.stdout);self.assertTrue(report['has_previous'])
            code,saved,summary,rows=native(root,profile)
            self.assertEqual((code,saved),(0,0))
            for key in ('applications','builds','updated','content_changed','recent','unknown_dates','indeterminate'):
                self.assertEqual(str(summary[key]),report[key])
            for actual,expected in zip(rows,report['entries']):
                for key,value in actual.items():self.assertEqual(value,expected[key])
            self.assertEqual(len(rows),len(report['entries']))
            self.assertEqual(profile.read_bytes(),saved_bytes);self.assertEqual((root/'.apkg/state').read_bytes(),before)

    def test_real_unchanged_refresh_clears_delta_but_saving_a_profile_keeps_it(self):
        with tempfile.TemporaryDirectory() as temporary:
            d=Path(temporary);root=d/'root';state(root,policy=channels.seed());endpoint=d/'channel';profile=d/'profile'
            put(endpoint,document([recent.record('demo')],1))
            self.assertEqual(clients.cli('source-cache',root,profile,endpoint,channels.CHANNEL).returncode,0)
            put(endpoint,document([recent.record('demo',version='2.0')],2))
            self.assertEqual(clients.cli('source-cache',root,profile,endpoint,channels.CHANNEL).returncode,0)
            self.assertEqual(json.loads(clients.cli('source-news',root,profile,'--json').stdout)['updated'],'1')
            saved=d/'saved-as'
            result=subprocess.run([str(clients.WORKER),str(root),str(profile),'53','0',str(saved)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            self.assertEqual(saved.read_bytes(),profile.read_bytes())
            self.assertEqual(json.loads(clients.cli('source-news',root,saved,'--json').stdout)['updated'],'1')
            self.assertEqual(clients.cli('source-cache',root,profile,endpoint,channels.CHANNEL).returncode,0)
            report=json.loads(clients.cli('source-news',root,profile,'--json').stdout)
            self.assertTrue(report['has_previous']);self.assertEqual(report['entries'],[])

    def test_history_below_replay_floor_is_readable_only_as_comparison_evidence(self):
        with tempfile.TemporaryDirectory() as temporary:
            d=Path(temporary);root=d/'root';state(root,policy=channels.seed());endpoint=d/'channel';old_profile=d/'old';profile=d/'current'
            old=document([recent.record('demo')],1);new=document([recent.record('demo',version='2.0')],2)
            put(endpoint,old);self.assertEqual(clients.cli('source-cache',root,profile,endpoint,channels.CHANNEL).returncode,0)
            shutil.copyfile(profile,old_profile);put(endpoint,new)
            self.assertEqual(clients.cli('source-cache',root,profile,endpoint,channels.CHANNEL).returncode,0)
            code,ctx=open_ctx(root,1);self.assertEqual(code,0);self.assertEqual(heads.accept(ctx,new),(0,1));lib.apkg_close(ctx)
            before=(root/'.apkg/state').read_bytes()
            result=clients.cli('source-news',root,profile,'--json');self.assertEqual(result.returncode,0,result.stderr)
            result=subprocess.run([str(clients.WORKER),str(root),str(endpoint),'54',str(old_profile),channels.CHANNEL],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            self.assertEqual((root/'.apkg/state').read_bytes(),before)

    def test_bad_previous_signature_is_not_a_false_first_observation_or_zero_delta(self):
        with tempfile.TemporaryDirectory() as temporary:
            d=Path(temporary);root=d/'root';state(root,policy=channels.seed());endpoint=d/'channel';profile=d/'profile'
            old=document([recent.record('demo')],1);new=document([recent.record('demo',version='2.0')],2)
            put(endpoint,old);self.assertEqual(clients.cli('source-cache',root,profile,endpoint,channels.CHANNEL).returncode,0)
            original=profile.read_bytes();profile.write_bytes(original.replace(b'1.0',b'9.0'))
            put(endpoint,new);corrupt=profile.read_bytes()
            self.assertEqual(clients.cli('source-cache',root,profile,endpoint,channels.CHANNEL).returncode,10)
            self.assertEqual(profile.read_bytes(),corrupt)
            profile.write_bytes(original)
            self.assertEqual(clients.cli('source-cache',root,profile,endpoint,channels.CHANNEL).returncode,0)
            good=profile.read_bytes();profile.write_bytes(good.replace(b'1.0',b'9.0'))
            self.assertEqual(clients.cli('source-news',root,profile,'--json').returncode,10)
            self.assertEqual(native(root,profile)[0],10)
            self.assertEqual(clients.cli('source-outdated',root,profile).returncode,0)

    def test_allocation_failures_clear_results_and_cancel_before_refresh_keeps_profile(self):
        with tempfile.TemporaryDirectory() as temporary:
            d=Path(temporary);root=d/'root';state(root,policy=channels.seed());endpoint=d/'channel';profile=d/'profile'
            old=document([recent.record('demo')],1);new=document([recent.record('demo',version='2.0')],2)
            s,ch=captured(root,new)
            for fault in range(1,351):self.assertIn(query(s,ch,old,fault=fault)[0],(0,4),fault)
            lib.apkg_snapshot_free(s);lib.apkg_channel_free(ch)
            put(endpoint,old);self.assertEqual(clients.cli('source-cache',root,profile,endpoint,channels.CHANNEL).returncode,0)
            before=profile.read_bytes();put(endpoint,new)
            code,saved,summary,rows=native(root,profile,endpoint,cancel=1)
            self.assertNotEqual(code,0);self.assertEqual(saved,0);self.assertEqual(profile.read_bytes(),before)
            code,saved,summary,rows=native(root,profile,endpoint,fault=1)
            self.assertEqual((code,saved),(4,1))
            self.assertEqual(json.loads(clients.cli('source-news',root,profile,'--json').stdout)['updated'],'1')

    def test_version_two_framing_refuses_truncation_extra_bytes_and_oversized_lengths(self):
        with tempfile.TemporaryDirectory() as temporary:
            d=Path(temporary);root=d/'root';state(root,policy=channels.seed());endpoint=d/'channel';profile=d/'profile'
            put(endpoint,document([recent.record('demo')],1))
            self.assertEqual(clients.cli('source-cache',root,profile,endpoint,channels.CHANNEL).returncode,0)
            put(endpoint,document([recent.record('demo',version='2.0')],2))
            self.assertEqual(clients.cli('source-cache',root,profile,endpoint,channels.CHANNEL).returncode,0)
            good=profile.read_bytes();self.assertEqual(good[12],2)
            bad=[good[:n] for n in range(49)]+[good+b'extra',good[:12]+b'\x03'+good[13:]]
            for offset in (29,33,37,41,45):bad.append(good[:offset]+b'\xff'*4+good[offset+4:])
            for raw in bad:
                profile.write_bytes(raw);self.assertEqual(config.read(profile)[0],1)
            profile.write_bytes(good)
            self.assertEqual(clients.cli('source-save',profile,endpoint,channels.CHANNEL).returncode,0)
            self.assertEqual(profile.read_bytes()[12],1)
            self.assertEqual(clients.cli('source-news',root,profile,'--json').returncode,5)
