# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
import ctypes as C
import json
import tempfile
import unittest
from pathlib import Path
from support import *
from test_operations import install,op,run_plan,text
from test_payload import archive,blob,desc,import_package
class InventoryScale(unittest.TestCase):
 def test_many_files_install_repair_remove_and_bounded_records(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d)/'root';c=init(root);source=Path(d)/'data';source.write_bytes(b'contents');base=blob('file',source);entries=[dict(base,path=f'files/item-{i:04}') for i in range(1200)];entries.insert(0,dict(path='files',type='directory'));data=desc(entries);pkg=archive(Path(d)/'many.pkg',data,[('objects/'+base['blob'],source)]);code,obj=import_package(c,pkg);self.assertEqual(code,0);code,s=snapshot(c);code,p=install_plan(c,s,obj);self.assertEqual(code,0);self.assertEqual(run_plan(c,p),(0,1));lib.apkg_plan_free(p);lib.apkg_package_free(obj);lib.apkg_snapshot_free(s)
   (root/'demo/files/item-0031').unlink();(root/'demo/files/personal').write_bytes(b'mine');code,p=op(c,'repair');self.assertEqual(code,0);self.assertLess(lib.apkg_plan_get(p).contents.canonical.size,2*1024*1024);self.assertEqual(run_plan(c,p),(0,1));self.assertEqual((root/'demo/files/item-0031').read_bytes(),b'contents');lib.apkg_plan_free(p);code,p=op(c,'remove');self.assertEqual(code,0);self.assertEqual(run_plan(c,p),(0,1));self.assertEqual(list((root/'demo/files').iterdir()),[root/'demo/files/personal']);lib.apkg_plan_free(p);lib.apkg_close(c)
 def test_import_rejects_invalid_text_without_io(self):
  lib.apkg_package_import.argtypes=[C.c_void_p,Text,C.c_void_p,C.POINTER(C.c_void_p),C.c_void_p]
  with tempfile.TemporaryDirectory() as d:
   root=Path(d)/'root';c=init(root)
   for bad in (b'bad\x1bname',b'bad\xffname',b'bad\nname',b'bad\x00name'):
    rawpath=C.create_string_buffer(bad);out=C.c_void_p(123);code=lib.apkg_package_import(c,Text(C.cast(rawpath,C.c_void_p),len(bad)),None,C.byref(out),None);self.assertEqual(code,1);self.assertFalse(out)
   self.assertFalse(Path(str(root)+'.apkg-objects').exists());lib.apkg_close(c)
