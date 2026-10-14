import sys
import unittest
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import prepare_system573_board as board
import prepare_mame_psx as mame


class System573Profiles(unittest.TestCase):
    def test_flash_only_geometry_and_exact_bios_selection(self):
        romset, _, bios = board.source_profile(ROOT.parent / 'mamedev', 'hyperbbc')
        self.assertEqual('std', bios)
        self.assertEqual(set(board.FILES), set(romset.regions))
        self.assertEqual([], romset.disks)
        selected = [f for f in romset.files if f.bios_id is None or romset.bios[f.bios_id] == bios]
        self.assertEqual(11, len(selected))
        self.assertTrue(all(f.status == 'good' for f in selected))
        self.assertEqual({'700a01.22g', 'h8a01.bin', '876ea.22h',
                          *('876ea.' + tag.split('.')[1] for tag in board.FLASH_TAGS)},
                         {f.filename for f in selected})
        self.assertEqual(0x80000, romset.regions['maincpu:rom'].size)
        self.assertEqual(0x40, romset.regions['h8_response'].size)
        self.assertTrue(all(romset.regions[tag].size == 0x200000 for tag in board.FLASH_TAGS))
        self.assertEqual(0x2000, romset.regions['m48t58'].size)

    def test_undumped_firmware_is_not_assembled(self):
        romset, _, _ = board.source_profile(ROOT.parent / 'mamedev', 'hyperbbc')
        self.assertNotIn('mcu', romset.regions)
        self.assertFalse(any(f.status == 'no_dump' for f in romset.files))
        self.assertFalse(any(f.filename == 'hd6473644h.18e' for f in romset.files))

    def test_native_media_checks_match_selected_source_chips(self):
        romset, _, bios = board.source_profile(ROOT.parent / 'mamedev', 'hyperbbc')
        expected = []
        for tag, filename in board.FILES.items():
            specs = [next(f for f in romset.files if f.index == op.file_index)
                     for op in romset.regions[tag].operations]
            spec = next(f for f in specs if f.bios_id is None or romset.bios[f.bios_id] == bios)
            expected.append((filename, spec.expected_size, int(spec.crc32, 16)))
        native = (ROOT / 'libpcsxcore/arcade/system573_media.inc').read_text()
        actual = [(name, int(size, 16), int(crc, 16))
                  for name, size, crc in re.findall(r'\{"([^"]+)", (0x[\da-f]+), (0x[\da-f]+)u\}', native)]
        self.assertEqual(expected, actual)

    def test_unreviewed_variants_and_bios_rejected(self):
        for name, bios in [('hyperbbca', 'std'), ('ddr2m', 'std'), ('pcnfrk4mk', 'std'),
                           ('hyperbbc', 'dsem2'), ('hyperbbc', 'gchgchmp')]:
            with self.assertRaises(mame.PreparationError):
                board.source_profile(ROOT.parent / 'mamedev', name, bios)


if __name__ == '__main__':
    unittest.main()
