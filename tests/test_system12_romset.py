"""The native System 12 romset loader against the Python preparation code.

Builds tests/system12_romset_test.c with the host compiler. Uses synthetic
data only, so it needs no ROMs. Run:

  python3 -m unittest tests.test_system12_romset -v
"""
from __future__ import annotations

import os
from pathlib import Path
import random
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile
import zlib

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import prepare_mame_psx as mame  # noqa: E402


class System12RomsetTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = os.environ.get("CC") or shutil.which("cc")
        if not compiler:
            raise unittest.SkipTest("no host C compiler")
        cls.tmp = Path(tempfile.mkdtemp(prefix="s12-romset-"))
        cls.driver = cls.tmp / "system12_romset_test"
        subprocess.run([compiler, "-O1", "-Wall", "-Wextra", "-Werror", f"-I{ROOT / 'include'}",
                        "-o", str(cls.driver), str(ROOT / "tests/system12_romset_test.c"),
                        str(ROOT / "libpcsxcore/arcade/mame_zip.c"),
                        str(ROOT / "libpcsxcore/arcade/mame_romindex.c"),
                        str(ROOT / "libpcsxcore/arcade/mame_romload.c"), "-lz"], check=True)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def run_driver(self, *args: object) -> subprocess.CompletedProcess:
        return subprocess.run([str(self.driver), *map(str, args)], capture_output=True, text=True)

    def test_load_modes_match_python(self):
        rng = random.Random(12)
        source = bytes(rng.randrange(256) for _ in range(4096))
        src_path = self.tmp / "source.bin"
        src_path.write_bytes(source)
        # (group, stride, swap): every mode System 12 uses, plus 16_WORD.
        for group, stride, swap in ((1, 1, False), (1, 2, False), (2, 2, True), (2, 4, False),
                                    (1, 4, False), (2, 2, False), (2, 4, True)):
            for source_offset, offset in ((0, 0), (64, 1), (0, 6)):
                length = 1024
                region_size = offset + (length // group - 1) * stride + group + 16
                op = mame.LoadOp(file_index=0, source_offset=source_offset, offset=offset, length=length,
                                 group=group, stride=stride, swap=swap, macro="test")
                expected = bytearray(region_size)
                mame._apply_load(expected, bytearray(region_size), op, source, "test")
                out = self.tmp / "region.bin"
                result = self.run_driver("load", src_path, region_size, source_offset, offset, length,
                                         group, stride, int(swap), out)
                with self.subTest(group=group, stride=stride, swap=swap, offset=offset):
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual(out.read_bytes(), bytes(expected))

    def test_load_rejects_out_of_range(self):
        src_path = self.tmp / "small.bin"
        src_path.write_bytes(bytes(256))
        out = self.tmp / "unused.bin"
        self.assertNotEqual(self.run_driver("load", src_path, 256, 0, 2, 256, 1, 2, 0, out).returncode, 0)
        self.assertNotEqual(self.run_driver("load", src_path, 1024, 128, 0, 256, 1, 1, 0, out).returncode, 0)

    def test_zip_members_by_crc(self):
        rng = random.Random(7)
        stored = bytes(rng.randrange(256) for _ in range(70000))
        deflated = bytes(rng.randrange(4) for _ in range(300000))
        path = self.tmp / "set.zip"
        with zipfile.ZipFile(path, "w") as archive:
            archive.writestr(zipfile.ZipInfo("a.bin"), stored, compress_type=zipfile.ZIP_STORED)
            archive.writestr(zipfile.ZipInfo("dir/b.bin"), deflated, compress_type=zipfile.ZIP_DEFLATED)
            archive.comment = b"TORRENTZIPPED-00000000"
        for data in (stored, deflated):
            out = self.tmp / "member.bin"
            result = self.run_driver("zip", path, f"{zlib.crc32(data):08x}", len(data), out)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(out.read_bytes(), data)
        # Same CRC with another size, or an unknown CRC, finds nothing.
        self.assertNotEqual(self.run_driver("zip", path, f"{zlib.crc32(stored):08x}", len(stored) - 1,
                                            self.tmp / "x").returncode, 0)
        self.assertNotEqual(self.run_driver("zip", path, "12345678", len(stored), self.tmp / "x").returncode, 0)

    def test_zip_corrupt_member_fails_crc(self):
        data = bytes(range(256)) * 64
        path = self.tmp / "corrupt.zip"
        with zipfile.ZipFile(path, "w") as archive:
            archive.writestr(zipfile.ZipInfo("a.bin"), data, compress_type=zipfile.ZIP_STORED)
        raw = bytearray(path.read_bytes())
        start = raw.index(data[:64])
        raw[start + 100] ^= 0xff
        path.write_bytes(bytes(raw))
        result = self.run_driver("zip", path, f"{zlib.crc32(data):08x}", len(data), self.tmp / "x")
        self.assertNotEqual(result.returncode, 0)


if __name__ == "__main__":
    unittest.main()
