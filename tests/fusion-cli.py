#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
"""Public default workflow in empty roots, using real TOML/.ameta publication.

The ELF header checks target/launch metadata; it is not an AROS runtime test.
No default user root, signing key or installed tool is used or changed.
"""
from contextlib import contextmanager
import hashlib
import http.server
import json
import os
import pty
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import threading

HERE=Path(__file__).resolve().parents[1]
APKG=HERE/'build/apkg'
WORKER=HERE/'build/test_pm_common'
AUTH='ab'*16
CHANNEL='cd'*16
TARGET='x86_64-aros-v11-native'
checks=0

def check(value,message):
 global checks
 checks+=1
 if not value:raise AssertionError(message)

def invoke(command,env,expect=0):
 result=subprocess.run(list(map(str,command)),env=env,capture_output=True,text=True)
 check(result.returncode==expect,f'{command}: status {result.returncode}: {result.stdout}{result.stderr}')
 return result.stdout

def commit(command,env):
 preview=invoke(command,env,16);match=re.search(r'Approved plan: ([a-f0-9]{64})',preview);check(bool(match),'concrete approved digest in preview');return invoke([*command,'--confirm',match.group(1)],env)

def source(base,name,depends=(),launch=False):
 folder=base/(name+' Drawer');folder.mkdir();program=b'helper bytes\n'
 if launch:
  program=bytearray(64);program[:7]=b'\x7fELF\x02\x01\x01';program[18:20]=(62).to_bytes(2,'little');program=bytes(program)
 (folder/'program').write_bytes(program);(folder/'program').chmod(0o755 if launch else 0o644);(folder/'.hidden').write_bytes(b'default config');(folder/'Empty').mkdir();(folder/'.ameta').write_text('ameta 1\nfile program\ncomment caf%C3%A9\nprot 0x40\nuid 42\ngid 123\ncolour red\n')
 manifest=folder.with_name(folder.name+'.arospkg.toml');manifest.write_text(f'id = "{name}"\nversion = "1.0"\nrevision = 1\nsummary = "Default workflow fixture"\ncategory = "util"\nlicense = "MIT"\nkind = "'+('app' if launch else 'data')+'"\narch = "x86_64"\nabi = "v11"\nsubdir = '+json.dumps(folder.name)+'\ndepends = '+json.dumps(list(depends))+'\ndepends_checked = true\nconfig = [".hidden"]\n'+ ('\n[launch]\nexecutable = "program"\nmode = "shell"\nworking_directory = "."\n' if launch else ''))
 return folder,manifest,program

def inventory(folder):return {str(p.relative_to(folder)):(p.stat().st_mode&0o777,hashlib.sha256(p.read_bytes()).hexdigest() if p.is_file() else None) for p in folder.rglob('*')}

@contextmanager
def serve(folder):
 class Handler(http.server.SimpleHTTPRequestHandler):
  def __init__(self,*args,**kwargs):super().__init__(*args,directory=str(folder),**kwargs)
  def log_message(self,*args):pass
 server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Handler);thread=threading.Thread(target=server.serve_forever,kwargs={'poll_interval':0.02},daemon=True);thread.start()
 try:yield f'http://127.0.0.1:{server.server_port}'
 finally:server.shutdown();server.server_close();thread.join(timeout=3)

def main():
 with tempfile.TemporaryDirectory(prefix='fusion-default-') as temporary:
  temp=Path(temporary);env=os.environ.copy();env['XDG_CONFIG_HOME']=str(temp/'unused config');env['PKG_ROOT']=str(temp/'unused legacy root');env['PKG_CHANNEL']=str(temp/'unused legacy channel')
  check('common engine' in invoke([APKG,'--version'],env),'default executable identifies common backend');check('channel-install' in invoke([APKG,'--help'],env),'default help exposes common channel workflow');check(APKG.read_bytes()==(HERE/'build/apkg-core').read_bytes(),'development alias is identical to default product')
  author=temp/'author.key';channel_key=temp/'channel.key';author_public=json.loads(invoke([APKG,'keygen',author],env))['public_key'];channel_public=json.loads(invoke([APKG,'keygen',channel_key],env))['public_key'];check(author.stat().st_mode&0o777==0o600,'private author key permissions')
  policy={'body':{'authorities':[{'id':AUTH,'keys':[{'algorithm':'ed25519','public_key':author_public}]}],'channels':[{'id':CHANNEL,'keys':[{'algorithm':'ed25519','public_key':channel_public}]}]},'format':'apkg-json-1','requires':[],'schema':'1','type':'policy'};policy_file=temp/'policy.json';policy_file.write_text(json.dumps(policy,sort_keys=True,separators=(',',':')))
  # Initialization's random identity is held with its plan in one process.
  preview_root=temp/'Preview Root';preview=subprocess.run([str(APKG),'init',str(preview_root),TARGET,str(policy_file)],env=env,stdin=subprocess.DEVNULL,capture_output=True,text=True);check(preview.returncode==16 and not preview_root.exists(),'noninteractive initialization preview grants nothing');check('Initialization preview only' in preview.stderr,'bootstrap preview has an explicit automation path')
  for correct in (False,True):
   terminal_root=temp/('Confirmed Root' if correct else 'Declined Root');master,slave=pty.openpty();child=subprocess.Popen([str(APKG),'init',str(terminal_root),TARGET,str(policy_file)],env=env,stdin=slave,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True);os.close(slave);prefix=[]
   try:
    while True:
     line=child.stdout.readline();prefix.append(line)
     if not line:raise AssertionError('missing terminal plan preview')
     match=re.search(r'Approved plan: ([a-f0-9]{64})',line)
     if match:break
    os.write(master,((match.group(1) if correct else 'wrong approval')+'\n').encode());tail,error=child.communicate(timeout=20);check(child.returncode==(0 if correct else 16),'terminal approves the retained exact plan or declines');check(terminal_root.exists()==correct,'terminal approval controls initialization')
   finally:
    os.close(master)
    if child.poll() is None:child.kill();child.wait()
  left=temp/'CLI root';right=temp/'Worker root'
  for root in (left,right):invoke([APKG,'init',root,TARGET,policy_file,'--yes'],env)
  authors=temp/'Authors';authors.mkdir();helper,_,_=source(authors,'helper');app,manifest,original_program=source(authors,'app',['helper >= 1.0'],True);channel=temp/'Public Channel'
  def publish(drawer):
   before=inventory(authors);answer=json.loads(invoke([sys.executable,HERE/'tools/apkg-publish',drawer,'--channel',channel,'--sign',author,'--authority',AUTH,'--channel-id',CHANNEL,'--channel-sign',channel_key,'--runtime','native'],env));check(answer['publication_mode']=='open' and answer['review']=='not-asserted','default publisher does not require or award review');check(inventory(authors)==before,'authoring sources and metadata stay unchanged');return answer
  publish(helper);publish(app);check(not (app/'application.json').exists(),'generated application description stays out of source')
  with serve(channel) as endpoint:
   discovery=json.loads(invoke([APKG,'channel-open',left,endpoint,CHANNEL],env));selected=next(r for r in discovery['entries'] if r['name']=='app');check(selected['review']=='not-asserted' and selected['target_matches'],'discovery exposes independent review and target facts');selection=CHANNEL+'/'+discovery['snapshot_sha256']+'/'+selected['descriptor_sha256'];command=[APKG,'channel-install',left,endpoint,*selection.split('/')];commit(command,env);invoke([WORKER,right,endpoint,'1','7',selection],env)
  left_state=json.loads((left/'.apkg/state').read_bytes())['body'];right_state=json.loads((right/'.apkg/state').read_bytes())['body'];check(left_state['installed']==right_state['installed'],'default CLI and actual worker retain identical installed records');check(left_state['channel_heads']==right_state['channel_heads'],'default CLI and actual worker retain identical replay floors');check(inventory(left/'app')==inventory(right/'app'),'installed payload and modes match across clients');check((left/'app'/'app Drawer'/'Empty').is_dir(),'empty directory retained');check((left/'app'/'app Drawer'/'program').read_bytes()==original_program,'published program bytes retained');check((left/'app'/'app Drawer'/'application.json').is_file(),'launch description generated and installed');check((left/'helper'/'helper Drawer'/'program').read_bytes()==b'helper bytes\n','dependency installed');check(any(e.get('reason')=='dependency' for e in left_state['installed']),'dependency reason retained')
  installed=left/'app'/'app Drawer';(installed/'.hidden').write_bytes(b'personal config');manifest.write_text(manifest.read_text().replace('version = "1.0"','version = "2.0"'));updated=original_program+b'updated';(app/'program').write_bytes(updated);(app/'.hidden').write_bytes(b'new default');publish(app);discovery=json.loads(invoke([APKG,'channel-open',left,channel,CHANNEL],env));selected=next(r for r in discovery['entries'] if r['name']=='app' and r['version']=='2.0');commit([APKG,'channel-install',left,channel,CHANNEL,discovery['snapshot_sha256'],selected['descriptor_sha256']],env);check((installed/'.hidden').read_bytes()==b'personal config','update preserves personal configuration');check((installed/'program').read_bytes()==updated,'update publishes selected new content');after=(left/'.apkg/state').read_bytes();upgrade_history=next(p for p in temp.glob(left.name+'.apkg-history.*') if (p/'decision').exists() and (p/'new-state').read_bytes()==after);floor=json.loads(after)['body']['channel_heads'];shutil.rmtree(channel);commit([APKG,'rollback',left,upgrade_history],env);check((installed/'program').read_bytes()==original_program,'retained rollback works after channel removal');check((installed/'.hidden').read_bytes()==b'personal config','rollback preserves personal configuration');check(json.loads((left/'.apkg/state').read_bytes())['body']['channel_heads']==floor,'package rollback keeps newer catalogue floor');(installed/'program').write_bytes(b'damage');commit([APKG,'repair',left,'app'],env);check((installed/'program').read_bytes()==original_program,'repair works offline');commit([APKG,'remove',left,'app'],env);check((installed/'.hidden').read_bytes()==b'personal config','removal preserves personal file');check(not (installed/'program').exists(),'removal discards pristine program');check(not (temp/'unused config').exists() and not (temp/'unused legacy root').exists() and not (temp/'unused legacy channel').exists(),'default workflow ignores legacy global request configuration')
  stage=temp/'Install Stage';invoke(['make','install','PREFIX=/opt/apkg alpha','DESTDIR='+str(stage)],env);prefix=stage/'opt/apkg alpha';installed_cli=prefix/'bin/apkg';check('common engine' in invoke([installed_cli,'--version'],env),'installed product uses common backend');check((prefix/'lib/libapkg.a').is_file() and not (prefix/'lib/libpkg.a').exists(),'installation provides common library only');consumer=temp/'consumer.c';consumer.write_text('#include "apkg.h"\n#include "apkg_client.h"\nint main(void) { apkg_ctx *ctx=0; apkg_open_options o={0}; o.contract_version=1; o.struct_size=sizeof o; o.root.data="/nonexistent-apkg-consumer-root"; o.root.size=31; if(apkg_open(&o,&ctx,0)) return 1; apkg_close(ctx); return 0; }\n');compiled=temp/'consumer';invoke([os.environ.get('CC','cc'),'-std=c99','-I'+str(prefix/'include'),consumer,prefix/'lib/libapkg.a','-o',compiled],env);invoke([compiled],env);check(not (prefix/'include/pkg.h').exists(),'installed public headers select common contract');installed_channel=temp/'Installed Publisher Channel';answer=json.loads(invoke([prefix/'bin/apkg-publish',app,'--channel',installed_channel,'--sign',author,'--authority',AUTH,'--channel-id',CHANNEL,'--channel-sign',channel_key,'--runtime','native'],env));check(answer['publication_mode']=='open','installed publisher uses common defaults and installed resources');check((prefix/'libexec/apkg/build/apkg').resolve()==installed_cli.resolve(),'installed authoring tools resolve the common product');built=temp/'installed-build.pkg';invoke([prefix/'bin/apkg-build',helper,'--output',built,'--sign',author,'--authority',AUTH,'--runtime','native'],env);check(built.is_file(),'installed builder works without a repository engine override');second_channel=temp/'Installed Channel Producer';invoke([prefix/'bin/apkg-channel',built,'--directory',second_channel,'--id',CHANNEL,'--sign',channel_key],env);check((second_channel/'current').is_file(),'installed channel producer uses the same common backend');missing=temp/'Missing Identity Channel';invoke([sys.executable,HERE/'tools/apkg-publish',helper,'--channel',missing,'--sign',author,'--runtime','native'],env,1);check(not missing.exists(),'missing common identities never fall back to legacy publication')
 print(f'fusion-cli: {checks} default-product checks passed; no AROS execution or live deployment')
 return 0
if __name__=='__main__':raise SystemExit(main())
