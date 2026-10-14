import sys
import unittest
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import prepare_metro_zn_board as metro
import generate_metro_zn_profiles as native
import prepare_mame_psx as mame


class MetroProfiles(unittest.TestCase):
    def test_exact_variants_and_undumped_firmware_remain_explicit(self):
        for name in metro.SETS:
            romset, _, config = metro.source_profile(ROOT.parent / 'mamedev', name)
            self.assertEqual(set(metro.TAGS), set(romset.regions))
            self.assertEqual((0x80000, 0x2c00000, 8, 8), tuple(romset.regions[t].size for t in metro.TAGS))
            self.assertEqual(15, len(romset.files))
            self.assertTrue(all(f.status == 'good' for f in romset.files))
            self.assertEqual(metro.DISKS[name], (romset.disks[0].name, romset.disks[0].region, romset.disks[0].sha1))
            self.assertEqual('false', config['board_execution_verified'])
        self.assertEqual(('hd64f3644.u2', 32768), metro.OMITTED['h83644'])
        self.assertEqual(('78081g503.ic655', 8192), metro.OMITTED['upd78081'])

    def test_generated_native_rom_checks_are_current(self):
        self.assertEqual((ROOT / 'libpcsxcore/arcade/metro_zn_profiles.inc').read_text(),
                         native.generate(ROOT.parent / 'mamedev'))

    def test_unreviewed_sets_rejected(self):
        for name in ('coh1002e', 'bldyror2', 'bustamove', 'bam2x'):
            with self.assertRaises(mame.PreparationError):
                metro.source_profile(ROOT.parent / 'mamedev', name)


if __name__ == '__main__':
    unittest.main()
