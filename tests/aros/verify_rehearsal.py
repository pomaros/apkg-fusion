#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
"""Verify an exported rehearsal report, independently of the target runner."""
import argparse
import json
from pathlib import Path
import re


def verify(prepared, report):
    if prepared.get('format') != 'apkg-aros-rehearsal-1':
        raise ValueError('unknown preparation format')
    nonce = prepared['nonce']
    lines = report.splitlines()
    if not lines or lines[0] != 'APKG-REHEARSAL ' + nonce:
        raise ValueError('missing or mismatched report identity')
    if lines[-1] != 'PASS ' + nonce or any(line.startswith('FAIL ') for line in lines):
        raise ValueError('incomplete or failed execution')
    blocks = []
    for line in lines[1:-1]:
        match = re.fullmatch(r'BEGIN ([a-z0-9-]+) rc=([0-9]+)', line)
        if match:
            blocks.append((match[1], int(match[2]), []))
        elif line.startswith(('BEGIN ', 'PASS ', 'APKG-REHEARSAL ')) or not blocks:
            raise ValueError('unexpected report structure')
        else:
            blocks[-1][2].append(line)
    observed = [(label, rc) for label, rc, _ in blocks]
    expected = [(step['label'], step['expected']) for step in prepared['steps']]
    if observed != expected or len({label for label, _, _ in blocks}) != len(blocks):
        raise ValueError('missing, reordered, duplicated or failed steps')
    output = {label: '\n'.join(body).strip() for label, _, body in blocks}
    if output['version'] != 'apkg-fusion 0.1.0-alpha (common engine, contract 1)':
        raise ValueError('wrong engine identity')
    for label, version in [('info-v1', '1.2'), ('still-v1', '1.2'), ('info-v2', '2.0')]:
        info = json.loads(output[label])
        if (info.get('name'), info.get('version'), info.get('revision'),
            info.get('arch'), info.get('abi'), info.get('signature'),
            info.get('authortrust'), info.get('installed')) != (
                'trial', version, 1, 'aarch64', 'v1', 'verified', 'accepted', True):
            raise ValueError('unexpected installed identity: ' + label)
    for label in ('edit-config', 'config', 'kept-config'):
        if output[label] != 'personal-configuration':
            raise ValueError('configuration not preserved: ' + label)
    if output['list-final'] != 'nothing installed':
        raise ValueError('package remains registered after removal')
    checked = output['verify'].splitlines()
    for required in ('Signature: verified', 'Author trust: accepted', 'changed 1',
                     'missing 0', 'unknown 0', 'ok Drawer/program',
                     'changed Drawer/.hidden [configuration]'):
        if required not in checked:
            raise ValueError('installed verification did not establish: ' + required)
    return dict(result='PASS', steps=len(blocks), nonce=nonce,
                scope='isolated RAM command rehearsal; GUI and durability separate')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--prepared', type=Path, required=True)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    try:
        verdict = verify(json.loads(args.prepared.read_text()), args.report.read_text())
    except (ValueError, KeyError, IndexError) as error:
        parser.exit(1, 'FAIL: ' + str(error) + '\n')
    print(json.dumps(verdict))
