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
import re
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
    namespace = args.namespace
    bundle = args.runtime_dir
    if not re.fullmatch(r'[A-Za-z][A-Za-z0-9]{0,23}', namespace):
        raise ValueError('namespace must contain 1 to 24 ASCII letters/digits, starting with a letter')
    if (not re.fullmatch(r'[A-Za-z][A-Za-z0-9_-]*:[A-Za-z0-9_./-]+', bundle)
            or any(p in ('.', '..', '') for p in bundle.split(':', 1)[1].split('/'))):
        raise ValueError('runtime directory must be an explicit DOS test path without traversal')
    report = 'RAM:' + namespace + 'TrialReport.txt'
    step_output = 'RAM:' + namespace + 'Step.txt'
    fresh = 'RAM:' + namespace + 'FreshRoot'
    gui_root = 'RAM:' + namespace + 'GUIRoot'
    channel_path = 'RAM:' + namespace + 'TrialChannel'
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    payload = out / 'payload/APKGTrial'; payload.mkdir(parents=True)
    workspace = out / 'host'; workspace.mkdir()
    original_cwd = Path.cwd()
    os.chdir(workspace)
    try:
        channel = Path(channel_path)
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
        confirmed('source-add', root, 'trial-source', channel_path, CHANNEL)
        shutil.copy2(root / '.apkg/state', payload / 'root-state')
        shutil.copy2(policy, payload / 'policy.json')
        shutil.copytree(channel, payload / 'channel',
                        ignore=shutil.ignore_patterns('.publish.lock'))
        newer = next(row for row in snapshot_document(channel)['body']['entries'] if row['descriptor']['body']['version'] == '2.0')
        bad_hash = newer['archive']['sha256']
    finally:
        os.chdir(original_cwd)
    for name, source in [('apkg-core', args.cli), ('PkgManager', args.gui), ('Launch', args.launch), ('NoRequesters', args.norequesters)]:
        shutil.copy2(source, payload / name)
    (payload / 'bad.pkg').write_bytes(b'invalid package\n')
    nonce = uuid.uuid4().hex
    root = 'RAM:' + namespace + 'TrialRoot'
    app = root + '/trial/Drawer'
    exe = bundle + '/apkg-core'
    lines = ['; Isolated RAM-only fusion rehearsal', 'FailAt 21',
             'Echo "APKG-REHEARSAL ' + nonce + '" >' + report]
    steps = []
    def step(label, command, expected=0, destination=None):
        steps.append(dict(label=label, command=command, expected=expected))
        destination = destination or step_output
        lines.extend([command + ' >' + destination, 'Set APKGRC $RC',
                      'Echo "BEGIN ' + label + ' rc=$APKGRC" >>' + report,
                      'Type ' + destination + ' >>' + report,
                      'If NOT $APKGRC EQ ' + str(expected) + ' VAL',
                      '  Echo "FAIL ' + label + ' rc=$APKGRC" >>' + report, '  Quit 20', 'EndIf'])
    step('no-requesters', bundle + '/NoRequesters')
    step('version', exe + ' --version')
    step('initialize', exe + ' init ' + fresh + ' aarch64-aros-v1-native ' + bundle + '/policy.json --yes')
    step('make-fixture', 'MakeDir ' + root + ' ' + root + '/.apkg ' + channel_path)
    step('copy-state', 'Copy ' + bundle + '/root-state ' + root + '/.apkg/state QUIET')
    step('copy-channel', 'Copy ' + bundle + '/channel/#? ' + channel_path + '/ ALL QUIET')
    step('select-v1', 'Copy ' + bundle + '/current-1.2 ' + channel_path + '/current QUIET')
    step('install', exe + ' legacy --root ' + root + ' install trial --yes')
    step('info-v1', exe + ' legacy --root ' + root + ' info trial')
    step('edit-config', 'Echo "personal-configuration"', destination=app + '/.hidden')
    step('select-v2', 'Copy ' + bundle + '/current-2.0 ' + channel_path + '/current QUIET')
    step('tamper', 'Copy ' + bundle + '/bad.pkg ' + channel_path + '/archives/' + bad_hash + '.pkg QUIET')
    step('refuse-tamper', exe + ' legacy --root ' + root + ' upgrade trial --yes', 10)
    step('still-v1', exe + ' legacy --root ' + root + ' info trial')
    step('restore-archive', 'Copy ' + bundle + '/channel/archives/' + bad_hash + '.pkg ' + channel_path + '/archives/' + bad_hash + '.pkg QUIET')
    step('upgrade', exe + ' legacy --root ' + root + ' upgrade trial --yes')
    step('info-v2', exe + ' legacy --root ' + root + ' info trial')
    step('verify', exe + ' legacy --root ' + root + ' verify trial')
    step('config', 'Type ' + app + '/.hidden')
    step('remove', exe + ' legacy --root ' + root + ' remove trial --yes')
    step('list-final', exe + ' legacy --root ' + root + ' list')
    step('kept-config', 'Type ' + app + '/.hidden')
    step('make-gui-root', 'MakeDir ' + gui_root + ' ' + gui_root + '/.apkg')
    step('gui-state', 'Copy ' + bundle + '/root-state ' + gui_root + '/.apkg/state QUIET')
    step('gui-install', exe + ' legacy --root ' + gui_root + ' install trial --yes')
    lines.extend(['Echo "PASS ' + nonce + '" >>' + report, 'Quit 0'])
    (payload / 'Trial').write_text('\n'.join(lines) + '\n')
    (payload / 'StartGUI').write_text(bundle + '/Launch "' + bundle + '/PkgManager --root ' + gui_root + ' --source-profile RAM:' + namespace + 'Trial.source --log RAM:' + namespace + 'Manager.log"\n')
    inventory = {str(p.relative_to(out / 'payload')): hashlib.sha256(p.read_bytes()).hexdigest()
                 for p in (out / 'payload').rglob('*') if p.is_file()}
    receipt = dict(format='apkg-aros-rehearsal-1', nonce=nonce, target='aarch64-aros-v1-native',
                   steps=steps, files=inventory, native_execution='not-performed', roots=[fresh, root, gui_root], runtime_dir=bundle, namespace=namespace, report=report)
    (out / 'prepared.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print(json.dumps(dict(result='PREPARED', steps=len(steps), files=len(inventory), output=str(out))))

if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--namespace', default='APKG', help='unique RAM fixture prefix for an isolated attempt')
    p.add_argument('--runtime-dir', default='SYS:APKGTrial', help='explicit target test directory containing the payload')
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--cli', type=Path, required=True)
    p.add_argument('--gui', type=Path, required=True)
    p.add_argument('--launch', type=Path, required=True)
    p.add_argument('--norequesters', type=Path, required=True)
    prepare(p.parse_args())
