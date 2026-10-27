# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
import json
import tempfile
import unittest
from pathlib import Path
from test_client import cli
from support import TARGET
class Bundles(unittest.TestCase):
 def test_unregistered_bundle_refuses_linked_application_record(self):
  with tempfile.TemporaryDirectory() as d:
   folder,_=self.bundle(d);outside=Path(d)/'outside.json'
   (folder/'application.json').rename(outside);(folder/'application.json').symlink_to(outside)
   self.assertEqual(cli('bundle-check',folder,TARGET).returncode,6)
 def bundle(self,d):
  folder=Path(d)/'unregistered application';folder.mkdir();binary=bytearray(64);binary[:7]=b'\x7fELF\x02\x01\x01';binary[18:20]=(62).to_bytes(2,'little');(folder/'program').write_bytes(binary)
  app=dict(schema=1,id='demo',name='Demo',version='1.0',revision=1,targets=[dict(os='aros',arch='x86_64',abi='v11')],launch=dict(mode='shell',executable='program',working_directory='.'))
  (folder/'application.json').write_text(json.dumps(app));return folder,app
 def test_unregistered_bundle_and_moved_folder(self):
  with tempfile.TemporaryDirectory() as d:
   folder,app=self.bundle(d);result=cli('bundle-check',folder,TARGET);self.assertEqual(result.returncode,0,result.stdout+result.stderr);self.assertIn('Registration: not required',result.stdout);moved=folder.with_name('moved bundle');folder.rename(moved);self.assertEqual(cli('bundle-check',moved,TARGET).returncode,0);self.assertEqual(cli('run-bundle',moved,TARGET).returncode,2);self.assertFalse((moved/'.apkg').exists())
 def test_escape_wrong_cpu_missing_icon_and_requirements(self):
  with tempfile.TemporaryDirectory() as d:
   folder,app=self.bundle(d);(folder/'outside').symlink_to(Path(d));app['launch']['executable']='outside/evil';(folder/'application.json').write_text(json.dumps(app));self.assertEqual(cli('bundle-check',folder,TARGET).returncode,1)
   app['launch']['executable']='program';app['launch']['mode']='workbench';(folder/'application.json').write_text(json.dumps(app));self.assertEqual(cli('bundle-check',folder,TARGET).returncode,5);(folder/'program.info').write_bytes(b'icon fixture');self.assertEqual(cli('bundle-check',folder,TARGET).returncode,0);self.assertEqual(cli('bundle-check',folder,'aarch64-aros-v1-native').returncode,13)
