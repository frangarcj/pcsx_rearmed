import importlib.util
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "vita_arcade_runner", ROOT / "tools/vita-harness/run_system12_vita3k.py")
RUNNER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RUNNER)


class ReplayTests(unittest.TestCase):
    def normalize(self, text, frames=3600):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "inputs.csv"
            path.write_text(text)
            return RUNNER.canonical_replay(path, frames)

    def test_simultaneous_controls_and_last_value(self):
        self.assertEqual(self.normalize(
            "3304,p1button2,1\n3304,p1button1,1\n3304,p1button1,0\n"),
            "frame,control,pressed\n3304,p1button1,0\n3304,p1button2,1\n")

    def test_header_comments_and_shorter_endpoint(self):
        self.assertEqual(self.normalize(
            "frame,control,pressed\n# replay\n0,coin1,1\n2,coin1,0\n100,p1start,1\n", 50),
            "frame,control,pressed\n0,coin1,1\n2,coin1,0\n")

    def test_frame_order_and_bound(self):
        for text in ("2,coin1,1\n1,coin1,0\n", "216000,coin1,1\n"):
            with self.assertRaises(ValueError):
                self.normalize(text)

    def test_invalid_controls_values_and_fields(self):
        for text in ("0,bogus,1\n", "0,coin1,2\n", "0,coin1,1,extra\n", "-1,coin1,1\n"):
            with self.assertRaises(ValueError):
                self.normalize(text)

    def test_nul_size_and_empty_replay(self):
        for text in ("0,coin1,1\0\n", "#" * (1024 * 1024 + 1), "frame,control,pressed\n"):
            with self.assertRaises(ValueError):
                self.normalize(text)


if __name__ == "__main__":
    unittest.main()
