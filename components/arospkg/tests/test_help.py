"""Compile the production help functions without the AROS runtime."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]

class Help(unittest.TestCase):
    def output(self, call='usage()'):
        source = (ROOT / 'src/pkg/main.c').read_text()
        start = source.index('static void usage(void)')
        end = source.index('/* The progress contract', start)
        with tempfile.TemporaryDirectory() as tmp:
            c = pathlib.Path(tmp) / 'help.c'
            exe = pathlib.Path(tmp) / 'help'
            c.write_text('#include <stdio.h>\nstatic const char *VERSION="apkg test";\n'
                         + source[start:end] + '\nint main(void) { ' + call + '; return 0; }\n')
            subprocess.run(['cc', str(c), '-o', str(exe)], check=True)
            return subprocess.check_output([str(exe)]).decode()

    def test_basic_help_excludes_test_controls(self):
        out = self.output()
        for flag in ('--expect ', '--verify-name ', '--interrupt-at ', 'assert-hash '):
            self.assertNotIn(flag, out)

    def test_basic_help_is_action_oriented(self):
        out = self.output()
        for text in ('Search available packages', 'Show package details',
                     'Check installed files', '--about', '--help-all', '--help-testing'):
            self.assertIn(text, out)

    def test_extended_help_retains_controls(self):
        out = self.output('usage_all()')
        for flag in ('--index ', '--dry-run ', '--expect ', '--interrupt-at '):
            self.assertIn(flag, out)
        testing = self.output('usage_testing()')
        self.assertIn('--cancel-at ', testing)
        self.assertNotIn('  install <id>', testing)

if __name__ == '__main__':
    unittest.main()
