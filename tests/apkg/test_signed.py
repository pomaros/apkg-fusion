# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
import ctypes as C
import tempfile
import unittest
from pathlib import Path
from support import *
class Signed(unittest.TestCase):
 def test_valid_and_refusals(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d)/'root';state(root,policy=policy());r,c=open_ctx(root);data=descriptor();sig=sign(data);before=(root/'.apkg/state').read_bytes();r,p=package(c,data,sig);self.assertEqual(r,0);lib.apkg_package_free(p)
   r,p=package(c,data.replace(b'hello',b'jello'),sig);self.assertEqual(r,10)
   altered=json.loads(sig);altered['signature']='00'*64;r,p=package(c,data,json.dumps(altered,sort_keys=True,separators=(',',':')).encode());self.assertEqual(r,11)
   pk,sk=keypair(bytes(reversed(range(32))));r,p=package(c,data,sign(data,pk,sk));self.assertEqual(r,12)
   wrong=descriptor(target='x86_64-aros-v1-native');r,p=package(c,wrong,sign(wrong));self.assertEqual(r,13)
   r,p=package(c,data,sign(data,domain='channel'));self.assertEqual(r,11)
   self.assertEqual((root/'.apkg/state').read_bytes(),before);lib.apkg_close(c)
 def test_signed_unsupported_and_bad_payload(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d)/'root';state(root,policy=policy());r,c=open_ctx(root)
   for path in ('../escape','/absolute','foo//bar','SYS:C/evil','foo\\bar'):
    obj=json.loads(descriptor());obj['body']['files'][0]['path']=path;data=json.dumps(obj,sort_keys=True,separators=(',',':')).encode();self.assertNotEqual(package(c,data,sign(data))[0],0)
   obj=json.loads(descriptor());obj['body']['files'][0]['sha256']='00'*32;data=json.dumps(obj,sort_keys=True,separators=(',',':')).encode();self.assertNotEqual(package(c,data,sign(data))[0],0)
   obj=json.loads(descriptor());obj['requires']=['unsupported'];data=json.dumps(obj,sort_keys=True,separators=(',',':')).encode();self.assertNotEqual(package(c,data,sign(data))[0],0)
   elf=bytearray(64);elf[:4]=b'\x7fELF';elf[4]=2;elf[5]=1;elf[6]=1;elf[18:20]=(183).to_bytes(2,'little');obj=json.loads(descriptor());f=obj['body']['files'][0];f['data']=elf.hex();f['size']=str(len(elf));f['sha256']=hashlib.sha256(elf).hexdigest();data=json.dumps(obj,sort_keys=True,separators=(',',':')).encode();self.assertEqual(package(c,data,sign(data))[0],13)
   lib.apkg_close(c)
if __name__=='__main__':unittest.main()
