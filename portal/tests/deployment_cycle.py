#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
"""Disposable published-artifact rehearsal; synthetic keys and a host Python payload.

This exercises package management for an AROS target from the host. Executing
Python with the host interpreter does not qualify an AROS application/runtime.
No production service, real account or installed system updater is touched.
"""
import argparse
from contextlib import contextmanager
import functools
import hashlib
import http.server
import json
import os
from pathlib import Path
import re
import shutil
import socket
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request

ROOT=Path(__file__).resolve().parents[2]

def encode(value):return json.dumps(value,sort_keys=True,separators=(',',':')).encode()
def run(*args,expected=0):
    result=subprocess.run(list(map(str,args)),capture_output=True,text=True,timeout=180)
    if result.returncode!=expected:raise AssertionError((args[0:2],result.returncode,result.stdout,result.stderr))
    return result.stdout

def approved(engine,*args):
    preview=run(engine,*args,expected=16)
    match=re.search(r'Approved plan: ([a-f0-9]{64})',preview)
    assert match,preview
    return run(engine,*args,'--confirm',match[1])

def get(url):
    with urllib.request.urlopen(url,timeout=15) as response:return response.read()

def state(root):return json.loads((root/'.apkg/state').read_bytes())['body']
def history(root):
    current=(root/'.apkg/state').read_bytes()
    return next(path for path in root.parent.glob(root.name+'.apkg-history.*') if (path/'decision').exists() and (path/'new-state').read_bytes()==current)

@contextmanager
def server(app,data,work,label,process_sink=None):
    with socket.socket() as sock:sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
    endpoint=f'http://127.0.0.1:{port}'
    settings=dict(os.environ,ASPNETCORE_ENVIRONMENT='Production',Portal__DataDir=str(data),
                  Portal__AllowLoopbackHttpPush='true',Portal__ApkgPath=str(app/'apkg'))
    with (work/(label+'.log')).open('w') as log:
        process=subprocess.Popen(['dotnet',str(app/'Portal.dll'),'--urls',endpoint],cwd=app,env=settings,stdout=log,stderr=subprocess.STDOUT)
        if process_sink is not None:process_sink(process)
        try:
            deadline=time.monotonic()+30
            while True:
                if process.poll() is not None:raise AssertionError('published portal exited; inspect '+str(work/(label+'.log')))
                try:
                    if get(endpoint+'/health').startswith(b'ok:'):break
                except (OSError,ValueError):pass
                if time.monotonic()>deadline:raise AssertionError('published portal readiness timed out')
                time.sleep(.1)
            yield endpoint
        finally:
            process.terminate()
            try:process.wait(timeout=15)
            except subprocess.TimeoutExpired:process.kill();process.wait()

class QuietHandler(http.server.SimpleHTTPRequestHandler):
    def log_message(self,*args):pass


def cycle(app,work):
    app=app.resolve();work=work.resolve()
    if work.exists():raise ValueError('work directory must not exist')
    work.mkdir(parents=True,mode=0o700)
    manifest=json.loads((app/'deployment.json').read_bytes())
    assert {path.relative_to(app).as_posix() for path in app.rglob('*') if path.is_file()}==set(manifest['files'])|{'deployment.json'}
    for name,digest in manifest['files'].items():assert hashlib.sha256((app/name).read_bytes()).hexdigest()==digest,name
    engine=app/'apkg';worker=ROOT/'build/test_pm_common'
    assert worker.exists(),'make build/test_pm_common first'
    artifacts=work/'artifacts';artifacts.mkdir()
    author=work/'author.key';stored_key=work/'stored.key';linked_key=work/'linked.key'
    for key in (author,stored_key,linked_key):run(engine,'keygen',key)
    public=lambda key:json.loads(run(engine,'key-public',key))['public_key']
    data=work/'data';(data/'state').mkdir(parents=True)
    registrations=[dict(GitHubId=index,Login='synthetic-'+channel,Name='Synthetic '+channel,Key=public(key),Channels=[channel],Files=files,Suspended=False,Since='2026-10-31T00:00:00Z')
                   for index,channel,key,files in [(101,'stored',stored_key,True),(102,'linked',linked_key,False)]]
    (data/'state/publishers.json').write_bytes(encode(registrations))
    for channel in ('stored','linked'):
        # Same marker produced by self-service registration; UI registration is
        # covered by the account tests, not by impersonating GitHub here.
        folder=data/'state'/channel;folder.mkdir(parents=True)
        (folder/'unlisted').write_text('synthetic registration\n')
    source=work/'Demo';source.mkdir();(source/'Empty').mkdir()
    archives=[]
    for version in (1,2):
        (source/'App.py').write_text(f'print("fusion-loop {version}.0")\n')
        (source/'.hidden').write_text(f'default configuration {version}\n')
        source.with_name('Demo.arospkg.toml').write_text(
            f'id = "fusion-loop"\nversion = "{version}.0"\nrevision = {version}\nsummary = "Synthetic portal lifecycle app"\n'
            'category = "util"\nlicense = "MIT"\nkind = "data"\narch = "x86_64"\nabi = "v11"\nsubdir = "Demo"\n'
            'depends = []\ndepends_checked = true\nconfig = [".hidden"]\n')
        archive=artifacts/f'app-{version}.pkg'
        run(sys.executable,ROOT/'tools/apkg-build',source,'--engine',engine,'--output',archive,'--sign',author,'--authority','1'*32,'--runtime','native')
        archives.append(archive)
    channelids={'stored':'cd'*16,'linked':'ef'*16}
    channelkeys={'stored':stored_key,'linked':linked_key}
    def publish(channel,version,endpoint,external):
        local=work/('publisher-'+channel)
        run(sys.executable,ROOT/'tools/apkg-channel',archives[version-1],'--engine',engine,'--directory',local,'--id',channelids[channel],'--sign',channelkeys[channel])
        args=[sys.executable,ROOT/'tools/apkg-portal-push',local,endpoint+'/'+channel,'--engine',engine,'--sign',channelkeys[channel],'--part-bytes','1024']
        if channel=='linked':
            head=(local/'current').read_text().strip()
            document=json.loads((local/'snapshots'/head/'channel.json').read_bytes())
            links={entry['archive']['sha256']:external+'/'+('app-1.pkg' if entry['descriptor']['body']['version']=='1.0' else 'app-2.pkg') for entry in document['body']['entries']}
            linkfile=work/'links.json';linkfile.write_bytes(encode(links));args.extend(['--links',linkfile])
        result=json.loads(run(*args));assert result['result']=='published',result
        assert json.loads(run(*args))['result']=='unchanged'
        return json.loads(get(endpoint+'/api/channels/'+channel+'/packages'))
    external=http.server.ThreadingHTTPServer(('127.0.0.1',0),functools.partial(QuietHandler,directory=str(artifacts)))
    thread=threading.Thread(target=external.serve_forever,daemon=True);thread.start()
    external_url=f'http://127.0.0.1:{external.server_port}'
    grant=lambda identity,key:dict(id=identity,keys=[dict(algorithm='ed25519',public_key=key)])
    policy=dict(format='apkg-json-1',requires=[],schema='1',type='policy',body=dict(authorities=[grant('1'*32,public(author))],channels=[grant(channelids['stored'],public(stored_key))]))
    seed=work/'policy.json';seed.write_bytes(encode(policy))
    roots=[work/'cli-root',work/'worker-root']
    baseline=None
    try:
        with server(app,data,work,'deployment-first') as endpoint:
            assert b'until further notice' in get(endpoint+'/')
            stored=publish('stored',1,endpoint,external_url);publish('linked',1,endpoint,external_url)
            baseline=stored['snapshot'];digest=stored['builds'][0]['build']['digest']
            first_page=stored['builds'][0]['page']
            page=get(endpoint+stored['builds'][0]['page']);assert b'1.0' in page
            for root in roots:run(engine,'init',root,'x86_64-aros-v11-native',seed,'--yes')
            approved(engine,'channel-install',roots[0],endpoint+'/stored',channelids['stored'],baseline,digest)
            result=run(worker,roots[1],endpoint+'/stored','1','7','/'.join((channelids['stored'],baseline,digest)))
            assert 'status=0 outcome=1; message ownership balanced' in result,result
            for root in roots:
                drawer=root/'fusion-loop/Demo'
                assert run(sys.executable,drawer/'App.py').strip()=='fusion-loop 1.0'
                assert (drawer/'Empty').is_dir()
                (drawer/'.hidden').write_bytes(b'personal configuration\n')
                (drawer/'Personal.txt').write_bytes(b'personal file\n')
            assert state(roots[0])['installed']==state(roots[1])['installed']
        # Actual process has exited: this is a quiesced backup, not a live copy.
        backup=work/'baseline.zip'
        run(sys.executable,ROOT/'tools/apkg-portal-data','backup','--data',data,'--archive',backup,'--stopped')
        with server(app,data,work,'deployment-upgrade') as endpoint:
            stored=publish('stored',2,endpoint,external_url);publish('linked',2,endpoint,external_url)
            # The public catalogue wraps full descriptor facts; select via the
            # canonical signed snapshot instead of relying on search ordering.
            local=work/'publisher-stored';snapshot=(local/'current').read_text().strip()
            document=json.loads((local/'snapshots'/snapshot/'channel.json').read_bytes())
            entry=next(item for item in document['body']['entries'] if item['descriptor']['body']['version']=='2.0')
            digest=hashlib.sha256(encode(entry['descriptor'])).hexdigest()
            approved(engine,'channel-install',roots[0],endpoint+'/stored',channelids['stored'],snapshot,digest)
            result=run(worker,roots[1],endpoint+'/stored','1','7','/'.join((channelids['stored'],snapshot,digest)))
            assert 'status=0 outcome=1' in result,result
            for root in roots:
                assert run(sys.executable,root/'fusion-loop/Demo/App.py').strip()=='fusion-loop 2.0'
                assert (root/'fusion-loop/Demo/.hidden').read_bytes()==b'personal configuration\n'
                assert 'missing 0' in run(engine,'verify',root,'fusion-loop')
            assert state(roots[0])['installed']==state(roots[1])['installed']
            assert len(list((data/'channels/linked/archives').glob('*.pkg')))==0
            assert len(list((data/'channels/linked/archives').glob('*.url')))==2
            # Earlier exact-build URL remains bound to v1 after publishing v2.
            assert b'1.0' in get(endpoint+first_page)
            assert hashlib.sha256(get(endpoint+'/stored/snapshots/'+baseline+'/channel.json')).hexdigest()==baseline
        external.shutdown();external.server_close();thread.join(timeout=5)
        # No portal/external server is running during recovery and uninstall.
        for index,root in enumerate(roots):
            retained=history(root)
            if index==0:approved(engine,'rollback',root,retained)
            else:assert 'status=0 outcome=1' in run(worker,root,retained,'1','4')
            drawer=root/'fusion-loop/Demo'
            assert run(sys.executable,drawer/'App.py').strip()=='fusion-loop 1.0'
            (drawer/'App.py').write_bytes(b'corrupted program')
            if index==0:approved(engine,'repair',root,'fusion-loop')
            else:assert 'status=0 outcome=1' in run(worker,root,'fusion-loop','1','2')
            assert run(sys.executable,drawer/'App.py').strip()=='fusion-loop 1.0'
            assert (drawer/'.hidden').read_bytes()==b'personal configuration\n'
            if index==0:approved(engine,'remove',root,'fusion-loop')
            else:assert 'status=0 outcome=1' in run(worker,root,'fusion-loop','1','1')
            assert state(root)['installed']==[]
            package=root/'fusion-loop'
            leftovers={path.relative_to(package).as_posix():path.read_bytes() for path in package.rglob('*') if path.is_file()}
            assert leftovers=={'Demo/.hidden':b'personal configuration\n','Demo/Personal.txt':b'personal file\n'},leftovers
            assert not (drawer/'Empty').exists()
            assert not Path(str(root)+'.apkg-transaction').exists()
            assert not list(root.parent.glob(root.name+'.apkg-prepare.*'))
        restored=work/'restored-data'
        run(sys.executable,ROOT/'tools/apkg-portal-data','restore','--data',restored,'--archive',backup,'--stopped')
        with server(app,restored,work,'deployment-restored') as endpoint:
            response=json.loads(get(endpoint+'/api/channels/stored/packages'))
            assert response['snapshot']==baseline and response['total']==1,response
            assert (restored/'state/publishers.json').read_bytes()==encode(registrations)
            assert (restored/'channels/stored/.portal-owner.json').read_bytes()==(data/'channels/stored/.portal-owner.json').read_bytes()
            assert b'until further notice' in get(endpoint+'/docs')
            assert get(endpoint+'/stored/current').strip().decode()==baseline
        report=dict(result='passed',deployment_manifest_sha256=hashlib.sha256((app/'deployment.json').read_bytes()).hexdigest(),
                    source_revision=manifest['revision'],dirty_source=manifest['dirty'],
                    checks=['published artifact health and warning','registered signed stored and link-only publishers','chunked publication and idempotent retry',
                            'CLI and actual worker install and upgrade','host Python payload execution v1 and v2','configuration preservation',
                            'offline CLI and worker rollback and repair','offline uninstall exact retained files and no pending transaction',
                            'quiesced private backup and exact restore','restored deployment health, identities, owner pins and baseline URLs'],
                    limitations=['synthetic publisher registration; live GitHub OAuth not exercised','host payload execution does not qualify AROS runtime',
                                 'local process/data rollback; no production cutover or power-loss claim'])
        (work/'report.json').write_text(json.dumps(report,sort_keys=True,indent=2)+'\n')
        return report
    finally:
        external.shutdown();external.server_close()

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--app',required=True,type=Path);parser.add_argument('--work',required=True,type=Path)
    args=parser.parse_args();print(json.dumps(cycle(args.app,args.work),sort_keys=True))
