# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
import ctypes as C
import os
import tempfile
import unittest
from support import *
from test_client import cli
from test_attributes import attrs, observed, setattrs
from test_payload import desc,archive,import_package
from test_operations import run_plan

class Sources(unittest.TestCase):
 def test_ameta_source_archive_installed_inventory_agree(self):
  with tempfile.TemporaryDirectory() as d:
   source=Path(d)/'source';source.mkdir();(source/'.hidden').write_bytes(b'payload');(source/'plain').write_bytes(b'default');(source/'Empty').mkdir();(source/'nested').mkdir();(source/'nested/tool').write_bytes(b'data');(source/'nested/tool').chmod(0o751)
   (source/'.ameta').write_text('ameta 1\nfile .hidden\nprot 0x8000000000000004\ncomment caf%C3%A9\nuid 4294967295\ngid 42\ncolour red\nfile Empty\ncomment empty\nfile plain\nprot 0x2\n')
   (source/'nested/.ameta').write_text('ameta 1\nfile tool\nprot 0x00000042\ncomment executable\n')
   before={p.relative_to(source).as_posix():p.read_bytes() for p in source.rglob('*') if p.is_file()};result=cli('source-inventory',source);self.assertEqual(result.returncode,0,result.stderr+result.stdout);inventory=json.loads(result.stdout);files=inventory['files'];self.assertFalse(any(e['path'].endswith('.ameta') for e in files));self.assertNotIn('amiga',next(e for e in files if e['path']=='plain'));hidden=next(e for e in files if e['path']=='.hidden');self.assertEqual(hidden['amiga'],attrs(protection=str(2**63+6),uid=str(2**32-1),gid='42',extra=[dict(key=b'colour'.hex(),value=b'red'.hex())]));tool=next(e for e in files if e['path']=='nested/tool');self.assertEqual(tool['amiga']['protection'],'64');self.assertEqual(tool['mode'],str(0o751))
   root=Path(d)/'root';c=init(root);data=desc(files);objects={e['blob']:source/e['path'] for e in files if e['type']=='file'};pkg=archive(Path(d)/'package',data,[('objects/'+sha,path) for sha,path in objects.items()]);code,obj=import_package(c,pkg);self.assertEqual(code,0);code,s=snapshot(c);code,p=install_plan(c,s,obj);self.assertEqual(code,0);self.assertEqual(run_plan(c,p),(0,1));installed=json.loads(cli('source-inventory',root/'demo').stdout);self.assertEqual(installed['files'],inventory['files']);self.assertEqual(observed(root/'demo/.hidden'),hidden['amiga']);self.assertEqual(before,{p.relative_to(source).as_posix():p.read_bytes() for p in source.rglob('*') if p.is_file()});lib.apkg_plan_free(p);lib.apkg_snapshot_free(s);lib.apkg_package_free(obj);lib.apkg_close(c)
 def test_bad_stale_reserved_and_unrepresentable_metadata_refuse(self):
  vectors=['not ameta\n','ameta 1\nfile missing\ncomment lost\n','ameta 1\nfile file\nuid 4294967296\n','ameta 1\nfile file\ncomment broken%zz\n','ameta 1\nfile .ameta\ncomment control\n','ameta 1\nfile file\ncomment %E2%98%83\n','ameta 1\nfile file\ncomment first\nfile file\ncomment second\n']
  with tempfile.TemporaryDirectory() as d:
   source=Path(d)/'source';source.mkdir();(source/'file').write_text('unchanged')
   for data in vectors:
    with self.subTest(data=data):
     (source/'.ameta').write_text(data);result=cli('source-inventory',source);self.assertNotEqual(result.returncode,0);self.assertNotIn('"files"',result.stdout);self.assertEqual((source/'file').read_text(),'unchanged');self.assertEqual((source/'.ameta').read_text(),data)
 def test_conflicting_live_and_ameta_facts_refuse(self):
  with tempfile.TemporaryDirectory() as d:
   source=Path(d)/'source';source.mkdir();(source/'file').write_text('body');value=attrs();setattrs(source/'file',value);(source/'.ameta').write_text('ameta 1\nfile file\ncomment different\n');self.assertEqual(cli('source-inventory',source).returncode,14);self.assertEqual(observed(source/'file'),value)
 def test_symlinks_and_missing_root_refuse_without_creation(self):
  with tempfile.TemporaryDirectory() as d:
   source=Path(d)/'source';self.assertNotEqual(cli('source-inventory',source).returncode,0);self.assertFalse(source.exists());source.mkdir();(source/'link').symlink_to(Path(d)/'outside');self.assertNotEqual(cli('source-inventory',source).returncode,0);self.assertFalse((Path(d)/'outside').exists())

 def test_inventory_allocation_failures_are_readonly_and_typed(self):
  lib.ak_source_inventory.argtypes=[C.c_char_p,C.POINTER(C.c_void_p),C.c_void_p];lib.ak_source_inventory.restype=C.c_int
  lib.apkg_client_text_free.argtypes=[C.c_void_p]
  with tempfile.TemporaryDirectory() as d:
   source=Path(d)/'source';source.mkdir();file=source/'file';file.write_bytes(b'data');control=source/'.ameta';control.write_text('ameta 1\nfile file\ncomment hello\ncolour red\n')
   for nth in range(1,201):
    with self.subTest(nth=nth):
     out=C.c_void_p();lib.apkg_test_fail_alloc(nth)
     try:code=lib.ak_source_inventory(os.fsencode(source),C.byref(out),None)
     finally:lib.apkg_test_fail_alloc(0)
     self.assertIn(code,(0,4));self.assertEqual(bool(out),code==0);lib.apkg_client_text_free(out);self.assertEqual(file.read_bytes(),b'data');self.assertEqual(control.read_text(),'ameta 1\nfile file\ncomment hello\ncolour red\n')
