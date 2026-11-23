# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
"""apkg fetch: one URL to a file, as curl -fLo would, with no root or trust.
The file takes its name only once complete and, with --sha256, matching."""
import ctypes as C
import hashlib
import os
import subprocess
import tempfile
import unittest
from pathlib import Path
from test_client import CLI
from test_http_channel import serve
from support import lib, Digest

lib.apkg_client_fetch.argtypes = [C.c_char_p, C.c_char_p, C.c_char_p, C.c_uint64, C.c_void_p, C.POINTER(C.c_uint64), C.c_void_p]
lib.apkg_client_fetch.restype = C.c_int

DATA = bytes(range(256)) * 1000

def fetch(*args, cwd=None):
    return subprocess.run([str(CLI), 'fetch', *map(str, args)], capture_output=True, timeout=120, cwd=cwd)

def leftovers(folder): return sorted(p.name for p in Path(folder).iterdir() if p.name.startswith('.apkg-fetch'))

class Fetch(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.d = Path(self.tmp.name)
        self.site = self.d / 'site'; self.site.mkdir(); (self.site / 'file.bin').write_bytes(DATA)
        self.out = self.d / 'out'; self.out.mkdir()
    def tearDown(self): self.tmp.cleanup()

    def test_named_output_and_reported_digest(self):
        with serve(self.site) as (url, _):
            done = fetch(url + '/file.bin', self.out / 'copy')
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual((self.out / 'copy').read_bytes(), DATA)
        self.assertIn(hashlib.sha256(DATA).hexdigest().encode(), done.stderr)
        self.assertIn(b'%d bytes' % len(DATA), done.stderr); self.assertEqual(leftovers(self.out), [])

    def test_default_name_ignores_the_query_and_follows_redirects(self):
        with serve(self.site) as (url, server):
            server.mode = 'redirect'
            done = fetch(url + '/file.bin?mirror=1&x=[a]', cwd=self.out)
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual((self.out / 'file.bin').read_bytes(), DATA)

    def test_standard_output(self):
        with serve(self.site) as (url, _):
            done = fetch(url + '/file.bin', '-')
        self.assertEqual(done.returncode, 0, done.stderr); self.assertEqual(done.stdout, DATA)

    def test_checksum_match_and_mismatch_keep_the_earlier_file(self):
        target = self.out / 'copy'; target.write_bytes(b'earlier')
        with serve(self.site) as (url, _):
            wrong = fetch(url + '/file.bin', target, '--sha256', '00' * 32)
            self.assertEqual(wrong.returncode, 10, wrong.stderr)
            self.assertEqual(target.read_bytes(), b'earlier'); self.assertEqual(leftovers(self.out), [])
            right = fetch(url + '/file.bin', target, '--sha256', hashlib.sha256(DATA).hexdigest())
        self.assertEqual(right.returncode, 0, right.stderr); self.assertEqual(target.read_bytes(), DATA)

    def test_missing_file_and_size_limit_change_nothing(self):
        target = self.out / 'copy'; target.write_bytes(b'earlier')
        with serve(self.site) as (url, _):
            self.assertEqual(fetch(url + '/absent.bin', target).returncode, 5)
            self.assertEqual(fetch(url + '/file.bin', target, '--max-bytes', 1000).returncode, 3)
        self.assertEqual(target.read_bytes(), b'earlier'); self.assertEqual(leftovers(self.out), [])

    def test_refused_requests_write_nothing(self):
        folder = self.out / 'drawer'; folder.mkdir()
        cases = (('ftp://example.org/f', self.out / 'a'), ('http://user:pw@127.0.0.1/f', self.out / 'b'),
                 ('http://127.0.0.1/f#part', self.out / 'c'), ('http://127.0.0.1/', None),
                 ('http://127.0.0.1/f', folder))
        for url, output in cases:
            with self.subTest(url=url, output=output):
                done = fetch(url, *([output] if output else []), cwd=self.out)
                self.assertEqual(done.returncode, 1, done.stderr)
        self.assertEqual(sorted(p.name for p in self.out.iterdir()), ['drawer'])
        self.assertEqual(fetch('http://127.0.0.1/f', '-', '--sha256', '00' * 32).returncode, 1)
        self.assertEqual(fetch().returncode, 1)
        self.assertEqual(fetch('http://127.0.0.1/f', '--max-bytes', 'x').returncode, 1)

class FetchThroughLibrary(unittest.TestCase):
    """The same contract through the API, so that it also runs on the built-in
    client AROS uses (make test-apkg-http-client)."""
    def test_api_fetch_checksum_missing_and_limit(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d); site = d / 'site'; site.mkdir(); (site / 'f.bin').write_bytes(DATA)
            target = d / 'copy'; digest = Digest(); size = C.c_uint64()
            with serve(site) as (url, _):
                base = (url + '/f.bin?v=2').encode()
                self.assertEqual(lib.apkg_client_fetch(base, str(target).encode(), None, 0, C.byref(digest), C.byref(size), None), 0)
                self.assertEqual(target.read_bytes(), DATA); self.assertEqual(size.value, len(DATA))
                self.assertEqual(bytes(digest.bytes), hashlib.sha256(DATA).digest())
                target.write_bytes(b'earlier')
                self.assertEqual(lib.apkg_client_fetch(base, str(target).encode(), b'00' * 32, 0, None, None, None), 10)
                self.assertEqual(lib.apkg_client_fetch((url + '/none').encode(), str(target).encode(), None, 0, None, None, None), 5)
                self.assertEqual(lib.apkg_client_fetch(base, str(target).encode(), None, 100, None, None, None), 3)
            self.assertEqual(target.read_bytes(), b'earlier'); self.assertEqual(leftovers(d), [])

if __name__ == '__main__':
    unittest.main()
