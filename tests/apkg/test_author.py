# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
import os
import struct
import subprocess
import sys
import tempfile
import unittest
from support import *
from test_client import cli
import test_client as clients
from test_attributes import observed, attrs
from test_operations import op, run_plan
from test_payload import import_package
BUILD=ROOT/'tools/apkg-build'

def author_fixture(folder,name='demo',depends=None,version='1.2',revision=3,kind='data',launch=False):
 folder.mkdir();(folder/'program').write_bytes(b'body');(folder/'.hidden').write_bytes(b'hidden');(folder/'Empty').mkdir();(folder/'.ameta').write_text('ameta 1\nfile program\ncomment caf%C3%A9\nprot 0x40\nuid 42\ngid 123\ncolour red\n')
 if launch:
  head=bytearray(64);head[:7]=b'\x7fELF\x02\x01\x01';head[18:20]=(62).to_bytes(2,'little');(folder/'program').write_bytes(head);(folder/'program').chmod(0o755)
 m=f'id = "{name}"\nversion = "{version}"\nrevision = {revision}\nsummary = "Author test"\ncategory = "util"\nlicense = "MIT"\nkind = "{kind}"\narch = "x86_64"\nabi = "v11"\nsubdir = "{folder.name}"\ndepends = {json.dumps(depends or [])}\ndepends_checked = true\nconfig = [".hidden"]\n'
 if launch:m+='\n[launch]\nexecutable = "program"\nmode = "shell"\nworking_directory = "."\n'
 path=folder.with_name(folder.name+'.arospkg.toml');path.write_text(m);return path

def key_fixture(path):path.write_text('Pkg-Secret-Key: 1\nSeed: '+bytes(range(32)).hex()+'\nPublic: '+PK.hex()+'\n');path.chmod(0o600);return path

def build(folder,out,key,*extra,engine=None):
 return subprocess.run([sys.executable,str(BUILD),str(folder),'--output',str(out),'--sign',str(key),'--authority',AUTH,'--runtime','native','--engine',str(engine or clients.CLI),*map(str,extra)],capture_output=True,text=True)

def members(path):
 # Independent minimal reader for fixture assertions, not production transport.
 data=path.read_bytes();assert data[:4]==b'PKG\1';assert int.from_bytes(data[4:8],'big')==len(data);pos=8;out={}
 while pos<len(data):
  n=int.from_bytes(data[pos:pos+4],'big');pos+=4;name=data[pos:pos+n].decode();pos+=n;assert data[pos]==0;pos+=1;n=int.from_bytes(data[pos:pos+4],'big');pos+=4;out[name]=data[pos:pos+n];pos+=n
 return out

class Authors(unittest.TestCase):
 def test_real_toml_ameta_archive_cli_worker_and_unchanged_source(self):
  with tempfile.TemporaryDirectory() as d:
   d=Path(d);source=d/'Drawer';mp=author_fixture(source);icon=d/'Drawer.info';icon.write_bytes(b'icon fixture');mp.write_text(mp.read_text()+'icon = \"Drawer.info\"\n');key=key_fixture(d/'key');before={p.name:p.read_bytes() for p in source.iterdir() if p.is_file()};out=d/'package.pkg';result=build(source,out,key);self.assertEqual(result.returncode,0,result.stderr);answer=json.loads(result.stdout);self.assertEqual(answer['public_key'],PK.hex());self.assertEqual(answer['publication_mode'],'unpublished');packed=members(out);self.assertEqual(packed['signature.json'],sign(packed['descriptor.json']));descriptor=json.loads(packed['descriptor.json']);self.assertEqual(descriptor['body']['revision'],'3');self.assertEqual(descriptor['body']['version'],'1.2');files=descriptor['body']['files'];self.assertIn('Drawer/Empty',[f['path'] for f in files]);self.assertIn('Drawer/.hidden',[f['path'] for f in files]);self.assertFalse(any(f['path'].endswith('.ameta') for f in files));original=next(f for f in files if f['path']=='Drawer/Package.arospkg.toml');self.assertEqual(packed['objects/'+original['blob']],mp.read_bytes());self.assertEqual(before,{p.name:p.read_bytes() for p in source.iterdir() if p.is_file()})
   helper=clients.OperationClients();a,_=helper.setup_root_package(d,'cli');b,_=helper.setup_root_package(d,'worker');helper.commit('install',a,out);helper.worker(b,out,0);helper.compare(a,b);self.assertEqual((a/'demo/Drawer.info').read_bytes(),icon.read_bytes());self.assertEqual((a/'demo/Drawer/.hidden').read_bytes(),b'hidden');self.assertTrue((a/'demo/Drawer/Empty').is_dir());self.assertEqual(observed(a/'demo/Drawer/program'),attrs(protection='66',extra=[dict(key=b'colour'.hex(),value=b'red'.hex())]));self.assertEqual(observed(a/'demo/Drawer/program'),observed(b/'demo/Drawer/program'))
 def test_native_key_generation_identity_and_signature_tampering(self):
  with tempfile.TemporaryDirectory() as d:
   d=Path(d);source=d/'Drawer';author_fixture(source);key=d/'key';generated=cli('keygen',key);self.assertEqual(generated.returncode,0,generated.stderr);public=json.loads(generated.stdout)['public_key'];self.assertEqual(json.loads(cli('key-public',key).stdout)['public_key'],public);self.assertEqual(key.stat().st_mode&0o777,0o600);before=key.read_bytes();self.assertNotEqual(cli('keygen',key).returncode,0);self.assertEqual(key.read_bytes(),before);out=d/'package';result=build(source,out,key);self.assertEqual(result.returncode,0,result.stderr);self.assertEqual(json.loads(result.stdout)['public_key'],public)
   root=d/'root';seed=d/'policy';seed.write_bytes(policy(bytes.fromhex(public)));initialized=cli('init',root,TARGET,seed,'--yes');self.assertEqual(initialized.returncode,0,initialized.stderr);code,c=open_ctx(root,1);self.assertEqual(code,0);code,obj=import_package(c,out);self.assertEqual(code,0);lib.apkg_package_free(obj);lib.apkg_close(c)
   key.write_bytes(before.replace(public.encode(),b'00'*32));self.assertNotEqual(cli('key-public',key).returncode,0);self.assertEqual(key.read_bytes(),before.replace(public.encode(),b'00'*32))
 def test_application_generated_from_toml_and_cpu_mismatch_refused(self):
  with tempfile.TemporaryDirectory() as d:
   d=Path(d);source=d/'App';mp=author_fixture(source,kind='app',launch=True);key=key_fixture(d/'key');out=d/'app.pkg';result=build(source,out,key);self.assertEqual(result.returncode,0,result.stderr);packed=members(out);body=json.loads(packed['descriptor.json'])['body'];app=next(f for f in body['files'] if f['path']=='App/application.json');application=json.loads(packed['objects/'+app['blob']]);self.assertEqual(application['launch']['executable'],'program');self.assertEqual(application['revision'],3);self.assertFalse((source/'application.json').exists());out.unlink();head=bytearray((source/'program').read_bytes());head[18:20]=(183).to_bytes(2,'little');(source/'program').write_bytes(head);self.assertNotEqual(build(source,out,key).returncode,0);self.assertFalse(out.exists());self.assertFalse(list(d.glob('.apkg-build-*')))
 def test_relations_config_revisions_and_refusals_have_no_partial_output(self):
  with tempfile.TemporaryDirectory() as d:
   d=Path(d);source=d/'Drawer';mp=author_fixture(source,depends=['provider >= 1.1']);key=key_fixture(d/'key');out=d/'package';result=build(source,out,key);self.assertEqual(result.returncode,0,result.stderr);body=json.loads(members(out)['descriptor.json'])['body'];self.assertEqual(body['relations']['depends'],[dict(authority=AUTH,name='provider',min_version='1.1',min_revision='1',target=TARGET)]);self.assertEqual(next(f for f in body['files'] if f['path']=='Drawer/.hidden')['config'],'preserve');original=out.read_bytes();self.assertNotEqual(build(source,out,key).returncode,0);self.assertEqual(out.read_bytes(),original);out.unlink();original_manifest=mp.read_text()
   for extra in ('review = "approved"\n','runs_on = ["unknown distribution"]\n','requires_system = [{type="library",id="x.library",min_version="1"}]\n'):
    with self.subTest(extra=extra):mp.write_text(original_manifest+extra);self.assertNotEqual(build(source,out,key).returncode,0);self.assertFalse(out.exists());self.assertFalse(list(d.glob('.apkg-build-*')))

 def test_large_application_build_and_install_uses_streaming_path(self):
  with tempfile.TemporaryDirectory() as d:
   d=Path(d);source=d/'App';author_fixture(source,kind='app',launch=True);key=key_fixture(d/'key');program=source/'program'
   with program.open('ab') as out:
    for _ in range(513):out.write(b'P'*65536)
   output=d/'large.pkg';result=build(source,output,key);self.assertEqual(result.returncode,0,result.stderr);self.assertGreater(output.stat().st_size,32*1024*1024);helper=clients.OperationClients();root,_=helper.setup_root_package(d,'root');helper.commit('install',root,output);self.assertEqual((root/'demo/App/program').stat().st_size,program.stat().st_size)
   import hashlib
   def digest(path):
    h=hashlib.sha256()
    with path.open('rb') as f:
     for chunk in iter(lambda:f.read(65536),b''):h.update(chunk)
    return h.digest()
   self.assertEqual(digest(program),digest(root/'demo/App/program'))
 def test_archive_publication_failure_and_process_death_leave_sources_retryable(self):
  import runpy
  from types import SimpleNamespace
  from unittest.mock import patch
  implementation=runpy.run_path(str(BUILD),run_name='author_fault_fixture')['build']
  for kind in ('io','death'):
   with self.subTest(kind=kind),tempfile.TemporaryDirectory() as d:
    d=Path(d);source=d/'Drawer';manifest=author_fixture(source);key=key_fixture(d/'key');output=d/'package';before=manifest.read_bytes();args=SimpleNamespace(drawer=source,output=output,sign=key,authority=AUTH,runtime='native',engine=clients.CLI)
    if kind=='io':
     with patch('os.link',side_effect=OSError('injected publication failure')):
      with self.assertRaises(OSError):implementation(args)
     self.assertFalse(list(d.glob('.apkg-build-*')))
    else:
     pid=os.fork()
     if pid==0:
      with patch.object(tempfile,'tempdir',str(d)),patch('os.link',side_effect=lambda *args:os._exit(99)):implementation(args)
      os._exit(98)
     _,status=os.waitpid(pid,0);self.assertEqual(os.waitstatus_to_exitcode(status),99);self.assertEqual(len(list(d.glob('.apkg-build-*'))),1)
    self.assertFalse(output.exists());self.assertEqual(manifest.read_bytes(),before);result=build(source,output,key);self.assertEqual(result.returncode,0,result.stderr);self.assertTrue(output.is_file())

 def test_readonly_source_drawer_mode_survives_generated_provenance(self):
  with tempfile.TemporaryDirectory() as d:
   d=Path(d);source=d/'Drawer';author_fixture(source);source.chmod(0o500);key=key_fixture(d/'key');output=d/'package';result=build(source,output,key);self.assertEqual(result.returncode,0,result.stderr);self.assertEqual(source.stat().st_mode&0o777,0o500);self.assertFalse((source/'Package.arospkg.toml').exists());body=json.loads(members(output)['descriptor.json'])['body'];self.assertEqual(next(f for f in body['files'] if f['path']=='Drawer')['mode'],'320');helper=clients.OperationClients();root,_=helper.setup_root_package(d,'root');helper.commit('install',root,output);self.assertEqual((root/'demo/Drawer').stat().st_mode&0o777,0o500)

 def test_prerelease_revision_and_all_implemented_relation_families(self):
  with tempfile.TemporaryDirectory() as d:
   d=Path(d);source=d/'Drawer';manifest=author_fixture(source,version='1.2-rc1',revision=7);manifest.write_text(manifest.read_text()+'conflicts = ["incompatible"]\nreplaces = ["obsolete"]\n\n[[recommends]]\nid = "optional"\nmin_version = "1.0"\nreason = "adds optional helpers"\n');key=key_fixture(d/'key');output=d/'package';result=build(source,output,key);self.assertEqual(result.returncode,0,result.stderr);body=json.loads(members(output)['descriptor.json'])['body'];self.assertEqual(body['version'],'1.2-rc1');self.assertEqual(body['revision'],'7');self.assertEqual(body['relations']['conflicts'][0]['name'],'incompatible');self.assertEqual(body['relations']['replaces'][0]['name'],'obsolete');self.assertEqual(body['relations']['recommends'][0]['min_version'],'1.0')

 def test_parent_control_and_output_alias_refuse_without_source_writes(self):
  with tempfile.TemporaryDirectory() as d:
   d=Path(d);source=d/'Drawer';author_fixture(source);key=key_fixture(d/'key');output=d/'package';parent=d/'.ameta';parent.write_text('ameta 1\nfile Drawer\ncomment parent metadata\n');result=build(source,output,key);self.assertNotEqual(result.returncode,0);self.assertIn('parent .ameta',result.stderr);self.assertFalse(output.exists());parent.unlink();inside=source/'output';inside.mkdir();alias=d/'alias';alias.symlink_to(inside,target_is_directory=True);result=build(source,alias/'package',key);self.assertNotEqual(result.returncode,0);self.assertFalse(list(inside.iterdir()));self.assertFalse((source/'Package.arospkg.toml').exists())
