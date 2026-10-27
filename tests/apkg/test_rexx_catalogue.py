# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
"""ARexx catalogue answers use signed common observations, offline."""
import json
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from support import AUTH, TARGET, descriptor, hashlib, sign, state, wire
from test_channel import CHANNEL, CP, CS, catalog, entry, seed
from test_client import WORKER


def offer(name, version='1.0', target=TARGET, authority=AUTH):
    body = json.loads(descriptor(name=name, target=target))['body']
    body.update(version=version, authority=authority)
    return entry(wire('descriptor', body))


def publish(endpoint, entries):
    data = catalog(entries)
    digest = hashlib.sha256(data).hexdigest()
    folder = endpoint / 'snapshots' / digest
    folder.mkdir(parents=True)
    (folder / 'channel.json').write_bytes(data)
    (folder / 'channel.signature.json').write_bytes(sign(data, CP, CS, 'channel'))
    (endpoint / 'current').write_text(digest + '\n')


def query(root, endpoint, match='', change=None):
    process = subprocess.Popen([str(WORKER), str(root), str(endpoint), '45',
                                '1' if change else '0', CHANNEL, match],
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, text=True)
    if change:
        assert process.stdout.readline() == 'loaded\n'
        change()
    out, err = process.communicate('\n' if change else '', timeout=60)
    assert process.returncode == 0, out + err
    status, _, text = out.partition('\n')
    return int(status.split()[1]), text.rstrip('\n')


class RexxCatalogue(unittest.TestCase):
    def test_current_target_names_deduplicate_and_filter_without_network_or_writes(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp); root = d / 'root'; endpoint = d / 'channel'
            before = state(root, policy=seed()); name = 'n' * 255
            publish(endpoint, [offer(name), offer(name, '2.0'), offer('other'),
                               offer('wrong-target', target='i386-aros-v0-native')])
            code, text = query(root, endpoint)
            self.assertEqual(code, 0); self.assertEqual(set(text.split()), {name, 'other'})
            self.assertEqual(query(root, endpoint, 'missing'), (0, ''))
            self.assertEqual(query(root, endpoint, 'nnn', lambda: shutil.rmtree(endpoint)), (0, name))
            self.assertEqual((root / '.apkg/state').read_bytes(), before)
            self.assertEqual(sorted(p.name for p in d.iterdir()), ['root'])

    def test_current_channel_trust_is_rechecked_for_retained_observation(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp); root = d / 'root'; endpoint = d / 'channel'
            state(root, policy=seed()); publish(endpoint, [offer('demo')])
            self.assertEqual(query(root, endpoint, change=lambda: state(root, 2, policy=seed(channels=False))), (12, ''))

    def test_different_authors_cannot_be_collapsed_into_one_unqualified_name(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp); root = d / 'root'; endpoint = d / 'channel'
            state(root, policy=seed())
            publish(endpoint, [offer('same'), offer('same', authority='12' * 16), offer('unique')])
            self.assertEqual(query(root, endpoint), (14, ''))
            self.assertEqual(query(root, endpoint, 'unique'), (0, 'unique'))

    def test_oversized_answer_refuses_whole_result_and_match_can_narrow_it(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp); root = d / 'root'; endpoint = d / 'channel'
            state(root, policy=seed())
            names = ['p%03d-' % i + 'x' * 250 for i in range(260)]
            publish(endpoint, [offer(name) for name in names])
            self.assertEqual(query(root, endpoint), (3, ''))
            self.assertEqual(query(root, endpoint, 'p000-'), (0, names[0]))

if __name__ == '__main__':
    unittest.main()
