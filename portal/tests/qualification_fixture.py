# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
"""Synthetic role/history fixtures signed by the common executable, not runtime evidence."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
repository,directory,engine=map(Path,sys.argv[1:4])
def run(*args):
 result=subprocess.run(list(map(str,args)),capture_output=True,text=True,check=True)
 return result.stdout
def encode(value):return json.dumps(value,sort_keys=True,separators=(',',':')).encode()
endpoint=directory/'channels/common'
head=(endpoint/'current').read_text().strip()
entry=next(entry for entry in json.loads((endpoint/'snapshots'/head/'channel.json').read_bytes())['body']['entries']
           if entry['descriptor']['body']['authority']=='1'*32)
body=entry['descriptor']['body']; digest=hashlib.sha256(encode(entry['descriptor'])).hexdigest()
reviewer=directory/'reviewer-key';run(engine,'keygen',reviewer)
public=json.loads(run(engine,'key-public',reviewer))['public_key']
author_public=json.loads(run(engine,'key-public',directory/'author-key1'))['public_key']
folders=[];previous=None
for name,kind,result,sequence in [('passed','review','passed',1),('revoked','review','revoked',2),('runtime-failed','runtime','failed',1)]:
 environment=None if kind=='review' else dict(hardware='synthetic fixture',os_build='not an AROS runtime test',test_suite='protocol-fixture/1',log_sha256='a'*64)
 record=dict(format='apkg-json-1',requires=[],schema='1',type='attestation',body=dict(
  **{field:body[field] for field in ('authority','name','version','revision','target')},descriptor_sha256=digest,
  issuer='ef'*16,kind=kind,result=result,sequence=str(sequence),issued_at=str(1234+sequence),
  supersedes=previous if kind=='review' else None,environment=environment,
  summary='Synthetic signed policy/history fixture',evidence='https://example.invalid/synthetic-fixture'))
 folder=directory/name;folder.mkdir();path=folder/'attestation.json';path.write_bytes(encode(record))
 (folder/'attestation.signature.json').write_text(run(engine,'sign-object',reviewer,'attestation',path).strip())
 folders.extend(['--attestation',folder])
 if kind=='review':previous=hashlib.sha256(encode(record)).hexdigest()
run(sys.executable,repository/'tools/apkg-channel',directory/'archive1.pkg','--engine',engine,
    '--directory',endpoint,'--id','cd'*16,'--sign',directory/'channel-key',*folders)
def grant(identity,key):return dict(id=identity,keys=[dict(algorithm='ed25519',public_key=key)])
policy=dict(format='apkg-json-1',requires=[],schema='1',type='policy',body=dict(authorities=[],
 reviewers=[grant('1'*32,author_public),grant('ef'*16,public)],testers=[grant('ef'*16,public)]))
(directory/'qualification-policy.json').write_bytes(encode(policy))
print(json.dumps(dict(snapshot=(endpoint/'current').read_text().strip(),descriptor=digest)))
