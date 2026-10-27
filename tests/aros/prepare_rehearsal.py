#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
"""Prepare an isolated AROS command rehearsal; never contact a target.

Inputs are explicit native candidates/helpers. Keys are synthetic test keys;
only signed public fixtures and a cloned, unused root seed enter the payload.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import uuid

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / 'tests/apkg'))
from test_author import author_fixture, key_fixture, build
from test_channel import CHANNEL, seed
from test_file_channel import channel_key, publish, snapshot_document
import test_client as clients


def run(*args):
    result = subprocess.run([str(clients.CLI), *map(str, args)], capture_output=True, text=True, timeout=120)
    if result.returncode:
        raise ValueError(result.stdout + result.stderr)
    return result


def confirmed(*args):
    import re
    result = subprocess.run([str(clients.CLI), *map(str, args)], capture_output=True, text=True, timeout=120)
    match = re.search(r'--confirm ([0-9a-f]{64})', result.stderr)
    if not match: raise ValueError(result.stdout + result.stderr)
    return run(*args, '--confirm', match.group(1))


def prepare(args):
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    payload = out / 'payload/APKGTrial'; payload.mkdir(parents=True)
    workspace = out / 'host'; workspace.mkdir()
    original_cwd = Path.cwd()
    os.chdir(workspace)
    try:
        channel = Path('RAM:APKGTrialChannel')
        key_fixture(Path('author-key')); channel_key(Path('channel-key'))
        for version in ('1.2', '2.0'):
            source = Path('drawer-' + version); source.mkdir()
            drawer = source / 'Drawer'
            manifest = author_fixture(drawer, name='trial', version=version, revision=1)
            manifest.write_text(manifest.read_text().replace('arch = "x86_64"', 'arch = "aarch64"').replace('abi = "v11"', 'abi = "v1"'))
            (drawer / '.ameta').unlink()  # basic native filesystem profile first
            (drawer / 'program').write_bytes(('payload-' + version + '\n').encode())
            package = source / 'package.pkg'
            result = build(drawer, package, Path('author-key'))
            if result.returncode: raise ValueError(result.stdout + result.stderr)
            result = publish(package, channel, Path('channel-key'))
            if result.returncode: raise ValueError(result.stdout + result.stderr)
            shutil.copy2(channel / 'current', payload / ('current-' + version))
        policy = workspace / 'policy.json'; policy.write_bytes(seed())
        root = workspace / 'unused-root'
        run('init', root, 'aarch64-aros-v1-native', policy, '--yes')
        confirmed('source-add', root, 'trial-source', 'RAM:APKGTrialChannel', CHANNEL)
        shutil.copy2(root / '.apkg/state', payload / 'root-state')
        shutil.copy2(policy, payload / 'policy.json')
        shutil.copytree(channel, payload / 'channel')
        newer = next(row for row in snapshot_document(channel)['body']['entries'] if row['descriptor']['body']['version'] == '2.0')
        bad_hash = newer['archive']['sha256']
    finally:
        os.chdir(original_cwd)
    for name, source in [('apkg-core', args.cli), ('PkgManager', args.gui), ('Launch', args.launch), ('NoRequesters', args.norequesters)]:
        shutil.copy2(source, payload / name)
    (payload / 'bad.pkg').write_bytes(b'invalid package\n')
    nonce = uuid.uuid4().hex
    root = 'RAM:APKGTrialRoot'
    app = root + '/trial/Drawer'
    exe = 'SYS:APKGTrial/apkg-core'
    lines = ['; Isolated RAM-only fusion rehearsal', 'FailAt 21',
             'Echo "APKG-REHEARSAL ' + nonce + '" >RAM:APKGTrialReport.txt']
    steps = []
    def step(label, command, expected=0, destination=None):
        steps.append(dict(label=label, command=command, expected=expected))
        destination = destination or 'RAM:APKGStep.txt'
        lines.extend([command + ' >' + destination, 'Set APKGRC $RC',
                      'Echo "BEGIN ' + label + ' rc=$APKGRC" >>RAM:APKGTrialReport.txt',
                      'Type ' + destination + ' >>RAM:APKGTrialReport.txt',
                      'If NOT $APKGRC EQ ' + str(expected) + ' VAL',
                      '  Echo "FAIL ' + label + ' rc=$APKGRC" >>RAM:APKGTrialReport.txt', '  Quit 20', 'EndIf'])
    step('no-requesters', 'SYS:APKGTrial/NoRequesters')
    step('version', exe + ' --version')
    step('initialize', exe + ' init RAM:APKGFreshRoot aarch64-aros-v1-native SYS:APKGTrial/policy.json --yes')
    step('make-fixture', 'MakeDir ' + root + ' ' + root + '/.apkg RAM:APKGTrialChannel')
    step('copy-state', 'Copy SYS:APKGTrial/root-state ' + root + '/.apkg/state QUIET')
    step('copy-channel', 'Copy SYS:APKGTrial/channel/#? RAM:APKGTrialChannel/ ALL QUIET')
    step('select-v1', 'Copy SYS:APKGTrial/current-1.2 RAM:APKGTrialChannel/current QUIET')
    step('install', exe + ' legacy --root ' + root + ' install trial --yes')
    step('info-v1', exe + ' legacy --root ' + root + ' info trial')
    step('edit-config', 'Echo "personal-configuration"', destination=app + '/.hidden')
    step('select-v2', 'Copy SYS:APKGTrial/current-2.0 RAM:APKGTrialChannel/current QUIET')
    step('tamper', 'Copy SYS:APKGTrial/bad.pkg RAM:APKGTrialChannel/archives/' + bad_hash + '.pkg QUIET')
    step('refuse-tamper', exe + ' legacy --root ' + root + ' upgrade trial --yes', 10)
    step('still-v1', exe + ' legacy --root ' + root + ' info trial')
    step('restore-archive', 'Copy SYS:APKGTrial/channel/archives/' + bad_hash + '.pkg RAM:APKGTrialChannel/archives/' + bad_hash + '.pkg QUIET')
    step('upgrade', exe + ' legacy --root ' + root + ' upgrade trial --yes')
    step('info-v2', exe + ' legacy --root ' + root + ' info trial')
    step('verify', exe + ' legacy --root ' + root + ' verify trial')
    step('config', 'Type ' + app + '/.hidden')
    step('remove', exe + ' legacy --root ' + root + ' remove trial --yes')
    step('list-final', exe + ' legacy --root ' + root + ' list')
    step('kept-config', 'Type ' + app + '/.hidden')
    step('make-gui-root', 'MakeDir RAM:APKGGUIRoot RAM:APKGGUIRoot/.apkg')
    step('gui-state', 'Copy SYS:APKGTrial/root-state RAM:APKGGUIRoot/.apkg/state QUIET')
    step('gui-install', exe + ' legacy --root RAM:APKGGUIRoot install trial --yes')
    lines.extend(['Echo "PASS ' + nonce + '" >>RAM:APKGTrialReport.txt', 'Quit 0'])
    (payload / 'Trial').write_text('\n'.join(lines) + '\n')
    (payload / 'StartGUI').write_text('SYS:APKGTrial/Launch "SYS:APKGTrial/PkgManager --root RAM:APKGGUIRoot --source-profile RAM:APKGTrial.source --log RAM:APKGManager.log"\n')
    inventory = {str(p.relative_to(out / 'payload')): hashlib.sha256(p.read_bytes()).hexdigest()
                 for p in (out / 'payload').rglob('*') if p.is_file()}
    receipt = dict(format='apkg-aros-rehearsal-1', nonce=nonce, target='aarch64-aros-v1-native',
                   steps=steps, files=inventory, native_execution='not-performed', roots=['RAM:APKGFreshRoot',root,'RAM:APKGGUIRoot'])
    (out / 'prepared.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print(json.dumps(dict(result='PREPARED', steps=len(steps), files=len(inventory), output=str(out))))

if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--cli', type=Path, required=True)
    p.add_argument('--gui', type=Path, required=True)
    p.add_argument('--launch', type=Path, required=True)
    p.add_argument('--norequesters', type=Path, required=True)
    prepare(p.parse_args())
