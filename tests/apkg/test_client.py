# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
import os
import re
import subprocess
import tempfile
import unittest
from pathlib import Path
from support import *
CLI=Path(os.environ.get('APKG_CLI',ROOT/'build/apkg'))
WORKER=Path(os.environ.get('APKG_PM_WORKER',ROOT/'build/test_pm_common'))
def cli(*args):return subprocess.run([str(CLI),*map(str,args)],capture_output=True,text=True)
class Clients(unittest.TestCase):
 def setup_root_package(self,d,name):
  root=Path(d)/name;seed=Path(d)/(name+'.policy');seed.write_bytes(policy());p=cli('init',root,TARGET,seed,'--yes');self.assertEqual(p.returncode,0,p.stdout+p.stderr);folder=Path(d)/(name+'.package');folder.mkdir();data=descriptor();(folder/'descriptor.json').write_bytes(data);(folder/'signature.json').write_bytes(sign(data));return root,folder
 def test_cli_preview_digest_and_native_worker_equivalence(self):
  with tempfile.TemporaryDirectory() as d:
   a,package_dir=self.setup_root_package(d,'cli with spaces');preview=cli('install',a,package_dir);self.assertEqual(preview.returncode,16);self.assertFalse((a/'demo').exists());digest=re.search(r'Approved plan: ([a-f0-9]{64})',preview.stdout).group(1)
   wildcard=cli('install',a,package_dir,'--confirm','*');self.assertEqual(wildcard.returncode,16);self.assertFalse((a/'demo').exists())
   result=cli('install',a,package_dir,'--confirm',digest);self.assertEqual(result.returncode,0,result.stdout+result.stderr)
   b,other_dir=self.setup_root_package(d,'gui with spaces');worker=subprocess.run([str(WORKER),str(b),str(other_dir),'1'],capture_output=True,text=True);self.assertEqual(worker.returncode,0,worker.stdout+worker.stderr)
   self.assertEqual((a/'demo/hello').read_bytes(),(b/'demo/hello').read_bytes());left=json.loads((a/'.apkg/state').read_bytes())['body'];right=json.loads((b/'.apkg/state').read_bytes())['body'];self.assertEqual(left['installed'],right['installed']);self.assertEqual(left['generation'],right['generation'])
 def test_gui_decline_and_close(self):
  for choice in (0,2,3,4,6):
   with self.subTest(choice=choice),tempfile.TemporaryDirectory() as d:
    root,folder=self.setup_root_package(d,'root');result=subprocess.run([str(WORKER),str(root),str(folder),str(choice)],capture_output=True,text=True);self.assertEqual(result.returncode,0,result.stdout+result.stderr);self.assertFalse((root/'demo').exists())
if __name__=='__main__':unittest.main()

class OperationClients(unittest.TestCase):
 setup_root_package=Clients.setup_root_package
 def commit(self,*args):
  preview=cli(*args);self.assertEqual(preview.returncode,16,preview.stdout+preview.stderr);digest=re.search(r'Approved plan: ([a-f0-9]{64})',preview.stdout).group(1);result=cli(*args,'--confirm',digest);self.assertEqual(result.returncode,0,result.stdout+result.stderr);return result
 def worker(self,root,arg,action,pool=None):
  args=[str(WORKER),str(root),str(arg),'1',str(action)]
  if pool:args.append(str(pool))
  result=subprocess.run(args,capture_output=True,text=True);self.assertEqual(result.returncode,0,result.stdout+result.stderr)
 def compare(self,a,b):
  left=json.loads((a/'.apkg/state').read_bytes())['body'];right=json.loads((b/'.apkg/state').read_bytes())['body'];self.assertEqual(left['installed'],right['installed']);self.assertEqual(left['generation'],right['generation'])
 def test_cli_and_worker_upgrade_repair_remove_equivalence(self):
  with tempfile.TemporaryDirectory() as d:
   a,pa=self.setup_root_package(d,'left');b,pb=self.setup_root_package(d,'right');self.commit('install',a,pa);self.worker(b,pb,0)
   for root in (a,b):(root/'demo/hello').write_text('damaged')
   self.commit('repair',a,'demo');self.worker(b,'demo',2);self.compare(a,b);self.assertEqual((a/'demo/hello').read_bytes(),b'hello\n')
   newer=json.loads((pa/'descriptor.json').read_bytes());newer['body']['version']='2.0';data=json.dumps(newer,sort_keys=True,separators=(',',':')).encode()
   for folder in (pa,pb):(folder/'descriptor.json').write_bytes(data);(folder/'signature.json').write_bytes(sign(data))
   self.commit('upgrade',a,pa);self.worker(b,pb,3);self.compare(a,b)
   query=subprocess.run([str(WORKER),str(b),str(pb),'7'],capture_output=True,text=True);self.assertEqual(query.returncode,0,query.stderr);self.assertIn('demo 2.0',query.stdout)
   self.commit('remove',a,'demo');self.worker(b,'demo',1);self.compare(a,b);self.assertFalse((a/'demo').exists());self.assertFalse((b/'demo').exists())
 def test_readonly_listing_folder_and_unsupported_launch(self):
  with tempfile.TemporaryDirectory() as d:
   a,pa=self.setup_root_package(d,'read');self.commit('install',a,pa);before=(a/'.apkg/state').read_bytes();listed=cli('list',a);self.assertEqual(listed.returncode,0);self.assertIn('demo 1.0',listed.stdout);self.assertNotIn('data":',listed.stdout);self.assertIn('not reverified',listed.stdout);self.assertEqual(cli('folder',a,'demo').returncode,0);self.assertEqual((a/'.apkg/state').read_bytes(),before)
   absent=Path(d)/'absent';self.assertEqual(cli('list',absent).returncode,0);self.assertFalse(absent.exists())
 def test_cli_and_worker_resolve_then_rollback_equivalence(self):
  from test_relations import related,requirement
  with tempfile.TemporaryDirectory() as d:
   a,pa=self.setup_root_package(d,'resolve-cli');b,pb=self.setup_root_package(d,'resolve-worker');pool=Path(d)/'pool';pool.mkdir();provider=pool/'provider';provider.mkdir();dep=related('provider');(provider/'descriptor.json').write_bytes(dep);(provider/'signature.json').write_bytes(sign(dep));app=related('app',depends=[requirement('provider')])
   for folder in (pa,pb):(folder/'descriptor.json').write_bytes(app);(folder/'signature.json').write_bytes(sign(app))
   self.commit('resolve',a,pa,'--pool',pool);self.worker(b,pb,5,pool);self.compare(a,b)
   def history(root):
    state=(root/'.apkg/state').read_bytes();return next(p for p in root.parent.glob(root.name+'.apkg-history.*') if (p/'decision').exists() and (p/'new-state').read_bytes()==state)
   self.commit('rollback',a,history(a));self.worker(b,history(b),4);self.compare(a,b);self.assertFalse((a/'app').exists());self.assertFalse((b/'provider').exists())

class ArchiveClients(unittest.TestCase):
 def test_archive_install_preview_and_worker_equivalence(self):
  from test_payload import archive,blob,desc
  helper=OperationClients()
  with tempfile.TemporaryDirectory() as d:
   a,unused=helper.setup_root_package(d,'archive-cli');b,unused=helper.setup_root_package(d,'archive-worker');source=Path(d)/'file';source.write_bytes(b'archive payload');entry=blob('hello',source);pkg=archive(Path(d)/'signed.pkg',desc([entry]),[('objects/'+entry['blob'],source)]);helper.commit('install',a,pkg);helper.worker(b,pkg,0);helper.compare(a,b)
