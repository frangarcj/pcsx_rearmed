from __future__ import annotations

import hashlib
import json
import sys
import tempfile
import unittest
from unittest import mock
import zipfile
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

import prepare_mame_psx as mame
import prepare_taito_fx1_board as prepare
import taito_fx1_boards as profiles


class TaitoFX1RegistryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.registry = profiles.build_registry()
        cls.by_set = {row["set"]: row for row in cls.registry["profiles"]}

    def test_registry_covers_all_fx1_classes_and_configurations(self):
        self.assertEqual(self.registry["mame_source_commit"], mame.PINNED_MAME_COMMIT)
        self.assertEqual(self.registry["mame_driver"], "sony/zn.cpp")
        self.assertEqual(self.registry["source_complete_rom_start_count"], 116)
        self.assertEqual(self.registry["family_game_count"], 22)
        self.assertEqual(self.registry["class_counts"], {
            "taito_fx1s_state": 10, "taito_fx1z_state": 8, "gdarius_state": 4,
        })
        self.assertEqual(self.registry["board_type_counts"], {"FX1S": 10, "FX1Z": 12})
        self.assertEqual(self.registry["bios_roots"], ["coh1000t"])
        candidates = [row["set"] for row in self.by_set.values() if row["runtime_candidate"]]
        self.assertEqual(set(candidates), {
            "sfchamp", "sfchampo", "sfchampu", "sfchampj", "psyforce",
            "psyforcej", "psyforcex", "mgcldate", "mgcldtex",
        })

    def test_fx1s_configuration_audio_geometry_and_regions_are_exact(self):
        row = self.by_set["psyforce"]
        self.assertEqual((row["board_type"], row["board_model"], row["board_family"]),
                         ("FX1S", "COH1000T", "Taito FX-1"))
        self.assertEqual((row["cpu_device"], row["cpu_clock_hz"], row["gpu_device"],
                          row["gpu_clock_hz"], row["main_ram_size"], row["vram_size"]),
                         ("CXD8530CQ", 67737600, "CXD8561Q", 53693175, 0x400000, 0x100000))
        self.assertEqual((row["sound_cpu_device"], row["sound_cpu_clock_hz"],
                          row["sound_device"], row["sound_clock_hz"],
                          row["sound_latch_device"], row["sample_region"]),
                         ("Z80", 4000000, "YM2610B", 8000000,
                          "TC0140SYT", "ym2610b:adpcma"))
        self.assertEqual((row["watchdog_device"], row["bank_select_register"],
                          row["bank_select_mask"], row["watchdog_clock_bit"]),
                         ("MB3773", 0x1fb40000, 3, 5))
        tags = {region["tag"]: region["size"] for region in row["rom_regions"]}
        self.assertEqual(tags, {
            "maincpu:rom": 0x80000, "cat702_1": 8, "cat702_2": 8,
            "upd78081": 0x2000, "bankedroms": 0x1000000,
            "audiocpu": 0x20000, "ym2610b:adpcma": 0x200000,
        })
        self.assertEqual(row["bios_parent"], "coh1000t")

    def test_all_set_sample_region_sizes_and_variant_rejections(self):
        self.assertEqual(self.by_set["mgcldtex"]["rom_regions"][-2]["tag"], "ym2610b:adpcma")
        mgcl = next(r for r in self.by_set["mgcldtex"]["rom_regions"]
                    if r["tag"] == "ym2610b:adpcma")
        self.assertEqual(mgcl["size"], 0x400000)
        zrom = self.by_set["raystorm"]
        self.assertFalse(zrom["runtime_candidate"])
        self.assertEqual(zrom["board_type"], "FX1Z")
        self.assertIn("taito_zoom:mn10200", {r["tag"] for r in zrom["rom_regions"]})
        gdarius = self.by_set["gdarius"]
        self.assertFalse(gdarius["runtime_candidate"])
        self.assertEqual(gdarius["state"], "gdarius_state")
        self.assertEqual(gdarius["board_model"], "COH1002TB")

    def test_undumped_region_exception_is_only_recorded_for_fx1s(self):
        fx1s = self.by_set["sfchamp"]["optional_undumped_media_review"]
        self.assertEqual(fx1s["tag"], "upd78081")
        self.assertEqual(fx1s["filename"], "78081g503.ic655")
        self.assertEqual(fx1s["size"], 0x2000)
        self.assertEqual(fx1s["handling"], "omitted_from_media_package_no_substitute")
        self.assertIn("ZNMCU", fx1s["source_proof"])
        self.assertIsNone(self.by_set["raystorm"]["optional_undumped_media_review"])

    def test_generator_is_deterministic(self):
        regenerated = profiles.build_registry()
        expected = json.loads(profiles.DEFAULT_OUTPUT.read_text(encoding="utf-8"))
        self.assertEqual(regenerated, expected)


class TaitoFX1PackagingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.by_set = {row["set"]: row for row in profiles.build_registry()["profiles"]}

    def test_package_emits_provenance_and_preserves_unrelated_files(self):
        profile = self.by_set["psyforce"]
        sizes = {region["tag"]: region["size"] for region in profile["rom_regions"]}
        regions = {tag: bytes([index]) * sizes[tag]
                   for index, tag in enumerate(profiles.FX1S_REQUIRED_REGIONS)}
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp) / "package"
            output.mkdir()
            (output / "keep.txt").write_text("sentinel", encoding="utf-8")
            manifest = prepare.package_regions("psyforce", profile, regions, output,
                                               {"parent_chain": ["coh1000t"],
                                                "media_origins": {"bios.bin": "/roms/coh1000t.zip"}})
            self.assertEqual((output / "keep.txt").read_text(encoding="utf-8"), "sentinel")
            self.assertEqual((output / "main.bin").read_bytes(), regions["maincpu:rom"])
            self.assertEqual((output / "adpcma.bin").read_bytes(), regions["ym2610b:adpcma"])
            ini = (output / "board.ini").read_text(encoding="utf-8")
            self.assertIn("format=pcsx-aow-taito-fx1-v1", ini)
            self.assertIn("sound_clock_hz=8000000", ini)
            self.assertIn("bank_select_register=0x1fb40000", ini)
            self.assertFalse(manifest["board_execution_verified"])
            self.assertEqual(manifest["omitted_media"][0]["filename"], "78081g503.ic655")
            metadata = json.loads((output / "manifest.json").read_text(encoding="utf-8"))
            self.assertEqual(metadata["regions"]["cat702_key0.bin"]["size"], 8)
            self.assertEqual(metadata["media_source"]["parent_chain"], ["coh1000t"])

    def test_package_rejects_wrong_source_size_and_unsupported_sets(self):
        profile = self.by_set["sfchamp"]
        sizes = {region["tag"]: region["size"] for region in profile["rom_regions"]}
        regions = {tag: bytes(sizes[tag]) for tag in profiles.FX1S_REQUIRED_REGIONS}
        regions["audiocpu"] = b"short"
        with tempfile.TemporaryDirectory() as temp:
            with self.assertRaises(prepare.TaitoFX1PreparationError):
                prepare.package_regions("sfchamp", profile, regions, Path(temp), {})
            with self.assertRaises(prepare.TaitoFX1PreparationError):
                prepare.package_regions("raystorm", self.by_set["raystorm"], {}, Path(temp), {})
            with self.assertRaises(prepare.TaitoFX1PreparationError):
                prepare.package_regions("coh1000t", self.by_set["coh1000t"], {}, Path(temp), {})

    def test_reviewed_no_dump_filter_does_not_apply_to_other_variants(self):
        source = (profiles.DEFAULT_SOURCE / "src/mame" / profiles.DRIVER).read_text()
        sets, _, _ = mame.parse_driver(source, profiles.DRIVER)
        filtered = prepare._filter_reviewed_upd78081(sets["sfchamp"], self.by_set["sfchamp"])
        self.assertNotIn("upd78081", filtered.regions)
        self.assertIn("cat702_1", filtered.regions)
        self.assertIn("cat702_2", filtered.regions)
        with self.assertRaises(prepare.TaitoFX1PreparationError):
            prepare._filter_reviewed_upd78081(sets["raystorm"], self.by_set["raystorm"])

    def test_filtered_rom_origins_keep_source_indices(self):
        source = (profiles.DEFAULT_SOURCE / "src/mame" / profiles.DRIVER).read_text()
        sets, _, _ = mame.parse_driver(source, profiles.DRIVER)
        filtered = prepare._filter_reviewed_upd78081(sets["psyforce"], self.by_set["psyforce"])
        origins = {spec.index: f"archive-{spec.index}" for spec in filtered.files}
        self.assertGreater(max(origins), len(filtered.files) - 1)
        regions = {tag: b"" for tag in profiles.FX1S_REQUIRED_REGIONS}
        with mock.patch.object(prepare, "assemble_verified", return_value=(regions, origins)):
            _, metadata = prepare._load_set("psyforce", profiles.DEFAULT_SOURCE,
                                            Path("unused"), self.by_set["psyforce"], None)
        self.assertEqual(metadata["media_origins"],
                         {spec.filename: f"archive-{spec.index}" for spec in filtered.files})
        self.assertNotIn("78081g503.ic655", metadata["media_origins"])

    def test_synthetic_archive_resolves_clone_media_from_parent_and_checks_hashes(self):
        bios = b"BIOS"
        game = b"GAME"
        digest = lambda data: (f"{zlib.crc32(data) & 0xffffffff:08x}", hashlib.sha1(data).hexdigest())
        bcrc, bsha = digest(bios)
        gcrc, gsha = digest(game)
        source = f'''
ROM_START( child )
 ROM_REGION( 0x4, "maincpu:rom", 0 )
 ROM_LOAD( "bios.bin", 0, 4, CRC({bcrc}) SHA1({bsha}) )
 ROM_REGION( 0x4, "bankedroms", 0 )
 ROM_LOAD( "game.bin", 0, 4, CRC({gcrc}) SHA1({gsha}) )
ROM_END
GAME( 1995, child, parent, machine, input, state, init, ROT0, "Taito", "Fixture", MACHINE_SUPPORTS_SAVE )
'''
        sets, parents, _ = mame.parse_driver(source, profiles.DRIVER)
        with tempfile.TemporaryDirectory() as temp:
            rompath = Path(temp)
            with zipfile.ZipFile(rompath / "parent.zip", "w") as archive:
                archive.writestr("bios.bin", bios)
            with zipfile.ZipFile(rompath / "child.zip", "w") as archive:
                archive.writestr("game.bin", game)
            assembled, origins = prepare.assemble_verified(sets["child"], rompath, parents)
            self.assertEqual(assembled["maincpu:rom"], bios)
            self.assertEqual(assembled["bankedroms"], game)
            self.assertTrue(origins)
            with zipfile.ZipFile(rompath / "child.zip", "w") as archive:
                archive.writestr("game.bin", b"BAD!")
            with self.assertRaises(prepare.TaitoFX1PreparationError):
                prepare.assemble_verified(sets["child"], rompath, parents)


if __name__ == "__main__":
    unittest.main()
