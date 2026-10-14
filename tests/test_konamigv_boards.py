#!/usr/bin/env python3
"""Source and media-package tests for the standalone System GV inventory."""
from __future__ import annotations

import json
import hashlib
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

import konamigv_boards as boards
import prepare_konamigv_board as prep


class KonamiGVProfilesTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.registry = prep.load_registry()
        cls.profiles = {row["set"]: row for row in cls.registry["profiles"]}

    def test_all_15_source_entries_are_covered(self) -> None:
        rebuilt = boards.build_registry()
        self.assertEqual(self.registry, rebuilt)
        self.assertEqual(15, self.registry["family_game_count"])
        self.assertEqual({"konamigv_state": 10, "tokimeki_state": 4,
                          "simpbowl_state": 1}, self.registry["source_class_counts"])
        self.assertEqual({"konamigv": 8, "btchamp": 1, "kdeadeye": 1,
                          "tmosh": 4, "simpbowl": 1},
                         self.registry["machine_config_counts"])

    def test_each_game_records_rom_and_cd_geometry(self) -> None:
        self.assertEqual(14, sum(not p["is_bios_root"] for p in self.profiles.values()))
        for set_name, profile in self.profiles.items():
            tags = {row["tag"] for row in profile["rom_regions"]}
            self.assertIn("maincpu:rom", tags)
            if set_name == "konamigv":
                self.assertFalse(profile["disk_media"])
                continue
            self.assertEqual({"maincpu:rom", "eeprom"} | ({"flash"} if set_name == "kdeadeye" else set()), tags)
            self.assertEqual(0x80, next(r["size"] for r in profile["rom_regions"] if r["tag"] == "eeprom"))
            self.assertEqual(1, len(profile["disk_media"]))
            self.assertEqual("cdrom", profile["disk_media"][0]["region"])
            self.assertEqual(40, len(profile["disk_media"][0]["sha1"]))

    def test_unsupported_game_io_and_bad_dump_are_explicit(self) -> None:
        self.assertEqual([], self.profiles["hyperath"]["unsupported_game_io"])
        self.assertEqual([], self.profiles["btchamp"]["unsupported_game_io"])
        self.assertTrue(any("printer" in item.lower() for item in self.profiles["tmosh"]["unsupported_game_io"]))
        self.assertEqual([], self.profiles["kdeadeye"]["unsupported_game_io"])
        bad = {profile["set"] for profile in self.registry["profiles"]
               if any(disk["status"] == "BAD_DUMP" for disk in profile["disk_media"])}
        self.assertEqual({"lacrazyc", "susume", "powyak96", "weddingr", "btchamp",
                          "nagano98", "tmoshspa"}, bad)
        self.assertFalse(self.profiles["konamigv"]["media_candidate"])

    def test_load_registry_rejects_incomplete_or_duplicate_sets(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "registry.json"
            doc = json.loads(json.dumps(self.registry))
            doc["profiles"].pop()
            path.write_text(json.dumps(doc))
            with self.assertRaises(prep.KonamiGVPreparationError):
                prep.load_registry(path)

    def test_load_registry_rejects_malformed_shapes_and_untyped_facts(self) -> None:
        malformed = [
            [],
            {**self.registry, "profiles": [None] * 15},
        ]
        for mutate in (
            lambda row: row["rom_regions"][0].update(size=True),
            lambda row: row["rom_regions"][0].update(size=524288.0),
            lambda row: row["rom_regions"][0].update(size=-1),
            lambda row: row["rom_regions"][0].update(tag="unmapped"),
            lambda row: row["rom_regions"].append(dict(row["rom_regions"][0])),
            lambda row: row["disk_media"][0].update(readonly=1),
            lambda row: row.update(year=1996.0),
        ):
            doc = json.loads(json.dumps(self.registry))
            row = next(item for item in doc["profiles"] if item["set"] == "hyperath")
            mutate(row)
            malformed.append(doc)
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "registry.json"
            for doc in malformed:
                path.write_text(json.dumps(doc), encoding="utf-8")
                with self.subTest(document=type(doc).__name__), self.assertRaises(
                        prep.KonamiGVPreparationError):
                    prep.load_registry(path)
            doc = json.loads(json.dumps(self.registry))
            doc["profiles"][-1]["set"] = doc["profiles"][0]["set"]
            path.write_text(json.dumps(doc))
            with self.assertRaises(prep.KonamiGVPreparationError):
                prep.load_registry(path)


class KonamiGVPackagingTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        registry = prep.load_registry()
        cls.profiles = {row["set"]: row for row in registry["profiles"]}

    def _package_fixture(self, set_name: str, out: Path) -> dict:
        profile = self.profiles[set_name]
        regions = {row["tag"]: bytes([i + 1]) * row["size"]
                   for i, row in enumerate(profile["rom_regions"])}
        disk = profile["disk_media"][0]
        disk_file = f"{disk['name']}.chd"
        disk_bytes = b"synthetic validated CHD fixture"
        report = {**disk, "package_file": disk_file,
                  "local_sha256": hashlib.sha256(disk_bytes).hexdigest(),
                  "validation": {"file_version": 5, "header_sha1": disk["sha1"],
                                 "logical_size": "100 bytes",
                                 "tracks": [{"number": 1, "type": "MODE1"},
                                            {"number": 2, "type": "AUDIO"}]}}
        source = {"rom_set": set_name, "parent_chain": [set_name, "konamigv"]}
        return prep.package_regions(set_name, profile, regions, out, source,
                                    {disk_file: disk_bytes}, [report])

    def test_transactional_package_preserves_unrelated_files(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            out = Path(temp) / "package"
            out.mkdir()
            (out / "keep.me").write_text("unrelated user file")
            (out / "nvram.bin").write_bytes(b"persisted settings")
            outside = Path(temp) / "victim.bin"
            outside.write_text("outside package")
            first = self._package_fixture("hyperath", out)
            malicious = json.loads(json.dumps(first))
            malicious["regions"] = {"../../victim.bin": {}, "..\\victim.bin": {},
                                    "keep.me": {}, "nvram.bin": {}}
            (out / "manifest.json").write_text(json.dumps(malicious), encoding="utf-8")
            before = {path.name: path.read_bytes() for path in out.iterdir() if path.is_file()}
            with self.assertRaisesRegex(prep.KonamiGVPreparationError, "claims files outside"):
                self._package_fixture("hyperath", out)
            self.assertEqual(before, {path.name: path.read_bytes()
                                      for path in out.iterdir() if path.is_file()})
            self.assertEqual("outside package", outside.read_text())

            # A valid prior manifest is accepted, while files outside its profile survive.
            (out / "manifest.json").write_text(json.dumps(first), encoding="utf-8")
            valid = self._package_fixture("hyperath", out)
            self.assertTrue((out / "keep.me").is_file())
            self.assertEqual(b"persisted settings", (out / "nvram.bin").read_bytes())
            self.assertEqual("outside package", outside.read_text())
            self.assertIn("eeprom.bin", valid["regions"])
            self.assertIn("gv021-j1.chd", valid["regions"])
            self.assertIn("region.maincpu:rom=bios.bin", (out / "board.ini").read_text())
            self.assertFalse(valid["game_execution_verified"])

    def test_malformed_or_foreign_old_manifest_rejects_without_mutation(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            out = Path(temp) / "package"
            out.mkdir()
            (out / "keep.me").write_bytes(b"keep")
            base = self._package_fixture("hyperath", out)
            cases = [[], {**base, "regions": []},
                     {**base, "mame_driver": "other_driver"},
                     {**base, "mame_source_commit": "0" * 40},
                     {**base, "mame_set": "not_a_gv_game"}]
            for malformed in cases:
                (out / "manifest.json").write_text(json.dumps(malformed), encoding="utf-8")
                before = {path.name: path.read_bytes() for path in out.iterdir() if path.is_file()}
                with self.subTest(manifest=type(malformed).__name__), self.assertRaises(
                        prep.KonamiGVPreparationError):
                    self._package_fixture("hyperath", out)
                self.assertEqual(before, {path.name: path.read_bytes()
                                          for path in out.iterdir() if path.is_file()})

    def test_valid_profile_change_removes_only_old_package_files(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            out = Path(temp) / "package"
            out.mkdir()
            self._package_fixture("kdeadeye", out)
            (out / "keep.me").write_bytes(b"keep")
            (out / "nvram.bin").write_bytes(b"persisted settings")
            self._package_fixture("hyperath", out)
            self.assertFalse((out / "flash.bin").exists())
            self.assertFalse((out / "054uaa01.chd").exists())
            self.assertTrue((out / "gv021-j1.chd").is_file())
            self.assertEqual(b"keep", (out / "keep.me").read_bytes())
            self.assertEqual(b"persisted settings", (out / "nvram.bin").read_bytes())

    def test_bad_profile_and_size_fail_without_touching_output(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            out = Path(temp) / "existing"
            out.mkdir()
            sentinel = out / "keep.me"
            sentinel.write_text("before")
            profile = dict(self.profiles["hyperath"])
            regions = {row["tag"]: bytes(row["size"]) for row in profile["rom_regions"]}
            regions["eeprom"] = b"short"
            with self.assertRaises(prep.KonamiGVPreparationError):
                prep.package_regions("hyperath", profile, regions, out, {})
            self.assertEqual("before", sentinel.read_text())
            self.assertEqual(["keep.me"], sorted(p.name for p in out.iterdir()))
            with self.assertRaises(prep.KonamiGVPreparationError):
                prep.package_regions("konamigv", self.profiles["konamigv"], {}, out, {})
            self.assertEqual("before", sentinel.read_text())

    def test_chd_file_version_requires_an_integer_not_bool(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            out = Path(temp) / "package"
            profile = self.profiles["hyperath"]
            regions = {row["tag"]: bytes(row["size"]) for row in profile["rom_regions"]}
            disk = profile["disk_media"][0]
            disk_file = f"{disk['name']}.chd"
            payload = b"fixture"
            report = {**disk, "package_file": disk_file,
                      "local_sha256": hashlib.sha256(payload).hexdigest(),
                      "validation": {"file_version": True, "header_sha1": disk["sha1"],
                                     "tracks": [{"number": 1, "type": "MODE1"}]}}
            with self.assertRaises(prep.KonamiGVPreparationError):
                prep.package_regions("hyperath", profile, regions, out, {},
                                     {disk_file: payload}, [report])
            self.assertFalse(out.exists())

    @unittest.skipUnless(shutil.which("chdman"), "MAME chdman is required for CHD verification test")
    def test_chdman_accepts_data_only_cd_and_checks_source_sha1(self) -> None:
        chdman = shutil.which("chdman")
        assert chdman is not None
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "data.bin").write_bytes(bytes(2048 * 10))
            (root / "disc.cue").write_text(
                'FILE "data.bin" BINARY\n  TRACK 01 MODE1/2048\n    INDEX 01 00:00:00\n', encoding="ascii")
            chd = root / "fixture.chd"
            subprocess.run([chdman, "createcd", "-i", str(root / "disc.cue"),
                            "-o", str(chd)], check=True, capture_output=True, text=True)
            info = subprocess.run([chdman, "info", "-i", str(chd), "-v"],
                                  check=True, capture_output=True, text=True).stdout
            sha = re.search(r"^SHA1:\s*([0-9a-f]{40})$", info, re.M).group(1)
            report = prep._verify_chd(chd, {"sha1": sha}, chdman)
            self.assertEqual(["MODE1"], [track["type"] for track in report["tracks"]])
            with self.assertRaises(prep.KonamiGVPreparationError):
                prep._verify_chd(chd, {"sha1": "0" * 40}, chdman)

    @unittest.skipUnless(shutil.which("chdman"), "MAME chdman is required for non-CD metadata test")
    def test_valid_non_cd_chd_is_rejected_for_lacking_cd_metadata(self) -> None:
        chdman = shutil.which("chdman")
        assert chdman is not None
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            raw = root / "disk.raw"
            raw.write_bytes(bytes(1024 * 1024))
            chd = root / "disk.chd"
            subprocess.run([chdman, "createhd", "-i", str(raw), "-o", str(chd),
                            "-chs", "128,1,16", "-ss", "512"],
                           check=True, capture_output=True, text=True)
            info = subprocess.run([chdman, "info", "-i", str(chd)],
                                  check=True, capture_output=True, text=True).stdout
            sha = re.search(r"^SHA1:\s*([0-9a-f]{40})$", info, re.M).group(1)
            with self.assertRaisesRegex(prep.KonamiGVPreparationError, "CD track metadata"):
                prep._verify_chd(chd, {"sha1": sha}, chdman)

    def test_legacy_chtr_mode2_metadata_is_accepted_but_unknown_type_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            fake = root / "chdman"
            fake.write_text("""#!/usr/bin/env python3
import sys
if sys.argv[1] == 'info':
 print('File Version: 4')
 print('Logical size: 2,048 bytes')
 print('SHA1: aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa')
 print("Metadata: Tag='CHTR' Index=0 Length=60 bytes")
 print('  TRACK:1 TYPE:MODE2_FORM1 SUBTYPE:NONE FRAMES:1')
else: sys.exit(0)
""", encoding="utf-8")
            fake.chmod(0o755)
            report = prep._verify_chd(root / "image.chd", {"sha1": "a" * 40}, str(fake))
            self.assertEqual("CHTR", report["tracks"][0]["tag"])
            self.assertEqual("MODE2_FORM1", report["tracks"][0]["type"])
            fake.write_text(fake.read_text(encoding="utf-8").replace("MODE2_FORM1", "ALIEN"),
                            encoding="utf-8")
            with self.assertRaisesRegex(prep.KonamiGVPreparationError, "unsupported CD track"):
                prep._verify_chd(root / "image.chd", {"sha1": "a" * 40}, str(fake))
            fake.write_text(fake.read_text(encoding="utf-8").replace("2,048 bytes", "0 bytes"),
                            encoding="utf-8")
            with self.assertRaisesRegex(prep.KonamiGVPreparationError, "supported CHD"):
                prep._verify_chd(root / "image.chd", {"sha1": "a" * 40}, str(fake))


if __name__ == "__main__":
    unittest.main(verbosity=2)
