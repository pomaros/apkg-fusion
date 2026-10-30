# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
import json
from pathlib import Path
import tempfile
import unittest
from test_author import key_fixture
from test_client import cli

class PushAuthentication(unittest.TestCase):
 def test_common_push_signatures_reject_tampering_and_other_domains(self):
  with tempfile.TemporaryDirectory() as temporary:
   d=Path(temporary); key=key_fixture(d/'key'); request=d/'request'
   original=b'pkg-push-1\nPOST\n/common/_push/plan\n'+b'1'*32+b'\n1\n'+b'0'*64+b'\n-\n'
   request.write_bytes(original)
   signed=cli('push-sign',key,request);self.assertEqual(signed.returncode,0,signed.stderr);signature=json.loads(signed.stdout)
   verified=cli('push-verify',signature['public_key'],signature['signature'],request)
   self.assertEqual(verified.returncode,0,verified.stderr);self.assertTrue(json.loads(verified.stdout)['verified'])
   for changed in (original.replace(b'POST',b'PUT'),original.replace(b'/common/',b'/other/'),b'other-domain\n'+original,original+b'\0'):
    request.write_bytes(changed);self.assertNotEqual(cli('push-verify',signature['public_key'],signature['signature'],request).returncode,0)
   for invalid in (b'APKG-SIG\0\1'+b'0'*100,b'no prefix\n',original[:-1],b'pkg-push-1\n'+b'x'*8192):
    request.write_bytes(invalid);self.assertNotEqual(cli('push-sign',key,request).returncode,0)
