# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
"""Host model of a DOS without ReplaceObject (dos.library before 50.84, or a
handler such as SFS or FFS without ACTION_REPLACE_OBJECT): every replacement
goes through the deterministic aside name, and an interruption after either
step leaves a reader the old or the new contents and recovery a whole state."""
import ctypes as C
import json
import os
import tempfile
import unittest
from pathlib import Path
from support import *
from test_operations import install, run_plan, payload

lib.pkg_fs_test_replace.argtypes = [C.c_int, C.c_int]
lib.pkg_fs_write_atomic.argtypes = [C.c_char_p, C.c_void_p, C.c_size_t]
lib.pkg_fs_write_atomic.restype = C.c_int
lib.pkg_fs_read.argtypes = [C.c_char_p, C.POINTER(C.c_void_p), C.POINTER(C.c_size_t)]
lib.pkg_fs_read.restype = C.c_int
lib.apkg_plan_upgrade.argtypes = [C.c_void_p, C.c_void_p, C.c_void_p, C.POINTER(C.c_void_p), C.c_void_p]
lib.apkg_plan_upgrade.restype = C.c_int
lib.apkg_snapshot_root.argtypes = [C.c_void_p]; lib.apkg_snapshot_root.restype = C.POINTER(RootView)
libc = C.CDLL(None); libc.free.argtypes = [C.c_void_p]

def asides(folder): return sorted(p.name for p in folder.iterdir() if p.name.startswith('.pkgold'))
def body(root): return json.loads((root / '.apkg/state').read_bytes())['body']
def fs_read(path):
    buf = C.c_void_p(); n = C.c_size_t()
    if lib.pkg_fs_read(str(path).encode(), C.byref(buf), C.byref(n)): return None
    data = C.string_at(buf, n.value); libc.free(buf); return data
def fs_write(path, data):
    b = C.create_string_buffer(data, len(data)); return lib.pkg_fs_write_atomic(str(path).encode(), b, len(data))
def newer(version='2.0', text='new'):
    obj = json.loads(descriptor(files=[payload('hello', text)])); obj['body']['version'] = version
    return json.dumps(obj, sort_keys=True, separators=(',', ':')).encode()
def plan_upgrade(c, data):
    code, s = snapshot(c); assert code == 0
    code, pkg = package(c, data, sign(data)); assert code == 0
    p = C.c_void_p(); code = lib.apkg_plan_upgrade(c, s, pkg, C.byref(p), None)
    lib.apkg_package_free(pkg); lib.apkg_snapshot_free(s); return code, p
def plan_install(c, data):
    code, s = snapshot(c); assert code == 0
    code, pkg = package(c, data, sign(data)); assert code == 0
    code, p = install_plan(c, s, pkg); lib.apkg_package_free(pkg); lib.apkg_snapshot_free(s); return code, p

class ReplaceWithoutReplaceObject(unittest.TestCase):
    def setUp(self): lib.pkg_fs_test_replace(1, 0)
    def tearDown(self): lib.pkg_fs_test_replace(0, 0); lib.apkg_test_fault(0, 0)

    def test_install_upgrade_remove_leave_no_aside(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d) / 'root'; c = init(root); install(c, descriptor())
            code, p = plan_upgrade(c, newer()); self.assertEqual(code, 0); self.assertEqual(run_plan(c, p), (0, 1)); lib.apkg_plan_free(p)
            self.assertEqual((root / 'demo/hello').read_text(), 'new')
            code, s = snapshot(c); out = C.c_void_p()
            lib.apkg_plan_remove.argtypes = [C.c_void_p, C.c_void_p, Text, C.POINTER(C.c_void_p), C.c_void_p]
            name = b'demo'; self.assertEqual(lib.apkg_plan_remove(c, s, Text(C.cast(C.c_char_p(name), C.c_void_p), 4), C.byref(out), None), 0)
            self.assertEqual(run_plan(c, out), (0, 1)); lib.apkg_plan_free(out); lib.apkg_snapshot_free(s)
            self.assertEqual(body(root)['installed'], []); self.assertEqual(asides(root / '.apkg'), [])
            lib.apkg_close(c)

    def interrupted(self, plan, step):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d) / 'root'; c = init(root); install(c, descriptor())
            old = (root / '.apkg/state').read_bytes()
            code, p = plan(c); self.assertEqual(code, 0); new = json.loads(C.string_at(lib.apkg_plan_get(p).contents.canonical.data, lib.apkg_plan_get(p).contents.canonical.size))['body']['new_state']
            code, a = approve(c, p); pid = os.fork()
            if pid == 0:
                lib.pkg_fs_test_replace(1, step); execute(c, p, a); os._exit(98)
            _, status = os.waitpid(pid, 0); self.assertEqual(os.waitstatus_to_exitcode(status), 99)
            meta = root / '.apkg'
            if step == 1:
                self.assertFalse((meta / 'state').exists()); self.assertEqual(len(asides(meta)), 1)
                self.assertEqual((meta / asides(meta)[0]).read_bytes(), old)
            else:
                self.assertEqual(json.loads((meta / 'state').read_bytes()), new); self.assertEqual(len(asides(meta)), 1)
            code, s = snapshot(c); self.assertEqual(code, 0); self.assertEqual(lib.apkg_snapshot_root(s).contents.state, 3); lib.apkg_snapshot_free(s)
            self.assertEqual(recover(c)[0], 0)
            self.assertEqual(json.loads((meta / 'state').read_bytes()), new); self.assertEqual(asides(meta), [])
            lib.apkg_approval_free(a); lib.apkg_plan_free(p); lib.apkg_close(c)

    def test_interrupted_install_publication_recovers_after_either_step(self):
        for step in (1, 2):
            with self.subTest(step=step):
                self.interrupted(lambda c: plan_install(c, descriptor(name='other')), step)

    def test_interrupted_upgrade_publication_recovers_after_either_step(self):
        for step in (1, 2):
            with self.subTest(step=step):
                self.interrupted(lambda c: plan_upgrade(c, newer()), step)

    def test_a_profile_reader_sees_old_or_new_and_the_next_write_cleans_up(self):
        for step, expected in ((1, b'old'), (2, b'new')):
            with self.subTest(step=step), tempfile.TemporaryDirectory() as d:
                path = Path(d) / 'profile'; self.assertEqual(fs_write(path, b'old'), 0)
                pid = os.fork()
                if pid == 0:
                    lib.pkg_fs_test_replace(1, step); fs_write(path, b'new'); os._exit(98)
                _, status = os.waitpid(pid, 0); self.assertEqual(os.waitstatus_to_exitcode(status), 99)
                self.assertEqual(path.exists(), step == 2); self.assertEqual(len(asides(Path(d))), 1)
                self.assertEqual(fs_read(path), expected)
                self.assertEqual(fs_write(path, b'third'), 0)
                self.assertEqual(fs_read(path), b'third'); self.assertEqual(asides(Path(d)), [])

    def test_a_missing_file_without_aside_is_still_missing(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertIsNone(fs_read(Path(d) / 'absent'))

if __name__ == '__main__':
    unittest.main()
