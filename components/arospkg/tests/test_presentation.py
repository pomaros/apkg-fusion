"""Exercise production presentation; fake only the native DOS capability query."""
import os
import pathlib
import re
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]

class Presentation(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="apkg-presentation-")
        cls.exe = pathlib.Path(cls.tmp.name) / "driver"
        subprocess.run([os.environ.get("CC", "cc"), "-std=c99", "-Wall", "-Wextra", "-Werror",
                        "-I" + str(ROOT / "tests/presentation-stubs"),
                        str(ROOT / "tests/presentation-driver.c"),
                        str(ROOT / "src/pkg/present.c"), "-o", str(cls.exe)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def output(self, mode, width=80):
        return subprocess.check_output([str(self.exe), mode, str(width)]).decode()

    def test_plain_and_redirect_get_stages_without_percentages(self):
        for mode in ("plain", "redirect"):
            with self.subTest(mode=mode):
                text = self.output(mode)
                self.assertEqual(text, "Downloading 1000 B\nVerifying archive\nInstalling files\n")
                self.assertNotIn("\x1b", text)

    def test_json_gets_no_progress_at_all(self):
        self.assertEqual(self.output("json"), "")

    def test_narrow_progress_keeps_percentage_not_package_prefix(self):
        text = self.output("progress", 24)
        self.assertIn("58%", text)
        for line in text.splitlines():
            self.assertLessEqual(len(re.sub(r"\x1b\[[0-9;]*[mK]", "", line).rstrip("\n")), 23)

    def test_phases_are_visible_without_fabricated_percentages(self):
        text = self.output("progress")
        self.assertIn("\nVerifying archive\n", text)
        self.assertIn("\nInstalling files\n", text)
        self.assertEqual(text.count("Verifying archive"), 1)
        self.assertNotIn("100%", text)

    def test_progress_adapts_after_resize(self):
        text = self.output("resize")
        frame = next(x for x in text.splitlines() if "58%" in x)
        self.assertLessEqual(len(re.sub(r"\x1b\[[0-9;]*[mK]", "", frame)), 23)

    def test_resize_does_not_rewrite_a_possibly_wrapped_previous_frame(self):
        for result in ("success", "cancel", "error"):
            with self.subTest(result=result):
                text = self.output("resize-cycle-" + result)
                self.assertNotIn("\r", text)
                self.assertNotIn("\x1b", text)
                narrow = text.split("<narrow>\n")[1].split("<wide>\n")[0]
                self.assertIn("20%", narrow)
                self.assertIn("30%", narrow)
                self.assertNotIn("21%", narrow)  # no per-callback scrolling
                for line in narrow.splitlines():
                    self.assertLessEqual(len(line), 11)
                self.assertIn("Downloading", text.split("<wide>\n")[1])
                ending = {"success": "installed", "cancel": "cancelled",
                          "error": "connection reset"}[result]
                self.assertTrue(text.endswith("\n" + ending + "\n"))

    def test_layout_preserves_details_and_fits_width(self):
        for width in (12, 24, 40, 80, 120):
            with self.subTest(width=width):
                text = re.sub(r"\x1b\[[0-9;]*[mK]", "", self.output("layout", width))
                for line in text.splitlines():
                    self.assertLessEqual(len(line), width - 1, repr(line))
                compact = re.sub(r"\s+", "", text)
                for item in ("Work:Applications/Micropolis", "0.1.0-rc3-aros2", "installed",
                             "a-very-long-package-identifier", "12345678901234567890",
                             "otherCPU:i386/v0", "savedcities."):
                    self.assertIn(item, compact)

    def test_slow_milliseconds_are_converted_to_50hz_ticks(self):
        for ms, ticks in ((-1, 0), (0, 0), (1, 1), (20, 1), (21, 2), (1000, 50), (5000, 250), (2147483647, 107374183)):
            with self.subTest(ms=ms):
                self.assertEqual(self.output("slow", ms), str(ticks) + "\n")

    def test_unknown_capabilities_do_not_authorize_escape_sequences(self):
        unknown = self.output("unknown")
        self.assertNotIn("\x1b", unknown)
        self.assertNotIn("\r", unknown)
        con = self.output("con")
        self.assertIn("58%", con)
        self.assertNotIn("\x1b", con)
        self.assertNotIn("\r", con)

    def test_byte_counters_do_not_overflow_or_exceed_100_percent(self):
        text = self.output("overflow")
        self.assertRegex(text, r"(49|50)%")
        self.assertIn("100%", text)
        self.assertNotIn("200%", text)

    def test_progress_leaves_no_stale_line(self):
        text = self.output("progress")
        self.assertTrue(text.endswith("\n"))
        self.assertNotIn("\r", text)
        self.assertNotIn("\x1b[K", text)
        self.assertTrue(text.endswith("Installing files\n"))

    def test_unknown_length_does_not_collide_with_engine_status(self):
        text = self.output("unknown-length")
        self.assertIn("\nverified and cached test.zip\n", text)
        self.assertNotIn("%", text)

    # Capability replies for the colour encoding (aros-xterm operation 13).
    # known: colours, styles, lines, geometry = 15; + interpretation = 31.
    def colors(self, known, interp, styles=1, how="auto"):
        return subprocess.check_output([str(self.exe), "colors", str(known), str(interp),
                                        str(styles), how]).decode()

    def test_known_ansi_keeps_basic_sgr(self):
        t = self.colors(31, 0)
        for code in ("\x1b[32m", "\x1b[33m", "\x1b[31m"):
            self.assertIn(code, t)
        self.assertNotIn("38;5", t)

    def test_known_pens_use_indexed_colours_at_ansi16(self):
        t = self.colors(31, 1)       # the driver reports level ANSI16
        for code in ("\x1b[38;5;2m", "\x1b[38;5;3m", "\x1b[38;5;1m"):
            self.assertIn(code, t)
        self.assertIsNone(re.search(r"\x1b\[3[0-7]m", t))
        self.assertIn("\x1b[0m", t)  # bold available: SGR 0 ends the colour

    def test_unknown_interpretation_keeps_basic_sgr(self):
        t = self.colors(15, 0)
        self.assertIn("\x1b[32m", t)
        self.assertNotIn("38;5", t)

    def test_pens_without_known_bit_is_ignored(self):
        t = self.colors(15, 1)
        self.assertIn("\x1b[32m", t)
        self.assertNotIn("38;5", t)

    def test_colour_without_bold_resets_with_39(self):
        for interp in (0, 1):
            with self.subTest(pens=interp):
                t = self.colors(31, interp, styles=0)
                self.assertIn("\x1b[39m", t)
                self.assertNotIn("\x1b[0m", t)
                self.assertNotIn("\x1b[1m", t)

    def test_no_colour_when_turned_off_or_not_a_window(self):
        for how in ("never", "plain", "json", "redirect"):
            for interp in (0, 1):
                with self.subTest(how=how, pens=interp):
                    t = self.colors(31, interp, how=how)
                    self.assertEqual(t, "native undetermined missing\n")

if __name__ == "__main__":
    unittest.main()
