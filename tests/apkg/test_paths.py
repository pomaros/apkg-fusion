# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
"""Exercise the production parent-path utility for POSIX and DOS roots."""
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
HARNESS = r'''
#include <assert.h>
#include "context.c"
int main(void) {
    const char *cases[][2] = {
        {"/tmp/root.apkg-archives", "/tmp"}, {"/root", "/"},
        {"RAM:root.apkg-archives", "RAM:"}, {"RAM Disk:root", "RAM Disk:"},
        {"Work:Packages/root", "Work:Packages"}, {"RAM:", "RAM:"}, {"/", "/"}
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        char out[AK_PATH];
        assert(!ak_parent(cases[i][0], out));
        assert(!strcmp(out, cases[i][1]));
    }
    char out[AK_PATH], long_path[AK_PATH + 2];
    memset(long_path, 'x', sizeof long_path - 1); long_path[sizeof long_path - 1] = 0;
    assert(ak_parent(long_path, out)); assert(ak_parent("relative", out));
    assert(ak_parent(NULL, out)); assert(ak_parent("/root", NULL));
    return 0;
}
'''


class ParentPaths(unittest.TestCase):
    def test_volume_children_nested_paths_and_invalid_inputs(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / 'probe.c').write_text(HARNESS)
            command = shlex.split(os.environ.get('CC', 'cc')) + [
                '-std=c99', '-O1', '-fsanitize=address,undefined',
                '-ffunction-sections', '-fdata-sections',
                '-Wl,-dead_strip' if sys.platform == 'darwin' else '-Wl,--gc-sections',
                '-I' + str(ROOT / 'include'), '-I' + str(ROOT / 'src/apkg'),
                str(root / 'probe.c'), '-o', str(root / 'probe')]
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(root / 'probe')], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__':
    unittest.main()
