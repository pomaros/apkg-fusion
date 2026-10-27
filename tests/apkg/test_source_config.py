# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
import ctypes as C
import os
import shutil
import subprocess
import tempfile
import unittest
from support import *
from test_client import CLI, WORKER
from test_summary import fixture, summarize, formatted
from test_updates import refresh
from test_channel import CHANNEL, catalog
from test_channel_heads import accept
from test_http_channel import serve
from test_operations import op, run_plan

class ConfigView(C.Structure):
    _fields_=[('endpoint',Text),('id',ID),('observation',Text),('signature',Text)]
lib.apkg_source_config_save.argtypes=[C.c_char_p,C.c_char_p,C.POINTER(ID),C.c_void_p,C.c_void_p]
lib.apkg_source_config_save.restype=C.c_int
lib.apkg_source_config_read.argtypes=[C.c_char_p,C.POINTER(C.c_void_p),C.c_void_p]
lib.apkg_source_config_read.restype=C.c_int
lib.apkg_source_config_get.argtypes=[C.c_void_p]
lib.apkg_source_config_get.restype=C.POINTER(ConfigView)
lib.apkg_source_config_free.argtypes=[C.c_void_p]
lib.apkg_source_config_open.argtypes=[C.c_void_p,C.c_char_p,C.POINTER(C.c_void_p),C.POINTER(C.c_void_p),C.c_void_p]
lib.apkg_source_config_open.restype=C.c_int

def read(path):
    out=C.c_void_p(); code=lib.apkg_source_config_read(str(path).encode(),C.byref(out),None)
    if code: assert not out
    return code,out

def save(path,endpoint,channel=None,identity=CHANNEL):
    ident=ID(); ident.bytes[:]=bytes.fromhex(identity)
    return lib.apkg_source_config_save(str(path).encode(),str(endpoint).encode(),C.byref(ident),channel,None)

def command(*args):
    return subprocess.run([str(CLI),*map(str,args)],capture_output=True,text=True,timeout=30)

def offline(root,path): return command('source-outdated',root,path)

def worker(root,path,fault=None):
    return subprocess.run([str(WORKER),str(root),str(path),'24']+([str(fault)] if fault is not None else []),capture_output=True,text=True,timeout=30)

class SourceConfig(unittest.TestCase):
    def tearDown(self): lib.apkg_test_fail_alloc(0)

    def test_settings_only_private_roundtrip_and_explicit_cache_clear(self):
        with tempfile.TemporaryDirectory() as tmp:
            d=Path(tmp); root,endpoint=fixture(d); profile=d/'profile with spaces'
            self.assertEqual(save(profile,'channel with spaces/é'),0)
            code,c=read(profile); self.assertEqual(code,0)
            v=lib.apkg_source_config_get(c).contents
            self.assertEqual(raw(v.endpoint).decode(),'channel with spaces/é'); self.assertEqual(bytes(v.id.bytes),bytes.fromhex(CHANNEL))
            self.assertEqual(v.observation.size,0)
            s,ch=C.c_void_p(),C.c_void_p()
            self.assertEqual(lib.apkg_source_config_open(c,str(root).encode(),C.byref(s),C.byref(ch),None),5)
            self.assertFalse(s); self.assertFalse(ch); lib.apkg_source_config_free(c)
            self.assertEqual(profile.stat().st_mode & 0o777,0o600)
            for bad in ('','a\nb','a\x7fb','x'*4096): self.assertEqual(save(profile,bad),1)
            code,s,ch=refresh(root,endpoint); self.assertEqual(code,0)
            self.assertEqual(save(profile,endpoint,ch),0)
            before=profile.read_bytes(); self.assertEqual(save(profile,endpoint,ch,'ef'*16),14); self.assertEqual(profile.read_bytes(),before)
            code,c=read(profile); self.assertEqual(code,0)
            retained_s,retained_ch=C.c_void_p(),C.c_void_p()
            self.assertEqual(lib.apkg_source_config_open(c,str(root).encode(),C.byref(retained_s),C.byref(retained_ch),None),0)
            lib.apkg_source_config_free(c)
            code,summary=summarize(retained_s,retained_ch); self.assertEqual(code,0)
            lib.apkg_snapshot_free(retained_s); lib.apkg_channel_free(retained_ch)
            self.assertEqual(formatted(summary,'newer'),(0,'3')); lib.apkg_update_summary_free(summary)

            lib.apkg_snapshot_free(s); lib.apkg_channel_free(ch)
            result=command('source-save',profile,endpoint,CHANNEL); self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            code,c=read(profile); self.assertEqual(lib.apkg_source_config_get(c).contents.observation.size,0); lib.apkg_source_config_free(c)

    def test_http_cache_survives_processes_and_native_save_load_matches_cli_offline(self):
        with tempfile.TemporaryDirectory() as tmp:
            d=Path(tmp); root,endpoint=fixture(d); profile=d/'saved'; native=d/'native'
            before=(root/'.apkg/state').read_bytes()
            with serve(endpoint) as (url,server):
                result=command('source-cache',root,profile,url,CHANNEL); self.assertEqual(result.returncode,0,result.stdout+result.stderr)
                result=subprocess.run([str(WORKER),str(root),url,'25','7',CHANNEL,str(native)],capture_output=True,text=True,timeout=30)
                self.assertEqual(result.returncode,0,result.stdout+result.stderr); self.assertEqual(result.stdout,'status 0\n')
                self.assertEqual(profile.read_bytes(),native.read_bytes())
                requests=len(server.requests)
                result=offline(root,profile); self.assertEqual(result.returncode,0,result.stdout+result.stderr)
                native_result=worker(root,native); self.assertEqual(native_result.returncode,0,native_result.stdout+native_result.stderr)
                self.assertEqual(native_result.stdout,'status 0\n'+result.stdout)
                self.assertEqual(len(server.requests),requests)
            shutil.rmtree(endpoint)
            result=offline(root,profile); self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            self.assertIn('newer 3\n',result.stdout); self.assertIn('freshness unknown\n',result.stdout)
            self.assertEqual((root/'.apkg/state').read_bytes(),before)
            shown=command('source-show',profile); self.assertEqual(shown.returncode,0); self.assertIn('endpoint '+url+'\n',shown.stdout)

    def test_current_root_policy_replay_and_pending_state_are_revalidated_after_restart(self):
        for change,expected in [('remove',0),('author',0),('channel',12),('floor',8),('fork',14),('pending',9)]:
            with self.subTest(change=change),tempfile.TemporaryDirectory() as tmp:
                d=Path(tmp); root,endpoint=fixture(d); profile=d/'saved'
                result=command('source-cache',root,profile,endpoint,CHANNEL); self.assertEqual(result.returncode,0,result.stdout+result.stderr)
                saved=profile.read_bytes()
                if change in ('remove','floor','fork'):
                    code,ctx=open_ctx(root,1); self.assertEqual(code,0)
                    try:
                        if change=='remove':
                            code,p=op(ctx,'remove','blocked'); self.assertEqual(code,0); self.assertEqual(run_plan(ctx,p),(0,1)); lib.apkg_plan_free(p)
                        else: self.assertEqual(accept(ctx,catalog(entries=[],sequence=2 if change=='floor' else 1)),(0,1))
                    finally: lib.apkg_close(ctx)
                elif change=='pending': Path(str(root)+'.apkg-transaction').mkdir()
                else:
                    body=json.loads((root/'.apkg/state').read_bytes())['body']; body['policy']['body']['authorities' if change=='author' else 'channels']=[]
                    (root/'.apkg/state').write_bytes(wire('root',body))
                before=(root/'.apkg/state').read_bytes(); result=offline(root,profile); self.assertEqual(result.returncode,expected,result.stdout+result.stderr)
                native=worker(root,profile); self.assertEqual(native.returncode,0,native.stdout+native.stderr)
                if expected: self.assertEqual(native.stdout,'status %d\n'%expected)
                else:
                    self.assertEqual(native.stdout,'status 0\n'+result.stdout)
                    self.assertIn('newer 2\n' if change=='remove' else 'metadataeligible 0\n',result.stdout)
                self.assertEqual(profile.read_bytes(),saved); self.assertEqual((root/'.apkg/state').read_bytes(),before)

    def test_malformed_frames_signed_tampering_and_size_limits_refuse(self):
        with tempfile.TemporaryDirectory() as tmp:
            d=Path(tmp); root,endpoint=fixture(d); profile=d/'saved'
            self.assertEqual(command('source-cache',root,profile,endpoint,CHANNEL).returncode,0)
            good=profile.read_bytes(); header=41
            mutations=[good[:n] for n in range(header)]+[good+b'extra',b'X'+good[1:],good[:12]+b'\x02'+good[13:]]
            lengths=13+16
            for offset in (lengths,lengths+4,lengths+8): mutations.append(good[:offset]+b'\xff'*4+good[offset+4:])
            mutations.append(good[:header]+b'\0'+good[header+1:])
            for data in mutations:
                profile.write_bytes(data); self.assertEqual(read(profile)[0],1)
            tampered=good.replace(b'2.0',b'9.0',1); self.assertNotEqual(tampered,good)
            profile.write_bytes(tampered); code,c=read(profile); self.assertEqual(code,0); lib.apkg_source_config_free(c)
            self.assertNotEqual(offline(root,profile).returncode,0)
            profile.write_bytes(good[:13]+bytes.fromhex('ef'*16)+good[29:]); self.assertEqual(offline(root,profile).returncode,12)
            with profile.open('wb') as out: out.truncate(16*1024*1024+1)
            self.assertEqual(read(profile)[0],3)

    def test_no_follow_nonblocking_failures_and_preserved_previous_profile(self):
        with tempfile.TemporaryDirectory() as tmp:
            d=Path(tmp); profile=d/'saved'; self.assertEqual(save(profile,'local'),0); good=profile.read_bytes()
            link=d/'link'; link.symlink_to(profile)
            self.assertEqual(read(link)[0],6); self.assertEqual(save(link,'other'),6); self.assertEqual(profile.read_bytes(),good)
            fifo=d/'fifo'; os.mkfifo(fifo); self.assertEqual(read(fifo)[0],6)
            folder=d/'folder'; folder.mkdir(); self.assertEqual(read(folder)[0],6); self.assertNotEqual(save(folder,'other'),0)
            self.assertEqual(read(d/'absent')[0],5)
            for nth in range(1,8):
                lib.apkg_test_fail_alloc(nth); code=save(profile,'other'); lib.apkg_test_fail_alloc(0)
                self.assertIn(code,(0,4))
                if code: self.assertEqual(profile.read_bytes(),good)
                else: self.assertEqual(save(profile,'local'),0)
            self.assertEqual(sorted(p.name for p in d.iterdir()),['fifo','folder','link','saved'])

    def test_allocation_failures_and_native_cancel_leave_profiles_and_roots_unchanged(self):
        with tempfile.TemporaryDirectory() as tmp:
            d=Path(tmp); root,endpoint=fixture(d); profile=d/'saved'; self.assertEqual(command('source-cache',root,profile,endpoint,CHANNEL).returncode,0)
            before=(root/'.apkg/state').read_bytes(); saved=profile.read_bytes()
            for nth in range(1,100):
                lib.apkg_test_fail_alloc(nth); code,c=read(profile)
                lib.apkg_test_fail_alloc(0); self.assertIn(code,(0,4))
                if c: lib.apkg_source_config_free(c)
            code,c=read(profile); self.assertEqual(code,0)
            for nth in range(1,180):
                s,ch=C.c_void_p(),C.c_void_p(); lib.apkg_test_fail_alloc(nth)
                code=lib.apkg_source_config_open(c,str(root).encode(),C.byref(s),C.byref(ch),None)
                lib.apkg_test_fail_alloc(0); self.assertIn(code,(0,4),nth)
                if code: self.assertFalse(s); self.assertFalse(ch)
                lib.apkg_snapshot_free(s); lib.apkg_channel_free(ch)
            lib.apkg_source_config_free(c)
            for fault,expected in [(-1,16),(1,4)]:
                result=worker(root,profile,fault); self.assertEqual(result.returncode,0,result.stdout+result.stderr); self.assertEqual(result.stdout,'status %d\n'%expected)
            self.assertEqual(profile.read_bytes(),saved); self.assertEqual((root/'.apkg/state').read_bytes(),before)
