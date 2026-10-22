import hashlib
import importlib.util
import json
import sys
import tempfile
import unittest
import zipfile
import zlib
from pathlib import Path


SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / "prepare_mame_psx.py"
SPEC = importlib.util.spec_from_file_location("prepare_mame_psx", SCRIPT)
prepare_mame_psx = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = prepare_mame_psx
SPEC.loader.exec_module(prepare_mame_psx)


def declaration(filename, data, offset=0, macro="ROM_LOAD", extra=""):
    crc = f"{zlib.crc32(data) & 0xffffffff:08x}"
    sha1 = hashlib.sha1(data).hexdigest()
    return f'{macro}("{filename}", {offset:#x}, {len(data):#x}, CRC({crc}) SHA1({sha1}){extra})'


def file_spec(index, filename, data, status="good"):
    crc = f"{zlib.crc32(data) & 0xffffffff:08x}"
    sha1 = hashlib.sha1(data).hexdigest()
    return prepare_mame_psx.FileSpec(index, filename, len(data), crc, sha1, status, None)


class PrepareMamePsxTests(unittest.TestCase):
    def test_extracts_helper_macros_bios_region_metadata_and_disk_pending(self):
        source = '''
        #define BOARD_BIOS \\
          ROM_REGION32_LE(0x10, "bios", ROMREGION_ERASEFF) \\
          ROM_SYSTEM_BIOS(0, "rev_a", "revision A") \\
          ROMX_LOAD("bios.bin", 0, 4, CRC(00000000) SHA1(0000000000000000000000000000000000000000), ROM_BIOS(0))
        ROM_START(game)
          BOARD_BIOS
          ROM_DEFAULT_BIOS("rev_a")
          ROM_REGION16_BE(0x20, "data", ROMREGION_ERASE00)
          DISK_REGION("cdrom")
          DISK_IMAGE_READONLY("disc", 0, SHA1(1111111111111111111111111111111111111111))
        ROM_END
        GAME(2000, game, parent, machine, input, state, init, ROT0, "maker", "title", 0)
        '''
        sets, parents, coverage = prepare_mame_psx.parse_driver(source, "test/driver.cpp")
        romset = sets["game"]
        self.assertEqual(parents["game"], "parent")
        self.assertEqual(coverage[0]["status"], "parsed")
        self.assertEqual(romset.default_bios, "rev_a")
        self.assertEqual(romset.regions["bios"].width, 32)
        self.assertEqual(romset.regions["bios"].endian, "little")
        self.assertEqual(romset.regions["bios"].erase, 0xff)
        self.assertEqual(romset.regions["data"].endian, "big")
        self.assertEqual(romset.disks[0].status if hasattr(romset.disks[0], "status") else romset.disks[0].sha1,
                         "1111111111111111111111111111111111111111")

    def test_assembles_lane_word_swap_fill_continue_reload_and_copy(self):
        blobs = {
            "even": b"ab", "odd": b"cd", "word": b"1234",
            "swap": b"5678", "b32": b"ij", "w32": b"klmn",
            "ws32": b"pqrs", "split": b"ABCDxy", "src": b"xy",
        }
        source = f'''ROM_START(layout)
          ROM_REGION(0x10, "lane", ROMREGION_ERASEFF)
          {declaration("even", blobs["even"], 0, "ROM_LOAD16_BYTE")}
          {declaration("odd", blobs["odd"], 1, "ROM_LOAD16_BYTE")}
          {declaration("word", blobs["word"], 4, "ROM_LOAD16_WORD")}
          {declaration("swap", blobs["swap"], 8, "ROM_LOAD16_WORD_SWAP")}
          ROM_REGION(0x30, "wide", 0)
          {declaration("b32", blobs["b32"], 0, "ROM_LOAD32_BYTE")}
          {declaration("w32", blobs["w32"], 8, "ROM_LOAD32_WORD")}
          {declaration("ws32", blobs["ws32"], 16, "ROM_LOAD32_WORD_SWAP")}
          ROM_REGION(0x10, "continued", 0)
          {declaration("split", b"ABCD", 0)}
          ROM_CONTINUE(4, 2)
          ROM_RELOAD(8, 2)
          ROM_FILL(10, 2, 0xee)
          ROM_REGION(0x10, "copy", ROMREGION_ERASEFF)
          ROM_COPY("continued", 0, 2, 4)
          ROM_END
        '''
        (romset,) = prepare_mame_psx.parse_driver(source, "test/layout.cpp")[0].values()
        data = {
            spec.index: blobs[spec.filename]
            for spec in romset.files
        }
        regions = prepare_mame_psx.assemble_regions(romset, data)
        self.assertEqual(regions["lane"][:12], b"acbd12346587")
        self.assertEqual(regions["lane"][12:], b"\xff" * 4)
        self.assertEqual(regions["wide"][0], ord("i"))
        self.assertEqual(regions["wide"][4], ord("j"))
        self.assertEqual(regions["wide"][8:10], b"kl")
        self.assertEqual(regions["wide"][12:14], b"mn")
        self.assertEqual(regions["wide"][16:18], b"qp")
        self.assertEqual(regions["wide"][20:22], b"sr")
        self.assertEqual(regions["continued"], b"ABCDxy\0\0AB\xee\xee\0\0\0\0")
        self.assertEqual(regions["copy"], b"\xff\xffABCD" + b"\xff" * 10)

    def test_verifies_parent_archive_fallback_and_rejects_bad_digest(self):
        content = b"verified-parent-rom"
        romset = prepare_mame_psx.RomSet(
            "child",
            {"main": prepare_mame_psx.Region("main", len(content), 8, "native", "0", 0)},
            [file_spec(0, "main.bin", content)], [], [], {}, None, "test/driver.cpp",
        )
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            parent = root / "parent.zip"
            with zipfile.ZipFile(parent, "w") as archive:
                archive.writestr("nested/main.bin", content)
            media, origins = prepare_mame_psx._load_rom_files(romset, root, ["parent"], None)
            self.assertEqual(media[0], content)
            self.assertEqual(Path(origins[0]).name, "parent.zip")

            bad_root = root / "bad"
            bad_root.mkdir()
            wrong = bad_root / "bad.zip"
            with zipfile.ZipFile(wrong, "w") as archive:
                archive.writestr("main.bin", b"wrong")
            bad = prepare_mame_psx.RomSet(
                "bad", romset.regions, romset.files, [], [], {}, None,
                romset.source_driver
            )
            with self.assertRaisesRegex(prepare_mame_psx.PreparationError, "failed size/CRC/SHA1"):
                prepare_mame_psx._load_rom_files(bad, bad_root, [], None)
            # The file is found by digest in a differently named ZIP, without
            # requiring the user to rename their existing archive.
            renamed = root / "personal-name.zip"
            with zipfile.ZipFile(renamed, "w") as archive:
                archive.writestr("main.bin", content)
            (found, origins) = prepare_mame_psx._load_rom_files(romset, root, [], None, renamed)
            self.assertEqual(found[0], content)
            self.assertEqual(Path(origins[0]).name, "personal-name.zip")

    def test_rom_continue_may_rewrite_loaded_bytes(self):
        source = f'''ROM_START(again)
          ROM_REGION(2, "r", 0)
          {declaration("a", b"abab", 0).replace("0x4", "0x2")}
          ROM_CONTINUE(0, 2)
          ROM_END'''
        again = prepare_mame_psx.parse_driver(source, "test/x.cpp")[0]["again"]
        data = {spec.index: b"abab" for spec in again.files}
        self.assertEqual(prepare_mame_psx.assemble_regions(again, data)["r"], b"ab")

    def test_fails_closed_on_collision_bounds_nodump_and_unknown_layout(self):
        overlap_source = f'''ROM_START(overlap)
          ROM_REGION(4, "r", 0)
          {declaration("a", b"ab", 0)}
          {declaration("b", b"cd", 1)}
          ROM_END'''
        overlap = prepare_mame_psx.parse_driver(overlap_source, "test/x.cpp")[0]["overlap"]
        data = {spec.index: b"ab" if spec.filename == "a" else b"cd" for spec in overlap.files}
        with self.assertRaisesRegex(prepare_mame_psx.PreparationError, "overlapping"):
            prepare_mame_psx.assemble_regions(overlap, data)

        bounds_source = f'''ROM_START(bounds)
          ROM_REGION(2, "r", 0)
          {declaration("a", b"abc", 0)}
          ROM_END'''
        bounds = prepare_mame_psx.parse_driver(bounds_source, "test/x.cpp")[0]["bounds"]
        with self.assertRaisesRegex(prepare_mame_psx.PreparationError, "exceeds region"):
            prepare_mame_psx.assemble_regions(bounds, {0: b"abc"})

        nodump, _, _ = prepare_mame_psx.parse_driver('''
          ROM_START(nodump) ROM_REGION(4, "r", 0)
          ROM_LOAD("secret.key", 0, 4, NO_DUMP) ROM_END
        ''', "test/x.cpp")
        with tempfile.TemporaryDirectory() as temp:
            with self.assertRaisesRegex(prepare_mame_psx.PreparationError, "NO_DUMP"):
                prepare_mame_psx._load_rom_files(nodump["nodump"], Path(temp), [], None)

        unsupported, _, report = prepare_mame_psx.parse_driver('''
          ROM_START(unsupported) ROM_REGION(4, "r", 0)
          ROM_LOAD24_WORD("odd.bin", 0, 2, CRC(00000000) SHA1(0000000000000000000000000000000000000000))
          ROM_END
        ''', "test/x.cpp")
        self.assertNotIn("unsupported", unsupported)
        self.assertEqual(report[0]["status"], "unsupported")
        self.assertIn("unsupported ROM load layout", report[0]["reason"])

    def test_bios_selection_filters_alternative_file_and_digest_is_required(self):
        source = '''
          ROM_START(bios)
            ROM_REGION(8, "rom", ROMREGION_ERASEFF)
            ROM_SYSTEM_BIOS(0, "a", "A")
            ROMX_LOAD("a.bin", 0, 2, CRC(00000000) SHA1(0000000000000000000000000000000000000000), ROM_BIOS(0))
            ROM_SYSTEM_BIOS(1, "b", "B")
            ROMX_LOAD("b.bin", 0, 2, CRC(00000000) SHA1(0000000000000000000000000000000000000000), ROM_BIOS(1))
          ROM_END
        '''
        (romset,) = prepare_mame_psx.parse_driver(source, "test/bios.cpp")[0].values()
        self.assertEqual(prepare_mame_psx._selected_bios(romset, None), "a")
        self.assertEqual(prepare_mame_psx._selected_bios(romset, "b"), "b")
        with self.assertRaisesRegex(prepare_mame_psx.PreparationError, "unknown BIOS"):
            prepare_mame_psx._selected_bios(romset, "missing")
        missing_hash, _, missing_hash_coverage = prepare_mame_psx.parse_driver('''
              ROM_START(hashless) ROM_REGION(4, "rom", 0)
              ROM_LOAD("x.bin", 0, 1, 0) ROM_END
            ''', "test/bios.cpp")
        self.assertNotIn("hashless", missing_hash)
        self.assertIn("requires CRC and SHA1", missing_hash_coverage[0]["reason"])


if __name__ == "__main__":
    unittest.main()
