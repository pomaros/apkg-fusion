# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
"""Native permission bits must not become duplicate extra metadata."""
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
HARNESS = r'''
#include "internal.h"
#include "pkg_fs.h"
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
/* Select the real native observation branch after host headers are loaded. */
#undef __APPLE__
#undef __linux__
#define __AROS__ 1
static unsigned long long protection;
static const char *comment = "";
int pkg_fs_amiga_get(const char *p, unsigned long long *word, char *text, size_t size) {
    (void)p; *word = protection; snprintf(text, size, "%s", comment); return 1;
}
#include "attributes.c"
int main(void) {
    const unsigned long corpus[][2] = {
        {0644, 34818}, {0755, 43520}, {0700, 0}, {0600, 2},
        {0400, 6}, {0000, 14}, {0777, 60928}
    };
    for (size_t i = 0; i < sizeof corpus / sizeof *corpus; i++) {
        char *out = NULL; protection = corpus[i][1];
        assert(ak_attributes_read("file", corpus[i][0], &out) == APKG_OK);
        assert(out == NULL);
    }
    const unsigned long extras[] = {1, 16, 64};
    for (size_t i = 0; i < sizeof extras / sizeof *extras; i++) {
        unsigned long extra = extras[i];
        char *out = NULL, wanted[80]; protection = 34818 | extra;
        assert(ak_attributes_read("file", 0644, &out) == APKG_OK && out);
        snprintf(wanted, sizeof wanted, "\"protection\":\"%llu\"", protection);
        assert(strstr(out, wanted)); free(out);
    }
    char *out = NULL; protection = 34818; comment = "note";
    assert(ak_attributes_read("file", 0644, &out) == APKG_OK && out);
    assert(strstr(out, "\"comment\":\"6e6f7465\""));
    assert(strstr(out, "\"protection\":\"34818\"")); free(out);
    return 0;
}
'''


class NativeAttributes(unittest.TestCase):
    def test_ordinary_rwx_is_in_mode_while_extra_flags_and_comments_survive(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / 'dos').mkdir()
            constants = ('OTR_EXECUTE', 'OTR_WRITE', 'OTR_READ',
                         'GRP_EXECUTE', 'GRP_WRITE', 'GRP_READ',
                         'EXECUTE', 'WRITE', 'READ')
            positions = (13, 14, 15, 9, 10, 11, 1, 2, 3)
            (root / 'dos/dos.h').write_text(''.join(
                f'#define FIBF_{name} (1u << {bit})\n'
                for name, bit in zip(constants, positions)))
            (root / 'probe.c').write_text(HARNESS)
            command = shlex.split(os.environ.get('CC', 'cc')) + [
                '-std=c99', '-O1', '-fsanitize=address,undefined',
                '-ffunction-sections', '-fdata-sections',
                '-Wl,-dead_strip' if sys.platform == 'darwin' else '-Wl,--gc-sections',
                '-I' + str(root), '-I' + str(ROOT / 'include'),
                '-I' + str(ROOT / 'src/apkg'), str(root / 'probe.c'),
                str(ROOT / 'build/libapkg-test.dylib'), '-o', str(root / 'probe')]
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(root / 'probe')], capture_output=True, text=True, cwd=ROOT)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__':
    unittest.main()
