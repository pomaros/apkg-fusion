#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
"""Host rehearsal: preserve a legacy root and adopt a separately prepared copy.

All common state changes go through apkg. The receipt is immutable evidence,
not a second transaction coordinator. A partial preparation is never reused.
"""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import sys

MAX = 16 * 1024 * 1024
RECEIPT = '.apkg-legacy/import.json'
NAME = re.compile(r'[a-z0-9][a-z0-9._-]{0,254}\Z')
SHA = re.compile(r'[0-9a-f]{64}\Z')
# arospkg's relation syntax (mkindex.py): an id, and for depends an optional
# dotted-number minimum.
DEPEND = re.compile(r'([a-z][a-z0-9._-]{0,63})(?: >= ([0-9]{1,9}(?:\.[0-9]{1,9}){0,7}))?\Z')
VERSION = re.compile(r'[0-9]{1,9}(?:\.[0-9]{1,9}){0,7}\Z')
FAMILIES = ('conflicts', 'depends', 'recommends', 'replaces')

class Refused(Exception):
    pass

def require(ok, message):
    if not ok:
        raise Refused(message)

def canonical(value):
    return json.dumps(value, ensure_ascii=False, sort_keys=True,
                      separators=(',', ':'), allow_nan=False).encode('utf-8')

def digest(data):
    return hashlib.sha256(data).hexdigest()

def pairs(items):
    result = {}
    for key, value in items:
        require(key not in result, 'duplicate JSON key: ' + key)
        result[key] = value
    return result

def decode(data):
    require(len(data) <= MAX, 'metadata exceeds 16 MiB')
    return json.loads(data, object_pairs_hook=pairs,
                      parse_constant=lambda value: (_ for _ in ()).throw(Refused('non-finite JSON number')))

def regular_bytes(path, limit=MAX):
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
    with os.fdopen(fd, 'rb') as f:
        require(stat.S_ISREG(os.fstat(f.fileno()).st_mode), 'metadata is not a regular file')
        data = f.read(limit + 1)
    require(len(data) <= limit, 'metadata exceeds bound: ' + str(path))
    return data

def file_facts(path):
    st = path.lstat()
    require(stat.S_ISREG(st.st_mode) and st.st_nlink == 1, 'artifact is not a regular unlinked file')
    h = hashlib.sha256()
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
    with os.fdopen(fd, 'rb') as f:
        before = os.fstat(f.fileno())
        require((before.st_dev,before.st_ino) == (st.st_dev,st.st_ino), 'artifact replaced during hashing')
        for block in iter(lambda: f.read(65536), b''):
            h.update(block)
        after = os.fstat(f.fileno())
    require((before.st_size,before.st_mtime_ns,before.st_ctime_ns) ==
            (after.st_size,after.st_mtime_ns,after.st_ctime_ns), 'artifact changed during hashing')
    return dict(sha256=h.hexdigest(),size=st.st_size)

def path_ok(value):
    return (isinstance(value, str) and 0 < len(value) < 4096 and
            all(32 <= ord(c) <= 126 and c not in '\\:"' for c in value) and
            all(c not in ('', '.', '..') for c in value.split('/')) and
            len(value.split('/')) <= 32)

# Python's macOS build need not expose os.*xattr. Use the host ABI explicitly.
LIBC = ctypes.CDLL(None, use_errno=True)
DARWIN = sys.platform == 'darwin'
LIBC.listxattr.argtypes = [ctypes.c_char_p, ctypes.c_void_p, ctypes.c_size_t] + ([ctypes.c_int] if DARWIN else [])
LIBC.listxattr.restype = ctypes.c_ssize_t
LIBC.getxattr.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_void_p, ctypes.c_size_t] + ([ctypes.c_uint32, ctypes.c_int] if DARWIN else [])
LIBC.getxattr.restype = ctypes.c_ssize_t
LIBC.setxattr.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int] + ([ctypes.c_int] if DARWIN else [])

def attributes(path):
    buf = ctypes.create_string_buffer(16384)
    args = [os.fsencode(path), buf, len(buf)] + ([1] if DARWIN else [])
    n = LIBC.listxattr(*args)
    require(n >= 0, 'cannot inventory extended attributes: ' + str(path))
    result = {}
    for name in buf.raw[:n].split(b'\0'):
        if not name or (DARWIN and name == b'com.apple.provenance'):
            continue  # OS bookkeeping, also excluded by the common engine.
        require(name == b'user.apkg.amiga', 'unrepresented extended attribute: ' + repr(name))
        value = ctypes.create_string_buffer(16384)
        args = [os.fsencode(path), name, value, len(value)] + ([0, 1] if DARWIN else [])
        size = LIBC.getxattr(*args)
        require(size >= 0, 'cannot read extended attribute')
        result[name.decode()] = value.raw[:size].hex()
    return result

def set_attributes(path, values):
    for name, value in values.items():
        data = bytes.fromhex(value)
        args = [os.fsencode(path), name.encode(), data, len(data), 0] + ([0] if DARWIN else [])
        require(LIBC.setxattr(*args) == 0, 'cannot preserve extended attribute')

def inventory(root, exclude=()):
    require(root.is_dir() and not root.is_symlink(), 'root is not a physical directory')
    rows = []
    def walk(path, relative):
        if any(relative == item or relative.startswith(item + '/') for item in exclude):
            return
        st = path.lstat()
        require(not st.st_mode & 0o7000 and st.st_uid == os.geteuid() and st.st_gid == os.getegid(),
                'unsupported ownership or special mode: ' + relative)
        directory = stat.S_ISDIR(st.st_mode)
        require(directory or (stat.S_ISREG(st.st_mode) and st.st_nlink == 1),
                'symlink, hard link or special node: ' + relative)
        row = dict(path=relative, type='directory' if directory else 'file',
                   mode=stat.S_IMODE(st.st_mode), attributes=attributes(path))
        if directory:
            rows.append(row)
            for child in sorted(path.iterdir(), key=lambda p: p.name):
                walk(child, (relative + '/' if relative else '') + child.name)
        else:
            h = hashlib.sha256()
            fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
            with os.fdopen(fd, 'rb') as f:
                before = os.fstat(f.fileno())
                require((before.st_dev, before.st_ino) == (st.st_dev, st.st_ino), 'file replaced during inventory')
                for block in iter(lambda: f.read(65536), b''):
                    h.update(block)
                after = os.fstat(f.fileno())
            require((before.st_size, before.st_mtime_ns, before.st_ctime_ns) ==
                    (after.st_size, after.st_mtime_ns, after.st_ctime_ns) and
                    path.lstat().st_ino == st.st_ino, 'file changed during inventory')
            row.update(sha256=h.hexdigest(), size=st.st_size)
            rows.append(row)
        require(len(rows) <= 10000, 'inventory exceeds 10,000 nodes')
    walk(root, '')
    rows.sort(key=lambda r: r['path'])
    require(len(canonical(rows)) <= MAX, 'inventory exceeds metadata bound')
    return rows

def identity(path):
    st = path.lstat()
    require(stat.S_ISDIR(st.st_mode), 'directory replaced by another node')
    return [st.st_dev, st.st_ino]

def legacy_relations(record):
    """Each family as a set of (name, minimum version); '0' means any. Names and
    minimums are what a legacy registry can state. Authority, target and
    revision floors come only from the signed artifact."""
    out = {}
    for family in ('depends', 'conflicts', 'replaces'):
        items = record.get(family, [])
        require(isinstance(items, list) and len(items) <= 16, 'invalid legacy relation list: ' + family)
        found = set()
        for item in items:
            hit = DEPEND.match(item) if isinstance(item, str) else None
            require(hit and (family == 'depends' or hit.group(2) is None), 'unreadable legacy relation: ' + family)
            found.add((hit.group(1), hit.group(2) or '0'))
        require(len(found) == len(items), 'duplicate legacy relation: ' + family)
        out[family] = found
    items = record.get('recommends', [])
    require(isinstance(items, list) and len(items) <= 16, 'invalid legacy relation list: recommends')
    found = set()
    for item in items:
        require(isinstance(item, dict) and 'id' in item and not set(item) - {'id', 'min_version', 'reason'} and
                isinstance(item['id'], str) and DEPEND.match(item['id']) and ' ' not in item['id'] and
                isinstance(item.get('reason', ''), str) and
                ('min_version' not in item or (isinstance(item['min_version'], str) and VERSION.match(item['min_version']))),
                'unreadable legacy relation: recommends')
        found.add((item['id'], item.get('min_version') or '0'))
    require(len(found) == len(items), 'duplicate legacy relation: recommends')
    out['recommends'] = found
    names = [name for family in ('depends', 'conflicts', 'replaces') for name, _ in out[family]]
    require(record.get('name') not in names, 'a legacy relation names its own package')
    return out

def package_evidence(record, raw):
    allowed = {'schema','name','version','revision','arch','abi','subdir','installed',
               'origin','archive_sha256','archive_size','reason','relations_schema','dir',
               'requires_system','icon','previous','launch','contents','post_install_notes',
               'depends','recommends','conflicts','replaces'}
    require(isinstance(record, dict) and not set(record) - allowed, 'unknown legacy record fields')
    require(type(record.get('schema')) is int and record['schema'] == 1, 'unsupported legacy schema')
    name = record.get('name')
    require(isinstance(name, str) and NAME.fullmatch(name) and name != 'db' and record.get('dir') == name,
            'ambiguous package/drawer identity')
    require(isinstance(record.get('version'), str) and record['version'], 'missing version')
    require(type(record.get('revision', 0)) is int and 0 <= record.get('revision', 0) < 2**64, 'invalid revision')
    require(record.get('reason', 'unrecorded') in ('explicit','dependency','unrecorded'), 'unknown installation reason')
    require(type(record.get('relations_schema', 1)) is int and record.get('relations_schema', 1) == 1, 'unsupported relation schema')
    require(isinstance(record.get('arch'), str) and record['arch'], 'unrecorded CPU requires an explicit mapping policy')
    require('abi' not in record or isinstance(record['abi'], str), 'invalid ABI fact')
    require(isinstance(record.get('archive_sha256'), str) and SHA.fullmatch(record['archive_sha256']) and
            type(record.get('archive_size')) is int and 0 <= record['archive_size'] < 2**64, 'invalid original archive identity')
    for field in ('origin','subdir','installed'):
        require(isinstance(record.get(field), str), 'invalid legacy text field: ' + field)
    require(isinstance(record.get('requires_system', []), list) and not record.get('requires_system'),
            'legacy semantics require an explicit adapter: requires_system')
    legacy_relations(record)
    icon = record.get('icon', {})
    require(isinstance(icon, dict) and icon.get('state', 'none') in ('none','kept-existing'),
            'owned root icon placement is not mapped')
    contents = record.get('contents')
    require(isinstance(contents, list), 'recorded payload baseline is unavailable')
    baseline = []
    for item in contents:
        require(isinstance(item, dict) and set(item) == {'path','sha256','size'} and
                path_ok(item['path']) and isinstance(item['sha256'], str) and SHA.fullmatch(item['sha256']) and
                type(item['size']) is int and 0 <= item['size'] < 2**64, 'invalid payload baseline')
        baseline.append(dict(path=item['path'], sha256=item['sha256'], size=str(item['size'])))
    baseline.sort(key=lambda f: f['path'])
    require(len({f['path'].casefold() for f in baseline}) == len(baseline), 'duplicate or case-aliased baseline')
    return dict(body=dict(baseline=baseline, reason=record.get('reason','unrecorded'),
                          record=raw.hex(), source='arospkg-v1'),
                format='apkg-json-1', requires=[], schema='1', type='adoption')

def inspect_source(source, mapping):
    rows = inventory(source)
    for name in ('.pkg','.apkg','.apkg-legacy'):
        require(not (source/name).exists(), 'mixed or already converted source root: ' + name)
    require(not (source/'db/lock.owner').exists(), 'legacy lock owner requires inspection')
    transactions = source/'db/transactions'
    require(not transactions.exists() or not any(transactions.iterdir()), 'legacy transactions require recovery')
    require(not any(Path(r['path']).name == 'self-update.json' for r in rows), 'self-update state requires recovery inspection')
    require(isinstance(mapping, dict) and set(mapping) == {'format','legacy_root','packages'} and
            mapping['format'] == 'apkg-legacy-import-1' and isinstance(mapping['legacy_root'], str), 'invalid explicit mapping')
    require(isinstance(mapping['packages'], list) and 0 < len(mapping['packages']) <= 128, 'mapping count outside 1..128')
    mapped = {}
    for item in mapping['packages']:
        require(isinstance(item, dict) and set(item) == {'name','artifact'} and
                isinstance(item['name'], str) and item['name'] not in mapped and
                isinstance(item['artifact'], str), 'duplicate or invalid explicit artifact mapping')
        artifact = Path(item['artifact']).absolute()
        require(not artifact.is_symlink() and artifact.exists(), 'artifact missing or symlinked')
        mapped[item['name']] = artifact
    records = {}; packages = []; artifacts = []; observations = []
    installed = source/'db/installed'
    require(installed.is_dir(), 'legacy installed registry is unavailable')
    for path in sorted(installed.iterdir()):
        raw = regular_bytes(path); record = decode(raw); evidence = package_evidence(record, raw)
        name = record['name']
        require(path.name == name + '.json' and name not in records and name in mapped, 'unmapped or ambiguous registry file')
        require((source/name).is_dir(), 'installed bundle directory is missing')
        records[name] = record
        artifact = mapped[name]
        facts = inventory(artifact) if artifact.is_dir() else file_facts(artifact)
        artifacts.append(dict(path=str(artifact), facts=facts))
        packages.append(dict(artifact=str(artifact), evidence=evidence))
        previous = record.get('previous', {})
        require(isinstance(previous, dict), 'invalid prior-history references')
        history = {}
        for key in ('registry','archive'):
            ref = previous.get(key, '')
            require(isinstance(ref, str), 'invalid history reference')
            prefix = mapping['legacy_root'].rstrip('/') + '/'
            relative = ref[len(prefix):] if mapping['legacy_root'] and ref.startswith(prefix) else None
            retained = bool(relative and path_ok(relative) and (source/relative).is_file())
            history[key] = dict(reference=ref, bytes='retained-unverified' if retained else 'unavailable-or-unresolved')
        observations.append(dict(name=name, original_authentication='unknown',
            abi='recorded' if record.get('abi') else 'unrecorded',
            revision='recorded' if 'revision' in record else 'unrecorded; compatibility default 0',
            relations=('recorded-empty' if not any(legacy_relations(record).values()) else 'recorded')
                if 'relations_schema' in record else 'unrecorded',
            launch='original record retained; no launch performed', history=history,
            legacy_rollback='not converted; retained records are not common rollback history'))
    require(set(records) == set(mapped), 'mapping and installed registry sets differ')
    # The recorded set must be consistent on its own before anything is copied.
    for name, record in records.items():
        relations = legacy_relations(record)
        for needed, _ in relations['depends']:
            require(needed in records, 'recorded dependency is not installed: %s needs %s' % (name, needed))
        for family in ('conflicts', 'replaces'):
            for other, _ in relations[family]:
                require(other not in records, 'recorded %s names an installed package: %s, %s' % (family, name, other))
    manifest = dict(body=dict(packages=packages), format='apkg-json-1', requires=[], schema='1', type='adoption-set')
    return rows, records, artifacts, observations, manifest

def run(apkg, *args, expected=0):
    result = subprocess.run([str(apkg), *map(str,args)], capture_output=True)
    require(result.returncode == expected, 'common engine refused: ' + result.stderr.decode(errors='replace') + result.stdout.decode(errors='replace')[:2000])
    return result.stdout

def write_new(path, data):
    with path.open('xb') as f:
        f.write(data);f.flush();os.fsync(f.fileno())

def copy_inventory(source, destination, rows):
    os.mkdir(destination, 0o700)  # Exclusive claim; partial preparations are never overwritten.
    for row in rows:
        if not row['path']:
            continue
        target = destination/row['path']
        if row['type'] == 'directory':
            target.mkdir(mode=0o700)
        else:
            fd = os.open(source/row['path'], os.O_RDONLY | os.O_NOFOLLOW)
            with os.fdopen(fd,'rb') as src, target.open('xb') as dst:
                for block in iter(lambda: src.read(65536), b''):
                    dst.write(block)
                dst.flush();os.fsync(dst.fileno())
    for row in reversed(rows):
        target = destination/row['path']
        target.chmod(row['mode']);set_attributes(target,row['attributes'])
    require(inventory(destination) == rows, 'copied tree differs from captured source')

def validate_selected(records, plan):
    installed = plan['body']['new_state']['body']['installed']
    require(len(installed) == len(records), 'common plan changed the import set')
    for entry in installed:
        body = entry['descriptor']['body']; record = records.get(body['name'])
        require(record is not None and body['version'] == record['version'] and
                body['revision'] == str(record.get('revision',0)), 'artifact version/revision mapping differs from legacy record')
        target = body['target'].split('-')
        require(len(target) == 4 and target[0] == record.get('arch') and
                (not record.get('abi') or target[2] == record['abi']), 'artifact target mapping differs from legacy facts')
        if 'relations_schema' in record:
            signed = body.get('relations', {})
            want = legacy_relations(record)
            for family in FAMILIES:
                got = {(r['name'], r['min_version']) for r in signed.get(family, [])}
                require(len(got) == len(signed.get(family, [])) and got == want[family],
                        'signed relations differ from recorded relations: ' + family)

def prepare(args):
    require(args.quiescent, 'prepare requires explicit --quiescent; no legacy writer may run')
    source = args.source.resolve(strict=True)
    destination = args.destination.parent.resolve(strict=True)/args.destination.name
    require(not args.source.is_symlink() and source != destination and source not in destination.parents and
            destination not in source.parents, 'source/destination overlap or symlink')
    require(not destination.exists() and not destination.is_symlink(), 'destination already exists; use a fresh copy')
    mapping = decode(regular_bytes(args.mapping)); policy = regular_bytes(args.policy)
    rows, records, artifacts, observations, manifest = inspect_source(source,mapping)
    source_id = identity(source)
    copy_inventory(source,destination,rows)
    require(identity(source) == source_id and inventory(source) == rows, 'source changed during copying')
    archive = destination/'.apkg-legacy';archive.mkdir(mode=0o700)
    (destination/'db').rename(archive/'db')
    write_new(archive/'mapping.json',canonical(mapping))
    write_new(archive/'policy.json',policy)
    write_new(archive/'adoption-set.json',canonical(manifest))
    run(args.apkg,'init',destination,args.target,archive/'policy.json','--yes')
    raw_plan = run(args.apkg,'adopt-set',destination,archive/'adoption-set.json','--json',expected=16).strip()
    plan = decode(raw_plan); require(canonical(plan) == raw_plan, 'engine returned a noncanonical plan')
    validate_selected(records,plan)
    require(identity(source) == source_id and inventory(source) == rows, 'source changed during preparation')
    body = dict(source=str(source),source_identity=source_id,source_inventory=rows,
                destination=str(destination),destination_identity=identity(destination),
                artifacts=artifacts,observations=observations,plan=plan,
                copied_inventory=inventory(destination), quiescent_operator_assertion=True)
    receipt = canonical(dict(format='apkg-legacy-copy-1',body=body))
    require(len(receipt) <= MAX,'prepared receipt exceeds bound')
    write_new(destination/RECEIPT,receipt)
    return dict(status='prepared',destination=str(destination),approval=digest(receipt),
                observations=observations,plan_sha256=digest(raw_plan))

def commit(args):
    destination = args.destination.resolve(strict=True)
    require(not args.destination.is_symlink(), 'destination is symlinked')
    raw = regular_bytes(destination/RECEIPT); receipt = decode(raw)
    require(canonical(receipt) == raw and receipt.get('format') == 'apkg-legacy-copy-1', 'invalid preparation receipt')
    require(args.confirm == digest(raw), 'migration confirmation does not match exact preparation')
    body = receipt['body']
    require(str(destination) == body['destination'] and identity(destination) == body['destination_identity'], 'prepared destination was replaced or moved')
    # Only this approved plan may be recovered automatically. A different
    # operation requires its own explicit recovery, even in the same root.
    transaction = destination.with_name(destination.name + '.apkg-transaction')
    if transaction.exists() or transaction.is_symlink():
        require(not transaction.is_symlink() and transaction.is_dir() and
                regular_bytes(transaction/'plan') == canonical(body['plan']),
                'pending transaction belongs to another plan; recover it separately')
    # Decided recovery is deliberately independent of original sources.
    run(args.apkg,'recover',destination)
    state = regular_bytes(destination/'.apkg/state')
    expected = canonical(body['plan']['body']['new_state'])
    if state == expected:
        return dict(status='imported',destination=str(destination),already_committed=True)
    require(inventory(destination,exclude=(RECEIPT,)) == body['copied_inventory'], 'prepared copy changed; refuse re-planning')
    source = Path(body['source'])
    require(identity(source) == body['source_identity'] and inventory(source) == body['source_inventory'], 'source changed before commit')
    for item in body['artifacts']:
        path = Path(item['path'])
        require(not path.is_symlink(), 'artifact replaced by symlink')
        facts = inventory(path) if path.is_dir() else file_facts(path)
        require(facts == item['facts'], 'artifact changed before commit')
    plan = canonical(body['plan'])
    run(args.apkg,'adopt-set',destination,destination/'.apkg-legacy/adoption-set.json','--confirm',digest(plan))
    require(regular_bytes(destination/'.apkg/state') == expected, 'committed state differs from approved plan')
    return dict(status='imported',destination=str(destination),already_committed=False)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--apkg',type=Path,default=Path(__file__).resolve().parents[1]/'build/apkg')
    commands = parser.add_subparsers(dest='command',required=True)
    p = commands.add_parser('prepare');p.add_argument('source',type=Path);p.add_argument('destination',type=Path)
    p.add_argument('mapping',type=Path);p.add_argument('policy',type=Path);p.add_argument('target');p.add_argument('--quiescent',action='store_true')
    p = commands.add_parser('commit');p.add_argument('destination',type=Path);p.add_argument('--confirm',required=True)
    args = parser.parse_args()
    try:
        result = prepare(args) if args.command == 'prepare' else commit(args)
        print(canonical(result).decode());return 0
    except (Refused,OSError,ValueError,KeyError,TypeError,RecursionError) as error:
        print('migration refused: ' + str(error),file=sys.stderr);return 1

if __name__ == '__main__':
    sys.exit(main())
