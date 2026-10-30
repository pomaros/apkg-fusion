# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
import hashlib
import json
from pathlib import Path
import runpy
import tempfile
import subprocess
import sys
import unittest
import zipfile
from support import ROOT

DATA=runpy.run_path(str(ROOT/'tools/apkg-portal-data'))

class PortalData(unittest.TestCase):
    def test_private_backup_restores_exact_common_data_modes_and_refuses_replacement(self):
        with tempfile.TemporaryDirectory() as temporary:
            base=Path(temporary);data=base/'data';data.mkdir(mode=0o700)
            for name,content in {'channels/demo/current':b'a'*64+b'\n','channels/demo/.portal-owner.json':b'owner',
                                 'channels/demo/archives/digest.pkg.url':b'https://example.invalid/archive',
                                 'state/publishers.json':b'publisher identity','state/auth-keys/key.xml':b'private cookie material',
                                 'staging/publisher/upload':b'partial upload'}.items():
                path=data/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(content);path.chmod(0o600)
            (data/'empty').mkdir(mode=0o750)
            archive=base/'backup.zip';DATA['backup'](data,archive)
            self.assertEqual(archive.stat().st_mode & 0o777,0o600)
            restored=base/'restored';DATA['restore'](archive,restored)
            self.assertEqual(DATA['members'](data),DATA['members'](restored))
            for operation in (lambda:DATA['backup'](data,archive),lambda:DATA['restore'](archive,restored),lambda:DATA['backup'](data,data/'backup.zip')):
                with self.assertRaises(ValueError):operation()

    def test_command_line_backup_restore_and_stopped_precondition(self):
        with tempfile.TemporaryDirectory() as temporary:
            base=Path(temporary);data=base/'data';data.mkdir();(data/'identity').write_bytes(b'synthetic')
            archive=base/'backup.zip';destination=base/'restored'
            for operation,folder in [('backup',data),('restore',destination)]:
                command=[sys.executable,str(ROOT/'tools/apkg-portal-data'),operation,'--data',str(folder),'--archive',str(archive)]
                self.assertEqual(subprocess.run(command,capture_output=True).returncode,2)
                completed=subprocess.run(command+['--stopped'],capture_output=True,text=True)
                self.assertEqual(completed.returncode,0,completed.stderr)
            self.assertEqual((destination/'identity').read_bytes(),b'synthetic')

    def test_restore_refuses_tampering_paths_and_extra_members_without_publishing_tree(self):
        with tempfile.TemporaryDirectory() as temporary:
            base=Path(temporary)
            for name in ('../escape','/absolute','a/../../escape','a\\escape','disk:escape','a//b','okay'):
                target=base/'restored';archive=base/'bad.zip'
                listing={name:dict(kind='file',mode=0o600,size=7,sha256=hashlib.sha256(b'correct').hexdigest())}
                with zipfile.ZipFile(archive,'w') as output:
                    output.writestr('manifest.json',json.dumps(dict(format='apkg-portal-backup-1',root_mode=0o700,members=listing)))
                    output.writestr('data/'+name,b'changed')
                with self.assertRaises(ValueError):DATA['restore'](archive,target)
                self.assertFalse(target.exists())
            data=base/'source';data.mkdir();(data/'link').symlink_to(base/'bad.zip')
            with self.assertRaises(ValueError):DATA['backup'](data,base/'symlink.zip')
