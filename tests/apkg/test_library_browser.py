# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
"""The native main-list model combines exact installed and signed candidate rows."""
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from support import *
import test_channel as channel
import test_client as clients
import test_operations as operations


def described(name='demo', version='1.0', summary=None, category=None, target=TARGET):
    body = json.loads(descriptor(name=name, target=target))['body']
    body['version'] = version
    presentation = {}
    if summary is not None: presentation['summary'] = summary
    if category is not None: presentation['category'] = category
    if presentation: body['presentation'] = presentation
    return wire('descriptor', body)


def fixture(directory, installed, candidates):
    root, endpoint = directory / 'root', directory / 'channel'
    state(root, policy=channel.seed())
    code, ctx = open_ctx(root, 1)
    assert code == 0
    for data in installed: operations.install(ctx, data)
    lib.apkg_close(ctx)
    entries = [channel.entry(item) for item in candidates]
    entries.sort(key=lambda item: hashlib.sha256(json.dumps(item['descriptor'],
        ensure_ascii=False, sort_keys=True, separators=(',', ':')).encode()).digest())
    data = wire('channel', dict(entries=entries, id=channel.CHANNEL, sequence='1'))
    digest = hashlib.sha256(data).hexdigest()
    folder = endpoint / 'snapshots' / digest
    folder.mkdir(parents=True)
    (folder / 'channel.json').write_bytes(data)
    (folder / 'channel.signature.json').write_bytes(sign(data, channel.CP, channel.CS, 'channel'))
    (endpoint / 'current').write_text(digest + '\n')
    return root, endpoint


def query(root, endpoint, filter=0, term='', category='', fault=0):
    before = (root / '.apkg/state').read_bytes()
    result = subprocess.run([str(clients.WORKER), str(root), str(endpoint) if endpoint else '-', '50',
        str(filter), channel.CHANNEL, term, category, str(fault)], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    assert (root / '.apkg/state').read_bytes() == before
    lines = result.stdout.splitlines()
    code = int(lines[0].split()[1])
    categories = [line.split('\t', 1)[1] for line in lines[1:] if line.startswith('category\t')]
    rows = [line.split('\t')[1:] for line in lines[1:] if line.startswith('row\t')]
    return code, categories, rows


def show(root, endpoint, name):
    before = (root / '.apkg/state').read_bytes()
    result = subprocess.run([str(clients.WORKER), str(root), str(endpoint) if endpoint else '-',
        '55', '0', channel.CHANNEL, name, '', '0'], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    assert (root / '.apkg/state').read_bytes() == before
    line = next(line for line in result.stdout.splitlines() if line.startswith('show\t'))
    _, code, kind, selected, version = line.split('\t')
    return int(code), kind, selected, version


class LibraryBrowser(unittest.TestCase):
    def test_show_finds_catalogue_only_full_name_without_installing(self):
        with tempfile.TemporaryDirectory() as tmp:
            name = 'n' * 255
            root, endpoint = fixture(Path(tmp), [], [described(name, '2.0')])
            self.assertEqual(show(root, endpoint, name), (0, 'candidate', name, '2.0'))
            self.assertEqual(show(root, endpoint, name[:-1]), (5, '', '', ''))
            self.assertEqual(show(root, endpoint, ''), (1, '', '', ''))

    def test_show_prefers_installed_build_to_several_catalogue_versions(self):
        with tempfile.TemporaryDirectory() as tmp:
            root, endpoint = fixture(Path(tmp), [described('demo', '1.0')],
                [described('demo', '2.0'), described('demo', '3.0')])
            self.assertEqual(show(root, endpoint, 'demo'), (0, 'installed', 'demo', '1.0 (revision 1)'))
            self.assertEqual(show(root, None, 'demo'), (0, 'installed', 'demo', '1.0 (revision 1)'))

    def test_show_refuses_ambiguous_versions_targets_and_authorities(self):
        for variant in ('version', 'target', 'authority'):
            with self.subTest(variant=variant), tempfile.TemporaryDirectory() as tmp:
                other = json.loads(described('demo'))['body']
                if variant == 'version': other['version'] = '2.0'
                elif variant == 'target': other['target'] = 'i386-aros-v0-native'
                else: other['authority'] = '12' * 16
                root, endpoint = fixture(Path(tmp), [],
                    [described('demo'), wire('descriptor',other), described('unique')])
                self.assertEqual(show(root, endpoint, 'demo'), (14, '', '', ''))
                self.assertEqual(show(root, endpoint, 'unique'), (0, 'candidate', 'unique', '1.0'))

    def test_show_missing_name_in_empty_root_is_not_a_selection(self):
        with tempfile.TemporaryDirectory() as tmp:
            root, _ = fixture(Path(tmp), [], [])
            self.assertEqual(show(root, None, 'missing'), (5, '', '', ''))

    def test_without_a_catalogue_installed_rows_remain_available(self):
        with tempfile.TemporaryDirectory() as temporary:
            root, endpoint = fixture(Path(temporary), [described(category='Tools')], [])
            self.assertEqual(query(root, None, 1)[2][0][1], 'demo')
            self.assertEqual(query(root, None, 2)[2], [])
            self.assertEqual(query(root, None, 3)[2], [])

    def test_same_name_authorities_and_changed_same_release_are_not_collapsed(self):
        with tempfile.TemporaryDirectory() as temporary:
            old = described(summary='installed release')
            changed = described(summary='changed same release')
            foreign_body = json.loads(old)['body']
            foreign_body['authority'] = 'ef' * 16
            foreign = wire('descriptor', foreign_body)
            root, endpoint = fixture(Path(temporary), [old], [changed, foreign])
            code, categories, rows = query(root, endpoint)
            self.assertEqual(code, 0)
            self.assertEqual(len(rows), 3)
            self.assertEqual({r[2] for r in rows}, {AUTH, 'ef' * 16})
            # The foreign authority collides with an occupied root name; it
            # remains visible, without becoming an installable missing package.
            self.assertEqual(query(root, endpoint, 2)[2], [])
            self.assertEqual(query(root, endpoint, 3)[2], [])
            self.assertTrue(any(r[4] == 'author not trusted' for r in rows))
            self.assertTrue(any(r[4] == 'content-conflict' for r in rows))

    def test_all_installed_available_and_update_filters_keep_exact_rows(self):
        with tempfile.TemporaryDirectory() as temporary:
            old = described(summary='old summary', category='Tools/Editors')
            new = described(version='2.0', summary='new summary', category='Media/Draw')
            unused = described('unused', category='Network')
            root, endpoint = fixture(Path(temporary), [old], [old, new, unused])
            code, categories, rows = query(root, endpoint)
            self.assertEqual(code, 0)
            self.assertEqual(categories, ['Media', 'Network', 'Tools'])
            self.assertEqual(len(rows), 3)
            self.assertEqual([(r[0], r[1]) for r in query(root, endpoint, 1)[2]], [('installed', 'demo')])
            self.assertEqual([(r[0], r[1]) for r in query(root, endpoint, 2)[2]], [('candidate', 'unused')])
            updates = query(root, endpoint, 3)[2]
            self.assertEqual(len(updates), 1)
            self.assertEqual(updates[0][1:4], ['demo', AUTH, '2.0'])
            self.assertEqual(query(root, endpoint, term='old summary')[2][0][0], 'installed')
            self.assertEqual(query(root, endpoint, term='new summary')[2][0][0], 'candidate')
            self.assertEqual(query(root, endpoint, category='Tools')[2][0][0], 'installed')
            self.assertEqual(query(root, endpoint, 3, category='Tools')[2], [])
            self.assertEqual(query(root, endpoint, 99)[2], [])

    def test_installed_category_comes_from_its_own_build_and_unknown_is_distinct(self):
        with tempfile.TemporaryDirectory() as temporary:
            old = described(summary='local metadata', category='Original')
            new = described(version='2.0', category='Replacement')
            absent = described('absent')
            empty = described('empty', category='')
            root, endpoint = fixture(Path(temporary), [old, absent, empty], [new, absent, empty])
            code, categories, rows = query(root, endpoint)
            self.assertEqual(code, 0)
            self.assertEqual(categories, ['(none)', 'Original', 'Replacement', '[unrecorded]'])
            self.assertEqual([r[1] for r in query(root, endpoint, category='[unrecorded]')[2]], ['absent'])
            self.assertEqual([r[1] for r in query(root, endpoint, category='(none)')[2]], ['empty'])
            self.assertEqual(query(root, endpoint, 1, term='local metadata')[2][0][-1], 'Original')

    def test_full_names_unicode_categories_and_more_than_32_categories(self):
        with tempfile.TemporaryDirectory() as temporary:
            names = ['a' * 255, 'a' * 254 + 'b']
            top = 'é' * 120
            records = [described(names[0], category=top + '/Draw', summary='Crème quoted "value"'),
                       described(names[1], category='Unicode')]
            records.extend(described('p%02d' % i, category='Category%02d/Sub' % i) for i in range(35))
            root, endpoint = fixture(Path(temporary), records[:1], records)
            code, categories, rows = query(root, endpoint)
            self.assertEqual(code, 0)
            self.assertEqual(len(categories), 37)
            self.assertIn(top, categories)
            self.assertEqual({r[1] for r in rows if r[1].startswith('a')}, set(names))
            found = query(root, endpoint, category=top)[2]
            self.assertEqual(len(found), 1)
            self.assertEqual(found[0][1], names[0])
            self.assertEqual(query(root, endpoint, term='Crème')[2], found)
            # Category choices remain present even when the search has no hits.
            self.assertEqual(query(root, endpoint, term='no result')[1], categories)

    def test_allocation_snapshot_and_count_failures_clear_output_without_mutation(self):
        with tempfile.TemporaryDirectory() as temporary:
            old = described(category='Tools')
            root, endpoint = fixture(Path(temporary), [old], [old, described('other')])
            for fault, expected in [(1, 4), (-1, 8), (-2, 3), (-3, 8)]:
                with self.subTest(fault=fault):
                    self.assertEqual(query(root, endpoint, fault=fault), (expected, [], []))
