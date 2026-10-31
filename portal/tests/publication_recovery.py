#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
"""Kill the real published portal around visibility; replay the signed publication."""
import argparse
import fcntl
import json
import os
from pathlib import Path
import runpy
import subprocess
import sys
import time

ROOT=Path(__file__).resolve().parents[2]
cycle=runpy.run_path(str(ROOT/'portal/tests/deployment_cycle.py'))
run,server=cycle['run'],cycle['server']

def wait_for(predicate,process,seconds=30):
    deadline=time.monotonic()+seconds
    while not predicate():
        if process.poll() is not None:raise AssertionError('publisher exited before the requested boundary')
        if time.monotonic()>deadline:raise AssertionError('publication boundary timed out')
        time.sleep(.01)

def opened(pid,path):
    expected=str(path.resolve())
    if sys.platform.startswith('linux'):
        for descriptor in Path('/proc',str(pid),'fd').iterdir():
            try:
                if os.readlink(descriptor)==expected:return True
            except FileNotFoundError:pass
        return False
    result=subprocess.run(['/usr/sbin/lsof','-a','-p',str(pid),'-Fn'],capture_output=True,text=True,check=True)
    return 'n'+expected in result.stdout.splitlines()

def exercise(app,work):
    app=app.resolve();work=work.resolve()
    if work.exists():raise ValueError('new work directory required')
    work.mkdir(parents=True,mode=0o700)
    engine=app/'apkg';source=work/'source';source.mkdir()
    run(sys.executable,ROOT/'portal/tests/common_fixture.py',ROOT,source,engine,'--rich')
    local=source/'channels/common'
    first=next(path.name for path in (local/'snapshots').iterdir() if json.loads((path/'channel.json').read_bytes())['body']['sequence']=='1')
    (local/'current').write_text(first+'\n')
    key=source/'channel-key';public=json.loads(run(engine,'key-public',key))['public_key']
    outcomes=[]
    for boundary in ('before','after'):
        data=work/(boundary+'-data');(data/'state').mkdir(parents=True)
        registration=[dict(GitHubId=101,Login='synthetic',Name='Synthetic publisher',Key=public,Channels=['common'],Files=True,Suspended=False,Since='2026-10-31T00:00:00Z')]
        (data/'state/publishers.json').write_text(json.dumps(registration))
        endpoint_dir=data/'channels/common';children=[]
        def push(endpoint,*extra):return [sys.executable,str(ROOT/'tools/apkg-portal-push'),str(local),endpoint+'/common','--engine',str(engine),'--sign',str(key),*extra]
        with server(app,data,work,'recovery-'+boundary,process_sink=children.append) as endpoint:
            assert json.loads(run(*push(endpoint,'--stage-only')))['result']=='staged'
            endpoint_dir.mkdir(parents=True,exist_ok=True)
            gate=None
            if boundary=='before':
                gate=(endpoint_dir/'.publish.lock').open('a+b');fcntl.flock(gate,fcntl.LOCK_EX)
            client=subprocess.Popen(push(endpoint),stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
            try:
                if boundary=='before':wait_for(lambda:opened(children[-1].pid,endpoint_dir/'.publish.lock'),client)
                else:wait_for(lambda:(endpoint_dir/'current').exists(),client)
                children[-1].kill();children[-1].wait(timeout=10)
            finally:
                if gate is not None:gate.close()
                try:client.communicate(timeout=20)
                except subprocess.TimeoutExpired:client.kill();client.communicate()
            assert (endpoint_dir/'current').exists()==(boundary=='after')
            assert not list(endpoint_dir.rglob('.incoming-*'))
            assert not list(endpoint_dir.glob('.current-*'))
        with server(app,data,work,'retry-'+boundary) as endpoint:
            result=json.loads(run(*push(endpoint)))
            assert result['result']==('published' if boundary=='before' else 'unchanged'),result
            assert (endpoint_dir/'current').read_text()==first+'\n'
            assert json.loads(cycle['get'](endpoint+'/api/channels/common/packages'))['total']==1
        outcomes.append(dict(boundary=boundary,result='passed'))
    report=dict(result='passed',source_revision=json.loads((app/'deployment.json').read_bytes())['revision'],checks=outcomes)
    (work/'report.json').write_text(json.dumps(report,sort_keys=True,indent=2)+'\n')
    return report

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--app',required=True,type=Path);parser.add_argument('--work',required=True,type=Path)
    args=parser.parse_args();print(json.dumps(exercise(args.app,args.work),sort_keys=True))
