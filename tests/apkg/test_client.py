# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
import os
import re
import subprocess
import tempfile
import unittest
from pathlib import Path
from support import *
CLI=Path(os.environ.get('APKG_CLI',ROOT/'build/apkg-core'))
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
