"""The native System 10, System 11 and Capcom ZN zip loader against the preparers.

For a set, builds MAME zips of synthetic ROM files (each has the size and
CRC32 that MAME declares, and random content), has the C loader
(libpcsxcore/arcade/arcade_romset.c) build the board package in memory, and
compares every file with the package that scripts/prepare_system10_board.py,
prepare_system11_board.py or prepare_capcom_zn_board.py writes from the same
data. Needs the pinned MAME checkout (../mamedev or $MAME_SOURCE) and a host
C compiler, and no ROMs. Run:

  python3 -m unittest tests.test_arcade_romset -v

A few of the smallest sets are checked by default. ARCADE_ROMSET_FULL=1
checks all 90, one set at a time (about 250 MiB of temporary files at most).
"""
from __future__ import annotations

import filecmp
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
import generate_arcade_romsets as gen  # noqa: E402
import prepare_capcom_zn_board as zn_prepare  # noqa: E402
import prepare_mame_psx as mame  # noqa: E402
import prepare_system10_board as s10_prepare  # noqa: E402
import prepare_system11_board as s11_prepare  # noqa: E402

MAME_SOURCE = Path(os.environ.get("MAME_SOURCE", ROOT.parent / "mamedev"))

_TABLE = []
for _i in range(256):
    _c = _i
    for _ in range(8):
        _c = (_c >> 1) ^ 0xEDB88320 if _c & 1 else _c >> 1
    _TABLE.append(_c)
_TOP = {_TABLE[i] >> 24: i for i in range(256)}


def forge(size: int, crc32: int) -> bytes:
    """Random bytes of this size whose CRC32 is crc32 (the last four are computed)."""
    assert size >= 4
    prefix = random.Random(crc32 ^ size).randbytes(size - 4)
    state = zlib.crc32(prefix) ^ 0xFFFFFFFF
    want = crc32 ^ 0xFFFFFFFF
    indexes, known = [], want
    for _ in range(4):
        i = _TOP[known >> 24]
        indexes.append(i)
        known = ((known ^ _TABLE[i]) << 8) & 0xFFFFFFFF
    patch = bytearray()
    for i in reversed(indexes):
        patch.append((i ^ state) & 0xFF)
        state = _TABLE[i] ^ (state >> 8)
    data = prefix + bytes(patch)
    assert zlib.crc32(data) == crc32
    return data


def hex_crc(spec: mame.FileSpec) -> int:
    return int(spec.crc32, 16)


class Case:
    """A set, its synthetic files, and the package the preparer writes from them."""

    def __init__(self, spec: gen.SetSpec, bios: str | None = None, with_all_bios: bool = False):
        """bios: the BIOS the package is for (the set's default if None). with_all_bios:
        the zips hold the files of every choice of BIOS, not only of that one."""
        self.spec = spec
        self.name = spec.profile["set"]
        self.cache: dict[tuple[int, int], bytes] = {}
        self.selected_bios = mame._selected_bios(spec.romset, bios)
        self.with_all_bios = with_all_bios

    def data(self, file: mame.FileSpec) -> bytes:
        key = (file.expected_size, hex_crc(file))
        if key not in self.cache:
            self.cache[key] = forge(*key)
        return self.cache[key]

    def own_files(self) -> list[mame.FileSpec]:
        """The set's files that the zips must hold (BIOS files of the other choice are not)."""
        return [f for f in self.spec.romset.files
                if f.bios_id is None or self.with_all_bios or self.spec.romset.bios.get(f.bios_id) == self.selected_bios]

    def package_inputs(self) -> list[mame.FileSpec]:
        """The files that the package is built from."""
        return [f for f in self.spec.romset.files
                if f.bios_id is None or self.spec.romset.bios.get(f.bios_id) == self.selected_bios]

    def aux_files(self) -> list[mame.FileSpec]:
        return [f for _, _, _, owner in self.spec.extra for f in owner.files]

    def package(self, output: Path) -> None:
        """The preparer's package for this data."""
        spec, romset = self.spec, self.spec.romset
        media = {f.index: self.data(f) for f in self.package_inputs()}
        regions = mame.assemble_regions(romset, media, self.selected_bios)
        aux = {}
        for _, _, _, owner in spec.extra:
            aux[owner.name] = mame.assemble_regions(owner, {f.index: self.data(f) for f in owner.files})
        if spec.family == "system10":
            s10_prepare.package_regions(self.name, spec.profile, regions, output)
        elif spec.family == "system11":
            bios = aux["c76"]["internal"][:s11_prepare.EXPECTED_C76["size"]]
            s11_prepare.package_regions(self.name, spec.profile, regions, bios, output)
        else:
            dsp_romset = spec.extra[0][3]
            dsp = aux[dsp_romset.name][next(iter(dsp_romset.regions))]
            zn_prepare.package_regions(self.name, spec.profile, regions, dsp, output,
                                       {"selected_bios": self.selected_bios})

    def package_files(self) -> list[str]:
        return ["board.ini", *(entry[0] for entry in self.spec.regions), *(entry[0] for entry in self.spec.extra)]


def write_zip(path: Path, members: list[bytes], compress: int = zipfile.ZIP_STORED) -> None:
    with zipfile.ZipFile(path, "w") as archive:
        for number, data in enumerate(members):
            archive.writestr(zipfile.ZipInfo(f"{number:03d}.rom"), data, compress_type=compress)


def unique(blobs: list[bytes]) -> list[bytes]:
    seen, result = set(), []
    for blob in blobs:
        key = (len(blob), zlib.crc32(blob))
        if key not in seen:
            seen.add(key)
            result.append(blob)
    return result


def total_size(spec: gen.SetSpec) -> int:
    return sum(f.expected_size for f in spec.romset.files)


def write_set_files(case: Case, rompath: Path, zip_name: str | None = None) -> None:
    """Own zip of the set (every file), and the aux files in their own zips as MAME has them."""
    write_zip(rompath / f"{zip_name or case.name}.zip", unique([case.data(f) for f in case.own_files()]))
    for _, _, _, owner in case.spec.extra:
        write_zip(rompath / f"{owner.name}.zip", [case.data(f) for f in owner.files])


class ArcadeRomsetTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = os.environ.get("CC") or shutil.which("cc")
        if not compiler:
            raise unittest.SkipTest("no host C compiler")
        if not (MAME_SOURCE / "src/mame").is_dir():
            raise unittest.SkipTest(f"no MAME checkout at {MAME_SOURCE}")
        cls.tmp = Path(tempfile.mkdtemp(prefix="arcade-romset-"))
        cls.driver = cls.tmp / "arcade_romset_test"
        arcade = ROOT / "libpcsxcore/arcade"
        subprocess.run([compiler, "-O1", "-Wall", "-Wextra", "-Werror", f"-I{ROOT / 'include'}",
                        f"-I{ROOT / 'libpcsxcore'}", "-o", str(cls.driver), str(ROOT / "tests/arcade_romset_test.c"),
                        *(str(arcade / name) for name in ("arcade_romset.c", "arcade_media.c", "mame_zip.c",
                                                          "mame_romindex.c", "mame_romload.c")), "-lz"], check=True)
        cls.specs = list(gen.loadable_sets(MAME_SOURCE))

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def run_driver(self, *args: object, bios: str | None = None) -> subprocess.CompletedProcess:
        env = {**os.environ, "ARCADE_BIOS": bios} if bios else None
        return subprocess.run([str(self.driver), *map(str, args)], capture_output=True, text=True, env=env)

    def fresh(self, name: str) -> Path:
        path = self.tmp / name
        shutil.rmtree(path, ignore_errors=True)
        path.mkdir()
        return path

    def spec_named(self, name: str) -> gen.SetSpec:
        return next(spec for spec in self.specs if spec.profile["set"] == name)

    def write_set(self, case: Case, rompath: Path, merged_into: str | None = None) -> None:
        write_set_files(case, rompath, merged_into)

    def compare(self, case: Case, rompath: Path, zip_name: str, bios: str | None = None) -> None:
        expected, actual = self.fresh("expected"), self.fresh("actual")
        case.package(expected)
        result = self.run_driver("dump", rompath / zip_name, actual, *case.package_files(), bios=bios)
        self.assertEqual(result.returncode, 0, f"{case.name}: {result.stderr}")
        self.assertEqual(result.stdout.strip(), case.name)
        for name in case.package_files():
            self.assertTrue(filecmp.cmp(expected / name, actual / name, shallow=False),
                            f"{case.name}: {name} differs from the preparer's package")
        # The package is exactly the preparer's, apart from its manifest.
        self.assertEqual(sorted(p.name for p in expected.iterdir() if p.name != "manifest.json"),
                         sorted(case.package_files()))
        shutil.rmtree(expected)
        shutil.rmtree(actual)

    def check_set(self, name: str) -> None:
        case = Case(self.spec_named(name))
        rompath = self.fresh("roms")
        self.write_set(case, rompath)
        self.compare(case, rompath, f"{name}.zip")
        shutil.rmtree(rompath)

    def sample(self) -> list[str]:
        names = []
        for family in ("system10", "system11", "capcom_zn"):
            sets = sorted((s for s in self.specs if s.family == family), key=total_size)
            names += [s.profile["set"] for s in sets[:2]]
        swap = [s for s in self.specs if s.family == "system11" and total_size(s) < 100 << 20 and any(
            isinstance(op, mame.LoadOp) and op.swap for r in s.romset.regions.values() for op in r.operations)]
        names += [s.profile["set"] for s in swap[:1]]
        return list(dict.fromkeys(names))

    def test_table_counts(self):
        counts = {}
        for spec in self.specs:
            counts[spec.family] = counts.get(spec.family, 0) + 1
        self.assertEqual(counts, {"system10": 12, "system11": 35, "capcom_zn": 43})

    def test_packages_equal_the_preparers(self):
        for name in self.sample():
            with self.subTest(set=name):
                self.check_set(name)

    @unittest.skipUnless(os.environ.get("ARCADE_ROMSET_FULL"), "ARCADE_ROMSET_FULL=1 checks all sets")
    def test_all_sets_equal_the_preparers(self):
        for spec in self.specs:
            with self.subTest(set=spec.profile["set"]):
                self.check_set(spec.profile["set"])

    def pair(self) -> tuple[gen.SetSpec, gen.SetSpec]:
        """The smallest System 11 clone and its parent."""
        clones = sorted((s for s in self.specs if s.family == "system11" and s.profile["parent"]),
                        key=lambda s: total_size(s))
        for clone in clones:
            parent = next((s for s in self.specs if s.profile["set"] == clone.profile["parent"]), None)
            if parent and total_size(parent) < 40 << 20:
                return parent, clone
        self.skipTest("no small System 11 parent and clone")

    def test_merged_split_and_renamed_zips(self):
        parent_spec, clone_spec = self.pair()
        parent, clone = Case(parent_spec), Case(clone_spec)
        parent_keys = {(f.expected_size, hex_crc(f)) for f in parent.own_files()}
        # Merged: the clone's files are in the parent's zip, and there is no clone zip.
        rompath = self.fresh("merged")
        write_zip(rompath / f"{parent.name}.zip", unique(
            [parent.data(f) for f in parent.own_files()] + [clone.data(f) for f in clone.own_files()]))
        for _, _, _, owner in parent.spec.extra:
            write_zip(rompath / f"{owner.name}.zip", [parent.data(f) for f in owner.files])
        self.compare(clone, rompath, f"{clone.name}.zip")
        self.compare(parent, rompath, f"{parent.name}.zip")
        # Split: the clone's zip holds only what the parent's does not.
        rompath = self.fresh("split")
        self.write_set(parent, rompath)
        write_zip(rompath / f"{clone.name}.zip", [clone.data(f) for f in clone.own_files()
                                                  if (f.expected_size, hex_crc(f)) not in parent_keys])
        self.compare(clone, rompath, f"{clone.name}.zip")
        # Renamed: a zip with the clone's files under another name is still the clone,
        # and a zip that is not named for a set is identified by its content.
        (rompath / f"{clone.name}.zip").rename(rompath / "mygame.zip")
        write_zip(rompath / f"{clone.name}.zip", [b"not the files of this set"])
        os.remove(rompath / f"{clone.name}.zip")
        result = self.run_driver("identify", rompath / "mygame.zip")
        self.assertEqual(result.stdout.strip(), clone.name, result.stderr)
        self.assertEqual(self.run_driver("scan", rompath).stdout.split(), sorted(
            self.run_driver("scan", rompath).stdout.split()))
        self.assertIn(clone.name, self.run_driver("scan", rompath).stdout.split())
        self.assertIn(parent.name, self.run_driver("scan", rompath).stdout.split())

    def test_incomplete_and_corrupt_sets(self):
        name = self.sample()[0]
        case = Case(self.spec_named(name))
        rompath = self.fresh("broken")
        files = [case.data(f) for f in case.own_files()]
        # A missing file: not complete, not listed, and the log names it.
        write_zip(rompath / f"{name}.zip", unique(files)[1:])
        for _, _, _, owner in case.spec.extra:
            write_zip(rompath / f"{owner.name}.zip", [case.data(f) for f in owner.files])
        self.assertNotIn(name, self.run_driver("scan", rompath).stdout.split())
        result = self.run_driver("dump", rompath / f"{name}.zip", self.fresh("out"), "board.ini")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("crc", result.stderr)
        # A corrupt member passes the directory check but fails its CRC32 when it is read.
        write_zip(rompath / f"{name}.zip", unique(files))
        raw = bytearray((rompath / f"{name}.zip").read_bytes())
        victim = unique(files)[0]
        start = raw.index(victim[:64])
        raw[start + 40] ^= 0xFF
        (rompath / f"{name}.zip").write_bytes(bytes(raw))
        self.assertIn(name, self.run_driver("scan", rompath).stdout.split())
        result = self.run_driver("dump", rompath / f"{name}.zip", self.fresh("out"), *case.package_files())
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("CRC32", result.stderr)

    def test_bios_choice(self):
        spec = min((s for s in self.specs if s.romset.bios), key=total_size)
        default = spec.romset.default_bios
        choices = list(spec.romset.bios.values())
        self.assertGreater(len(choices), 1)
        rompath = self.fresh("bios")
        write_set_files(Case(spec, with_all_bios=True), rompath)
        # Every choice loads its own BIOS; no preference loads the default.
        for bios in choices:
            self.compare(Case(spec, bios, with_all_bios=True), rompath, f"{spec.profile['set']}.zip", bios=bios)
        self.compare(Case(spec, default, with_all_bios=True), rompath, f"{spec.profile['set']}.zip")
        self.compare(Case(spec, default, with_all_bios=True), rompath, f"{spec.profile['set']}.zip", bios="nonsense")
        # Zips that hold only one BIOS load with it, whatever is preferred.
        other = next(c for c in choices if c != default)
        rompath = self.fresh("bios-one")
        write_set_files(Case(spec, other), rompath)
        self.compare(Case(spec, other), rompath, f"{spec.profile['set']}.zip", bios=default)
        self.compare(Case(spec, other), rompath, f"{spec.profile['set']}.zip")

    def test_rom_continue_overwrites(self):
        # Prime Goal EX loads each half of a file over the same bytes ("first & second half identical").
        self.check_set("primglex")

    def test_index_is_kept_until_the_zips_change(self):
        name = self.sample()[0]
        case = Case(self.spec_named(name))
        rompath = self.fresh("cache")
        write_set_files(case, rompath)
        result = self.run_driver("cache", rompath, rompath / f"{name}.zip")
        self.assertEqual(result.stdout.split(), ["same=1", "changed=1"], result.stderr)

    def test_deflated_members(self):
        name = self.sample()[0]
        case = Case(self.spec_named(name))
        rompath = self.fresh("deflated")
        write_zip(rompath / f"{name}.zip", unique([case.data(f) for f in case.own_files()]), zipfile.ZIP_DEFLATED)
        for _, _, _, owner in case.spec.extra:
            write_zip(rompath / f"{owner.name}.zip", [case.data(f) for f in owner.files], zipfile.ZIP_DEFLATED)
        self.compare(case, rompath, f"{name}.zip")


if __name__ == "__main__":
    unittest.main()
