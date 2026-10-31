#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
"""Operator-side full cycle against an existing HTTPS portal, with synthetic data.

Prepare prints public publisher grants. Install those grants temporarily on the
portal before run. Private author/channel keys remain on the operator machine.
This does not test AROS execution or live GitHub account consent.
"""
import argparse
import hashlib
import json
from pathlib import Path
import runpy
import subprocess
import urllib.error
import sys

ROOT=Path(__file__).resolve().parents[2]
helpers=runpy.run_path(str(ROOT/'portal/tests/deployment_cycle.py'))
run,approved,state,history,get,encode=(helpers[name] for name in ('run','approved','state','history','get','encode'))
AUTH='ad'*16
CHANNELS={'stored':('fusion-check-store','a1'*16),'linked':('fusion-check-linked','b2'*16)}

def public(engine,key):return json.loads(run(engine,'key-public',key))['public_key']

def prepare(engine,work):
    if work.exists():raise ValueError('prepare needs a new work directory')
    work.mkdir(parents=True,mode=0o700)
    for name in ('author','stored','linked'):run(engine,'keygen',work/(name+'.key'))
    source=work/'LiveProbe';source.mkdir();(source/'Empty').mkdir()
    for version in (1,2):
        (source/'App.py').write_text(f'print("fusion-live-check {version}.0")\n')
        (source/'.hidden').write_text(f'default {version}\n')
        source.with_name('LiveProbe.arospkg.toml').write_text(
            f'id = "fusion-live-check"\nversion = "{version}.0"\nrevision = {version}\nsummary = "Synthetic deployment check; no AROS runtime qualification"\n'
            'category = "util"\nlicense = "MIT"\nkind = "data"\narch = "x86_64"\nabi = "v11"\nsubdir = "LiveProbe"\n'
            'depends = []\ndepends_checked = true\nconfig = [".hidden"]\n')
        run(sys.executable,ROOT/'tools/apkg-build',source,'--engine',engine,'--output',work/f'app-{version}.pkg','--sign',work/'author.key','--authority',AUTH,'--runtime','native')
    grants=';'.join('deployment-'+kind+':'+public(engine,work/(kind+'.key'))+':'+channel+(':files' if kind=='stored' else '') for kind,(channel,identity) in CHANNELS.items())
    (work/'publisher-grants.json').write_text(json.dumps({'Portal__SignedKeys':grants}))
    return dict(result='prepared',grants=grants,channels=[channel for channel,identity in CHANNELS.values()])

def publish(command,base,channel,snapshot,previous):
    for attempt in range(3):
        result=subprocess.run(list(map(str,command)),capture_output=True,text=True,timeout=180)
        if result.returncode==0:
            answer=json.loads(result.stdout)
            assert answer['snapshot']==snapshot and answer['result'] in ('published','unchanged'),answer
            return answer
        if not any(f'HTTP {status}:' in result.stderr for status in (503,504)):
            raise AssertionError(result.stderr)
        # An uncertain response permits only the same proposal after observing its head.
        try:visible=get(base+'/'+channel+'/current').decode().strip()
        except urllib.error.HTTPError as error:
            if error.code!=404:raise
            visible=None
        if visible not in (snapshot,previous):raise AssertionError('Head changed during uncertain publication')
    raise AssertionError('Publication remains uncertain after bounded same-snapshot retries')

def exercise(engine,worker,work,base):
    if not base.startswith('https://'):raise ValueError('live checks require HTTPS')
    for kind,(channel,identity) in CHANNELS.items():
        try:get(base+'/'+channel+'/current')
        except Exception as error:
            if not isinstance(error,urllib.error.HTTPError) or error.code!=404:raise
        else:raise ValueError('test channel exists; refusing to change it')
    roots=[work/'cli-root',work/'worker-root']
    grant=lambda identity,key:dict(id=identity,keys=[dict(algorithm='ed25519',public_key=key)])
    policy=dict(format='apkg-json-1',requires=[],schema='1',type='policy',body=dict(authorities=[grant(AUTH,public(engine,work/'author.key'))],
        channels=[grant(identity,public(engine,work/(kind+'.key'))) for kind,(channel,identity) in CHANNELS.items()]))
    seed=work/'policy.json';seed.write_bytes(encode(policy))
    for root in roots:run(engine,'init',root,'x86_64-aros-v11-native',seed,'--yes')
    snapshots={};digests={};first_page=None
    for version in (1,2):
        for kind,(channel,identity) in CHANNELS.items():
            local=work/('publisher-'+kind)
            run(sys.executable,ROOT/'tools/apkg-channel',work/f'app-{version}.pkg','--engine',engine,'--directory',local,'--id',identity,'--sign',work/(kind+'.key'))
            snapshot=(local/'current').read_text().strip();document=json.loads((local/'snapshots'/snapshot/'channel.json').read_bytes())
            entry=next(item for item in document['body']['entries'] if item['descriptor']['body']['version']==str(version)+'.0')
            digest=hashlib.sha256(encode(entry['descriptor'])).hexdigest()
            command=[sys.executable,ROOT/'tools/apkg-portal-push',local,base+'/'+channel,'--engine',engine,'--sign',work/(kind+'.key'),'--part-bytes','1024']
            if kind=='linked':
                links={item['archive']['sha256']:base+'/'+CHANNELS['stored'][0]+'/archives/'+item['archive']['sha256']+'.pkg' for item in document['body']['entries']}
                path=work/'links.json';path.write_bytes(encode(links));command+=['--links',path]
            result=publish(command,base,channel,snapshot,snapshots.get(kind))
            assert json.loads(run(*command))['result']=='unchanged'
            catalogue=json.loads(get(base+'/api/channels/'+channel+'/packages'));assert catalogue['total']==version
            snapshots[kind]=snapshot;digests[kind]=digest
            if kind=='stored' and version==1:first_page=catalogue['builds'][0]['page']
        channel,identity=CHANNELS['stored']
        approved(engine,'channel-install',roots[0],base+'/'+channel,identity,snapshots['stored'],digests['stored'])
        output=run(worker,roots[1],base+'/'+channel,'1','7','/'.join((identity,snapshots['stored'],digests['stored'])))
        assert 'status=0 outcome=1; message ownership balanced' in output,output
        assert state(roots[0])['installed']==state(roots[1])['installed']
        for root in roots:
            drawer=root/'fusion-live-check/LiveProbe'
            assert run(sys.executable,drawer/'App.py').strip()==f'fusion-live-check {version}.0'
            assert (drawer/'Empty').is_dir()
            if version==1:
                (drawer/'.hidden').write_bytes(b'personal configuration\n');(drawer/'Personal.txt').write_bytes(b'personal file\n')
            assert (drawer/'.hidden').read_bytes()==b'personal configuration\n'
        assert b'1.0' in get(base+first_page)
    linked_root=work/'linked-root';run(engine,'init',linked_root,'x86_64-aros-v11-native',seed,'--yes')
    channel,identity=CHANNELS['linked']
    approved(engine,'channel-install',linked_root,base+'/'+channel,identity,snapshots['linked'],digests['linked'])
    assert run(sys.executable,linked_root/'fusion-live-check/LiveProbe/App.py').strip()=='fusion-live-check 2.0'
    # Recovery uses retained local content; the staging suite separately stops its HTTP listener.
    for index,root in enumerate(roots):
        retained=history(root)
        if index==0:approved(engine,'rollback',root,retained)
        else:assert 'status=0 outcome=1' in run(worker,root,retained,'1','4')
        drawer=root/'fusion-live-check/LiveProbe';assert run(sys.executable,drawer/'App.py').strip()=='fusion-live-check 1.0'
        (drawer/'App.py').write_bytes(b'damaged program')
        if index==0:approved(engine,'repair',root,'fusion-live-check')
        else:assert 'status=0 outcome=1' in run(worker,root,'fusion-live-check','1','2')
        assert run(sys.executable,drawer/'App.py').strip()=='fusion-live-check 1.0'
        if index==0:approved(engine,'remove',root,'fusion-live-check')
        else:assert 'status=0 outcome=1' in run(worker,root,'fusion-live-check','1','1')
        remaining={path.relative_to(root/'fusion-live-check').as_posix():path.read_bytes() for path in (root/'fusion-live-check').rglob('*') if path.is_file()}
        assert remaining=={'LiveProbe/.hidden':b'personal configuration\n','LiveProbe/Personal.txt':b'personal file\n'}
        assert state(root)['installed']==[]
        assert not (drawer/'Empty').exists() and not Path(str(root)+'.apkg-transaction').exists()
        assert not list(root.parent.glob(root.name+'.apkg-prepare.*'))
    approved(engine,'remove',linked_root,'fusion-live-check');assert state(linked_root)['installed']==[]
    assert not (linked_root/'fusion-live-check').exists()
    assert b'until further notice' in get(base+'/')
    report=dict(result='passed',endpoint=base,operator_binaries=dict(engine_sha256=hashlib.sha256(engine.read_bytes()).hexdigest(),
        worker_sha256=hashlib.sha256(worker.read_bytes()).hexdigest(),driver_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest()),checks=['HTTPS signed stored and link-only publication v1/v2','resumable upload and idempotent retry','exact historical build URL',
        'CLI and actual worker install/upgrade','external archive redirect consumed by common client','host payload execution v1/v2','configuration preservation',
        'CLI and worker rollback/repair/uninstall','exact retained personal files and no pending transactions','migration notice'],
        limitations=['host execution does not qualify AROS runtime','live GitHub account consent not exercised'])
    (work/'report.json').write_text(json.dumps(report,sort_keys=True,indent=2)+'\n')
    return report

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('phase',choices=['prepare','run']);parser.add_argument('--engine',required=True,type=Path)
    parser.add_argument('--worker',type=Path,default=ROOT/'build/test_pm_common');parser.add_argument('--work',required=True,type=Path);parser.add_argument('--url',default='https://aros-pkg.azurewebsites.net')
    args=parser.parse_args();print(json.dumps(prepare(args.engine.resolve(),args.work.resolve()) if args.phase=='prepare' else exercise(args.engine.resolve(),args.worker.resolve(),args.work.resolve(),args.url.rstrip('/')),sort_keys=True))
