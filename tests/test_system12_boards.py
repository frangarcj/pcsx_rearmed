import hashlib
import importlib.util
import json
import tempfile
import unittest
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts" / "prepare_system12_board.py"
SPEC = importlib.util.spec_from_file_location("prepare_system12_board", SCRIPT)
module = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(module)


class System12BoardProfiles(unittest.TestCase):
    def test_registry_covers_every_mame_system12_alias_and_board_state(self):
        registry = json.loads((ROOT / "scripts/system12_boards.json").read_text())
        inventory = json.loads((ROOT / "scripts/mame_psx_systems.json").read_text())
        system = next(item for item in inventory["systems"] if item["family"].startswith("Namco System 12"))
        profiles = registry["sets"]
        self.assertEqual(registry["mame_source_commit"], module.COMMIT)
        self.assertEqual(len(profiles), 58)
        self.assertEqual({p["set"] for p in profiles}, {p["set"] for p in system["sets"]})
        self.assertTrue(all(p["ram_size"] in (0x400000, 0x1000000) for p in profiles))
        self.assertTrue(all(p["boot_hack"] == (p["machine"] in ("coh700b", "coh716", "golgo13", "kartduel", "cdxa_pcb", "truckk")) for p in profiles))
        self.assertEqual({p["machine"] for p in profiles}, {"coh700", "coh700b", "coh716", "aplarail", "technodr", "golgo13", "cdxa_pcb", "truckk", "kartduel"})
        self.assertEqual({p["set"] for p in profiles if p["alt_bank"]}, {"mdhorse", "kaiunqz", "sws2001", "golgo13", "g13knd", "ujlnow", "truckk"})
        for profile in profiles:
            self.assertEqual(profile["tektagt_dma"], profile["state"] == "tektagt_state")
        # The protection is emulated (MAME tektagt_state): no set needs anything else.
        self.assertEqual(next(p for p in profiles if p["set"] == "tektagt")["unsupported"], [])
        self.assertEqual(next(p for p in profiles if p["set"] == "ptblank2")["unsupported"], [])
        self.assertEqual(next(p for p in profiles if p["set"] == "kartduel")["unsupported"], [])

    def test_packs_dynamic_mame_region_sizes_and_declares_unsupported_devices(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            inputs, output = root / "input", root / "output"
            inputs.mkdir()
            region_data = {
                "maincpu:rom": b"m" * 0x400000,
                "bankedroms": b"b" * 0x800000,
                "sub": b"s" * 0x80000,
                "c352": b"w" * 0x800000,
            }
            regions = {}
            for i, (tag, data) in enumerate(region_data.items()):
                name = f"{i:03d}_region.bin"
                (inputs / name).write_bytes(data)
                regions[tag] = {"file": name, "size": len(data),
                    "crc32": f"{zlib.crc32(data) & 0xffffffff:08x}",
                    "sha1": hashlib.sha1(data).hexdigest()}
            (inputs / "manifest.json").write_text(json.dumps({
                "mame_set": "sws2001", "mame_driver": "namco/namcos12.cpp",
                "mame_source_commit": module.COMMIT,
                "rom_regions_assembled": True, "regions": regions,
                "pending_disk_media": [],
            }))
            manifest = module.prepare(inputs, output)
            cfg = (output / "board.cfg").read_text()
            self.assertIn("ram_size=16777216", cfg)
            self.assertIn("bank_size=8388608", cfg)
            self.assertIn("alt_bank=1", cfg)
            self.assertIn("boot_hack=1", cfg)
            self.assertEqual((output / "wave.bin").stat().st_size, 0x800000)
            self.assertEqual(manifest["unsupported"], [])

    def test_marks_profiles_with_external_devices_and_detects_corrupt_region(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            inputs, output = root / "input", root / "output"
            inputs.mkdir()
            data = {"maincpu:rom": b"a", "bankedroms": b"b", "sub": b"c", "c352": b"d"}
            regions = {}
            for i, (tag, body) in enumerate(data.items()):
                name = f"{i}.bin"; (inputs / name).write_bytes(body)
                regions[tag] = {"file": name, "size": len(body), "crc32": f"{zlib.crc32(body) & 0xffffffff:08x}", "sha1": hashlib.sha1(body).hexdigest()}
            manifest = {"mame_set": "truckk", "mame_driver": "namco/namcos12.cpp",
                "mame_source_commit": module.COMMIT, "rom_regions_assembled": True, "regions": regions}
            (inputs / "manifest.json").write_text(json.dumps(manifest))
            registry = json.loads(module.REGISTRY.read_text())
            next(p for p in registry["sets"] if p["set"] == "truckk")["unsupported"] = ["future_device"]
            registry_path = root / "registry.json"
            registry_path.write_text(json.dumps(registry))
            result = module.prepare(inputs, output, registry_path)
            self.assertEqual(result["unsupported"], ["future_device"])
            (inputs / "0.bin").write_bytes(b"X")
            with self.assertRaises(module.BoardPreparationError):
                module.prepare(inputs, output)


if __name__ == "__main__":
    unittest.main()
