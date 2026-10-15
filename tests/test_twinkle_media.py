"""Original-media audit regressions; local pinned source/ROMs are optional."""
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import audit_twinkle_media as audit
import prepare_mame_psx as mame

SOURCE = ROOT.parent / 'mamedev'
ROMS = ROOT / 'roms'


@unittest.skipUnless((SOURCE / audit.DRIVER).is_file() and
                     all((ROMS / n).is_file() for n in ('gq863.zip', 'bmiidx2.zip')),
                     'requires the local pinned source and original Twinkle chip archives')
class TwinkleMediaTests(unittest.TestCase):
    def test_original_chips_and_word_order(self):
        payloads, report = audit.audit(SOURCE, ROMS, True)
        self.assertEqual(set(payloads), {'bios.bin', 'soundcpu.bin', 'security.bin'})
        self.assertEqual(payloads['soundcpu.bin'][:8], bytes.fromhex('0014000000000086'))
        self.assertTrue(report['rom_chips_verified'])
        self.assertFalse(report['complete_media_verified'])
        self.assertFalse(report['native_profile_available'])
        self.assertFalse(report['board_execution_verified'])
        self.assertEqual(len(report['disks']), 3)
        self.assertTrue(all(not d['verified'] for d in report['disks'].values()))

    def test_corrupt_original_security_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp);shutil.copy2(ROMS / 'gq863.zip', path / 'gq863.zip')
            with zipfile.ZipFile(ROMS / 'bmiidx2.zip') as z:
                data = bytearray(z.read('985a02'));data[7] ^= 1
            with zipfile.ZipFile(path / 'bmiidx2.zip', 'w') as z:
                z.writestr('985a02', data)
            with self.assertRaises(mame.PreparationError):
                audit.audit(SOURCE, path, True)

    def test_missing_hdd_cannot_be_a_complete_audit(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaisesRegex(mame.PreparationError, 'missing original'):
                audit.verify_disk(Path(tmp) / '985hdda01.chd', '985hdda01', 'chdman')

    def test_unreviewed_first_style_is_rejected(self):
        with self.assertRaisesRegex(mame.PreparationError, 'only original IIDX 2nd'):
            audit.source_profile(SOURCE, 'bmiidx')

    @unittest.skipUnless(shutil.which('chdman') and (ROMS / 'ddr2m/895jaa02.chd').is_file(),
                         'requires chdman and the local original DDR CD')
    def test_actual_wrong_game_disc_rejected(self):
        with self.assertRaisesRegex(mame.PreparationError, 'SHA1 mismatch'):
            audit.verify_disk(ROMS / 'ddr2m/895jaa02.chd', 'gc985a01', shutil.which('chdman'))


if __name__ == '__main__':
    unittest.main()
