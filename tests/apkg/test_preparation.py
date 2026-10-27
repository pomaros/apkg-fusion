# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
import ctypes as C
import json
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import unittest
from support import *
from test_operations import install, payload, run_plan
import test_operations as operations
from test_channel import catalog, seed, read_channel
from test_channel_heads import accepting
from test_payload import archive
from test_file_channel import artifact

MODES=('initialize','single','changes','heads')
PREFIX=(1,1201,1202,1203,1204)

def contents(root):
 path=root/'.apkg/state';return path.read_bytes() if path.exists() else None

def scenario(d,mode):
 root=d/'root'
 if mode=='initialize':
  code,c=open_ctx(root,1);assert code==0;code,s=snapshot(c);assert code==0;code,p=initialize_plan(c,s,seed=seed());lib.apkg_snapshot_free(s);assert code==0;return root,c,p,None
 before=state(root,policy=seed());code,c=open_ctx(root,1);assert code==0
 if mode=='heads':
  code,ch=read_channel(c,catalog(sequence=2));assert code==0;code,p=accepting(c,ch);lib.apkg_channel_free(ch);assert code==0;return root,c,p,before
 if mode=='changes':
  install(c,descriptor());(root/'demo/personal').write_bytes(b'keep this');data=json.loads(descriptor(files=[payload('hello','new')]));data['body']['version']='2.0';data=json.dumps(data,sort_keys=True,separators=(',',':')).encode();before=contents(root)
 else:data=descriptor()
 code,pkg=package(c,data,sign(data));assert code==0;code,s=snapshot(c);assert code==0
 if mode=='changes':p=C.c_void_p();code=lib.apkg_plan_upgrade(c,s,pkg,C.byref(p),None)
 else:code,p=install_plan(c,s,pkg)
 lib.apkg_snapshot_free(s);lib.apkg_package_free(pkg);assert code==0;return root,c,p,before

def crash(c,p,point,action=2):
 code,a=approve(c,p);assert code==0;pid=os.fork()
 if pid==0:
  lib.apkg_test_fault(point,action);code,outcome=execute(c,p,a);os._exit(100+code)
 lib.apkg_approval_free(a);return pid

def exited(pid):
 _,status=os.waitpid(pid,0);assert os.waitstatus_to_exitcode(status)==99,status

def stopped(pid):
 _,status=os.waitpid(pid,os.WUNTRACED);assert os.WIFSTOPPED(status) and os.WSTOPSIG(status)==signal.SIGSTOP,status

def reap(pid):
 try:
  waited,status=os.waitpid(pid,os.WNOHANG)
  if not waited:os.kill(pid,signal.SIGKILL);os.waitpid(pid,0)
 except ChildProcessError:pass

def drafts(root):return sorted(root.parent.glob(root.name+'.apkg-prepare.*'))

class Preparation(unittest.TestCase):
 def tearDown(self):lib.apkg_test_fault(0,0);lib.apkg_test_fail_alloc(0)
 def test_death_before_publication_never_blocks_root_and_explicit_retry_commits(self):
  for mode in MODES:
   for point in PREFIX:
    with self.subTest(mode=mode,point=point),tempfile.TemporaryDirectory() as d:
     root,c,p,before=scenario(Path(d),mode);exited(crash(c,p,point));self.assertEqual(contents(root),before);self.assertFalse(Path(str(root)+'.apkg-transaction').exists());orphan=drafts(root);self.assertEqual(len(orphan),1);self.assertEqual(orphan[0].stat().st_mode&0o777,0o700);binding=orphan[0].stat().st_ino;code,reader=open_ctx(root);self.assertEqual(code,0);code,s=snapshot(reader);self.assertEqual(code,0);self.assertEqual(lib.apkg_snapshot_root(s).contents.pending_count.value,0);self.assertEqual(lib.apkg_snapshot_root(s).contents.state,0 if mode=='initialize' else 2);lib.apkg_snapshot_free(s);lib.apkg_close(reader);self.assertEqual(recover(c),(0,0));self.assertEqual(contents(root),before);self.assertEqual(run_plan(c,p),(0,1));self.assertEqual(orphan[0].stat().st_ino,binding);self.assertFalse(Path(str(root)+'.apkg-transaction').exists())
     if mode=='changes':self.assertEqual((root/'demo/personal').read_bytes(),b'keep this');self.assertEqual((root/'demo/hello').read_text(),'new')
     lib.apkg_plan_free(p);lib.apkg_close(c)
 def test_error_prefixes_clean_only_own_draft_and_post_publish_errors_restore(self):
  for mode in MODES:
   for point in (*PREFIX,1205):
    with self.subTest(mode=mode,point=point),tempfile.TemporaryDirectory() as d:
     root,c,p,before=scenario(Path(d),mode);unrelated=root.parent/(root.name+'.apkg-prepare.'+'f'*32);unrelated.mkdir(mode=0o700);(unrelated/'keep').write_bytes(b'foreign evidence');code,a=approve(c,p);lib.apkg_test_fault(point,1);self.assertEqual(execute(c,p,a),(6,2 if point==1205 else 0));lib.apkg_test_fault(0,0);self.assertEqual(contents(root),before);self.assertEqual(drafts(root),[unrelated]);self.assertEqual((unrelated/'keep').read_bytes(),b'foreign evidence');self.assertEqual(recover(c),(0,0));lib.apkg_approval_free(a);self.assertEqual(run_plan(c,p),(0,1));lib.apkg_plan_free(p);lib.apkg_close(c)
 def test_published_ready_journal_recovers_at_both_sides_of_decision(self):
  for mode in MODES:
   for point in (1205,2,4,5):
    with self.subTest(mode=mode,point=point),tempfile.TemporaryDirectory() as d:
     root,c,p,before=scenario(Path(d),mode);exited(crash(c,p,point));active=Path(str(root)+'.apkg-transaction');self.assertTrue((active/'ready').is_file());self.assertEqual(drafts(root),[]);code,reader=open_ctx(root);self.assertEqual(code,0);code,s=snapshot(reader);self.assertEqual(code,0);self.assertEqual(lib.apkg_snapshot_root(s).contents.state,3);lib.apkg_snapshot_free(s);lib.apkg_close(reader);self.assertEqual(recover(c),(0,2 if point in (1205,2) else 1));self.assertEqual(recover(c),(0,0))
     if point in (1205,2):self.assertEqual(contents(root),before);self.assertEqual(run_plan(c,p),(0,1))
     self.assertFalse(active.exists());lib.apkg_plan_free(p);lib.apkg_close(c)
 def test_ready_draft_remains_invisible_to_readers_until_atomic_publication(self):
  with tempfile.TemporaryDirectory() as d:
   root,c,p,before=scenario(Path(d),'changes');pid=crash(c,p,1204,3);stopped(pid);self.addCleanup(reap,pid)
   try:
    draft=drafts(root)[0];self.assertTrue((draft/'ready').is_file());self.assertFalse(Path(str(root)+'.apkg-transaction').exists());self.assertEqual(contents(root),before);code,reader=open_ctx(root);code,s=snapshot(reader);self.assertEqual(code,0);self.assertEqual(lib.apkg_snapshot_root(s).contents.state,2);lib.apkg_snapshot_free(s);lib.apkg_close(reader);self.assertEqual((root/'demo/hello').read_text(),'hello\n');self.assertEqual(recover(c)[0],7) # parent lock is still held by the stopped writer
   finally:os.kill(pid,signal.SIGKILL);os.waitpid(pid,0)
   self.assertEqual(recover(c),(0,0));self.assertEqual(run_plan(c,p),(0,1));lib.apkg_plan_free(p);lib.apkg_close(c)
 def test_foreign_active_journal_wins_exclusive_publication_without_overwrite(self):
  for mode in ('single','changes'):
   with self.subTest(mode=mode),tempfile.TemporaryDirectory() as d:
    root,c,p,before=scenario(Path(d),mode);pid=crash(c,p,1204,3);stopped(pid);self.addCleanup(reap,pid);active=Path(str(root)+'.apkg-transaction');active.mkdir(mode=0o700);(active/'foreign').write_bytes(b'keep me');os.kill(pid,signal.SIGCONT);_,status=os.waitpid(pid,0);self.assertEqual(os.waitstatus_to_exitcode(status),108);self.assertEqual(contents(root),before);self.assertEqual((active/'foreign').read_bytes(),b'keep me');self.assertEqual(drafts(root),[]);self.assertEqual(recover(c),(9,3));(active/'foreign').unlink();active.rmdir();self.assertEqual(run_plan(c,p),(0,1));lib.apkg_plan_free(p);lib.apkg_close(c)
 def test_replaced_draft_identity_is_refused_without_deleting_foreign_bytes(self):
  with tempfile.TemporaryDirectory() as d:
   root,c,p,before=scenario(Path(d),'single');pid=crash(c,p,1204,3);stopped(pid);self.addCleanup(reap,pid);draft=drafts(root)[0];held=Path(d)/'held-preparation';draft.rename(held);draft.mkdir(mode=0o700);(draft/'foreign').write_bytes(b'keep me');os.kill(pid,signal.SIGCONT);_,status=os.waitpid(pid,0);self.assertEqual(os.waitstatus_to_exitcode(status),106);self.assertEqual((draft/'foreign').read_bytes(),b'keep me');self.assertTrue((held/'ready').is_file());self.assertFalse(Path(str(root)+'.apkg-transaction').exists());self.assertEqual(contents(root),before);self.assertEqual(recover(c),(0,0));self.assertEqual(run_plan(c,p),(0,1));lib.apkg_plan_free(p);lib.apkg_close(c)
 def test_ambiguous_preexisting_active_prefix_still_refuses_recovery(self):
  with tempfile.TemporaryDirectory() as d:
   root,c,p,before=scenario(Path(d),'single');active=Path(str(root)+'.apkg-transaction');active.mkdir(mode=0o700);(active/'unfinished').write_bytes(b'foreign or old prefix');self.assertEqual(recover(c),(9,3));self.assertEqual((active/'unfinished').read_bytes(),b'foreign or old prefix');self.assertEqual(contents(root),before);lib.apkg_plan_free(p);lib.apkg_close(c)
 def test_native_instrumented_error_prefix_cleanup_and_successful_retry(self):
  runner=Path(os.environ.get('APKG_PREPARATION_MEMORY',ROOT/'build/test_preparation_memory'))
  with tempfile.TemporaryDirectory() as d:
   d=Path(d);simple=archive(d/'simple.pkg',descriptor());complex_archive=artifact(d)
   for name,pkg in (('simple',simple),('complex',complex_archive)):
    for point in (*PREFIX,1205):
     root=d/(name+str(point));state(root,policy=seed());result=subprocess.run([str(runner),str(root),str(pkg),str(point)],capture_output=True,text=True);self.assertEqual(result.returncode,0,result.stdout+result.stderr);self.assertEqual(drafts(root),[]);self.assertFalse(Path(str(root)+'.apkg-transaction').exists());self.assertTrue((root/'demo').is_dir())
 def test_draft_name_does_not_reduce_existing_history_filename_budget(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d)/('r'*208);before=state(root,policy=seed());code,c=open_ctx(root,1);code,pkg=package(c,descriptor(),sign(descriptor()));self.assertEqual(code,0);code,s=snapshot(c);code,p=install_plan(c,s,pkg);self.assertEqual(code,0);lib.apkg_snapshot_free(s);lib.apkg_package_free(pkg);self.assertEqual(run_plan(c,p),(0,1));self.assertEqual(drafts(root),[]);self.assertTrue((root/'demo/hello').is_file());lib.apkg_plan_free(p);lib.apkg_close(c)
