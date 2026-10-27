# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
"""Run native identity code against DOS-shaped locks and enumeration cursors."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
HEADERS = r'''
#ifndef MOCK_IDENTITY_DOS
#define MOCK_IDENTITY_DOS
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
typedef const char *CONST_STRPTR;
typedef uintptr_t BPTR;
#define BADDR(p) ((void *)(uintptr_t)(p))
#define AROS_BSTR_ADDR(p) ((unsigned char *)(p) + 1)
#define AROS_BSTR_strlen(p) (*(unsigned char *)(p))
#define SHARED_LOCK 0
#define MEMF_ANY 0
#define MEMF_CLEAR 1
#define DOS_FIB 1
struct DeviceList { unsigned char *dl_Name; uint32_t dl_VolumeDate[3]; };
struct FileLock { uintptr_t fl_Key; };
struct InfoData { BPTR id_VolumeNode; };
struct FileInfoBlock { uintptr_t fib_DiskKey; };
static unsigned char volume_name[] = {4, 'T', 'e', 's', 't'};
static struct DeviceList volume = {volume_name, {1, 2, 3}};
static struct FileLock object = {0x12345678abcdef01ull};
static uintptr_t cursor = 0x11110001;
static int fail_lock, fail_info;
static BPTR Lock(CONST_STRPTR path, int mode) {
    (void)path; (void)mode; return fail_lock ? 0 : (BPTR)&object;
}
static void UnLock(BPTR lock) { (void)lock; }
static void *AllocVec(size_t size, int flags) { (void)flags; return calloc(1, size); }
static void FreeVec(void *p) { free(p); }
static int Info(BPTR lock, struct InfoData *info) {
    (void)lock; info->id_VolumeNode = (BPTR)&volume; return !fail_info;
}
/* Examine's cursor deliberately differs from the locked object identity. */
static void *AllocDosObject(int kind, void *tags) {
    (void)kind; (void)tags; return calloc(1, sizeof(struct FileInfoBlock));
}
static void FreeDosObject(int kind, void *p) { (void)kind; free(p); }
static int Examine(BPTR lock, struct FileInfoBlock *fib) {
    (void)lock; fib->fib_DiskKey = cursor; return 1;
}
#endif
'''
HARNESS = r'''
#include <assert.h>
#include <string.h>
#include "internal.h"
static mode_t mode = S_IFDIR | 0700;
static int fake_stat(const char *p, struct stat *st) {
    (void)p; memset(st, 0, sizeof *st); st->st_mode = mode;
    st->st_ino = 42; st->st_dev = 999; return 0;
}
#define stat(p, s) fake_stat(p, s)
#define lstat(p, s) fake_stat(p, s)
#include "identity.c"
int main(void) {
    ak_identity before, after;
    assert(!ak_stat("RAM:folder", &before));
    assert(before.st_ino == object.fl_Key && ak_identity_known(&before));
    assert(before.metadata.st_mode == mode);
    cursor++; /* child creation changes enumeration, not the directory */
    assert(!ak_stat("RAM:renamed-folder", &after));
    assert(before.st_dev == after.st_dev && before.st_ino == after.st_ino);
    object.fl_Key += 1ull << 32; /* identical low 32 bits, different object */
    assert(!ak_stat("RAM:replacement", &after));
    assert(before.st_ino != after.st_ino);
    object.fl_Key = before.st_ino;
    volume.dl_VolumeDate[0]++;
    assert(!ak_stat("RAM:folder", &after)); assert(before.st_dev != after.st_dev);
    object.fl_Key = 0;
    assert(!ak_stat("RAM:unknown", &after)); assert(!ak_identity_known(&after));
    object.fl_Key = before.st_ino; fail_info = 1;
    assert(!ak_stat("RAM:no-volume", &after)); assert(!ak_identity_known(&after));
    fail_info = 0; fail_lock = 1;
    assert(!ak_stat("RAM:no-lock", &after)); assert(!ak_identity_known(&after));
    fail_lock = 0; mode = S_IFLNK | 0777;
    assert(!ak_lstat("RAM:link", &after)); assert(!ak_identity_known(&after));
    return 0;
}
'''


class NativeIdentity(unittest.TestCase):
    def test_enumeration_changes_high_bits_volume_replacement_and_unknown_keys(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for name in ('dos/dos.h', 'dos/dosextens.h', 'exec/memory.h',
                         'proto/dos.h', 'proto/exec.h'):
                header = root / name
                header.parent.mkdir(parents=True, exist_ok=True)
                header.write_text(HEADERS)
            (root / 'probe.c').write_text(HARNESS)
            command = shlex.split(os.environ.get('CC', 'cc')) + [
                '-D_DEFAULT_SOURCE', '-D_DARWIN_C_SOURCE', '-D__AROS__',
                '-std=c99', '-O1', '-fsanitize=address,undefined',
                '-I' + str(root), '-I' + str(ROOT / 'include'),
                '-I' + str(ROOT / 'src/apkg'), str(root / 'probe.c'),
                '-o', str(root / 'probe')]
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(root / 'probe')], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__':
    unittest.main()
