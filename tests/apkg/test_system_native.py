# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
"""Execute the native probe branch with mocked Exec/DOS, not an AROS runtime.
A resource allocation contains only a Node: ASan detects Library overreads.
"""
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]

HEADERS = r'''
#ifndef MOCK_EXEC_H
#define MOCK_EXEC_H
#include <stdint.h>
typedef char *STRPTR;
typedef const char *CONST_STRPTR;
typedef unsigned long ULONG;
typedef uintptr_t BPTR;
#define SHARED_LOCK 0
struct Node { void *next; };
struct Library { struct Node node; unsigned short lib_Version; };
struct List { void *entry; };
struct ExecBase { struct List LibList, ResourceList, DeviceList; };
static struct ExecBase base;
#define SysBase (&base)
static void Forbid(void) {}
static void Permit(void) {}
static struct Node *FindName(struct List *list, STRPTR name) {
    (void)name; return list->entry;
}
static struct Library *OpenLibrary(CONST_STRPTR name, ULONG version) {
    (void)name; (void)version; return 0;
}
static void CloseLibrary(struct Library *library) { (void)library; }
static BPTR Lock(CONST_STRPTR path, int mode) { (void)path; (void)mode; return 0; }
static void UnLock(BPTR lock) { (void)lock; }
#endif
'''
HARNESS = r'''
#include <assert.h>
#include "system.c"
int main(void) {
    struct Node *resource = malloc(sizeof *resource);
    assert(resource);
    base.ResourceList.entry = resource;
    ak_probe r = {0};
    strcpy(r.type, "resource"); strcpy(r.id, "FileSystem.resource");
    probe(&r); assert(r.state == 0 && r.version == -1);
    r.min_version = 1;
    probe(&r); assert(r.state == 2 && r.version == -1);
    base.ResourceList.entry = NULL;
    probe(&r); assert(r.state == 1 && r.version == -1);
    free(resource);
    struct Library device = {0}; device.lib_Version = 5;
    base.DeviceList.entry = &device;
    strcpy(r.type, "device"); strcpy(r.id, "test.device");
    probe(&r); assert(r.state == 0 && r.version == 5);
    r.min_version = 6;
    probe(&r); assert(r.state == 1 && r.version == 5);
    base.DeviceList.entry = NULL;
    probe(&r); assert(r.state == 1 && r.version == -1);
    return 0;
}
'''

class NativeProbe(unittest.TestCase):
    def test_node_only_resource_and_versioned_device(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            for name in ('exec/execbase.h', 'exec/libraries.h', 'proto/dos.h', 'proto/exec.h'):
                p = d / name; p.parent.mkdir(parents=True, exist_ok=True)
                p.write_text(HEADERS)
            (d / 'probe.c').write_text(HARNESS)
            command = shlex.split(os.environ.get('CC', 'cc')) + [
                '-std=c99', '-O1', '-D__AROS__', '-fsanitize=address,undefined',
                '-ffunction-sections', '-fdata-sections',
                '-Wl,-dead_strip' if sys.platform == 'darwin' else '-Wl,--gc-sections',
                '-I' + str(d), '-I' + str(ROOT / 'include'),
                '-I' + str(ROOT / 'src/apkg'), str(d / 'probe.c'), '-o', str(d / 'probe')]
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(d / 'probe')], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)

if __name__ == '__main__':
    unittest.main()
