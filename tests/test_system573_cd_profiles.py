import sys
import unittest
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import prepare_system573_cd_board as board
import prepare_mame_psx as mame


class System573CDProfiles(unittest.TestCase):
    def test_exact_original_cassette_and_cd_identity(self):
        r, _, bios = board.source_profile(ROOT.parent / 'mamedev')
        selected = [f for f in r.files if f.bios_id is None or r.bios[f.bios_id] == bios]
        self.assertEqual({'700a01.22g', 'h8a01.bin', 'gn895jaa.u1'}, {f.filename for f in selected})
        cassette = next(f for f in selected if f.filename == 'gn895jaa.u1')
        self.assertEqual(('bad_dump', 548, '5d906be5', '4ea9c5506aaaf1726f2a39d0a37a8df35a6aad47'),
                         (cassette.status, cassette.expected_size, cassette.crc32, cassette.sha1))
        self.assertEqual('cfe3a6f3ed62ba388b07045e29e22472d17dcfe4', r.disks[0].sha1)
        self.assertTrue(r.disks[0].readonly)
        self.assertNotIn('mcu', r.regions)
        self.assertEqual(set(board.FILES), set(r.regions))

    def test_erased_flash_and_timekeeper_are_not_fabricated_dumps(self):
        r, _, _ = board.source_profile(ROOT.parent / 'mamedev')
        self.assertFalse(any(tag.startswith('29f016a') or tag == 'm48t58' for tag in r.regions))

    def test_unreviewed_bios_games_rejected(self):
        for name, bios in [('ddr2ml', 'std'), ('pcnfrk4mk', 'std'), ('hyperbbc', 'std'), ('ddr2m', 'dsem2')]:
            with self.assertRaises(mame.PreparationError):
                board.source_profile(ROOT.parent / 'mamedev', name, bios)


if __name__ == '__main__':
    unittest.main()
