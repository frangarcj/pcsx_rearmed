from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import audit_system573_digital_media as board
import prepare_mame_psx as mame
ORIGINAL_NAMES = {'main.bin': '700a01.22g', 'h8_response.bin': 'h8a01.bin',
                  'cassette.bin': 'gka25kaa.u1', 'serial.bin': 'gea25aaa.u6',
                  'digital_id.bin': 'digital-id.bin', 'msu_flash.bin': 'flash.20t',
                  'msu_timekeeper.bin': 'm48t58y.6t'}


class DigitalMedia(unittest.TestCase):
    def test_exact_kaa_configuration(self):
        romset, parents, bios = board.source_profile(ROOT.parent / 'mamedev')
        self.assertEqual(['pcnfrk4m', 'sys573'], mame._lineage('pcnfrk4mk', parents))
        self.assertEqual('std', bios)
        self.assertEqual(set(board.FILES), set(romset.regions))
        self.assertFalse(any(k.startswith('29f016a') or k == 'm48t58' for k in romset.regions))
        selected = [f for f in romset.files if f.bios_id is None or romset.bios[f.bios_id] == bios]
        self.assertEqual({'700a01.22g', 'h8a01.bin', 'gka25kaa.u1', 'gka25kaa.u6'}, {f.filename for f in selected})
        self.assertEqual('good', next(f.status for f in selected if f.filename.endswith('.u1')))
        self.assertEqual('bad_dump', next(f.status for f in selected if f.filename.endswith('.u6')))
        self.assertEqual(board.DISK_SHA, romset.disks[0].sha1)
        self.assertEqual({'k573dio', 'k573msu'}, set(board.source_device_profiles(ROOT.parent / 'mamedev')))

    def test_unreviewed_profiles_rejected(self):
        for name, bios in [('pcnfrk4m', 'std'), ('drmn4m', 'std'), ('ddr2m', 'std'), ('pcnfrk4mk', 'dsem2')]:
            with self.assertRaises(mame.PreparationError):
                board.source_profile(ROOT.parent / 'mamedev', name, bios)

    def original_payloads(self):
        rompath = ROOT / 'roms'
        if not all((rompath / name).is_file() for name in ['sys573.zip', 'pcnfrk4mk.zip', 'pcnfrk4m.zip',
                                                         'k573_dio.zip', 'k573msu.zip']):
            self.skipTest('optional original KAA ROM archives not present')
        return board.audit_roms(ROOT.parent / 'mamedev', rompath)

    def test_original_chip_audit_and_historical_serial_name(self):
        payloads, report = self.original_payloads()
        self.assertTrue(report['rom_chips_verified'])
        self.assertFalse(report['complete_media_verified'])
        self.assertFalse(report['board_execution_verified'])
        self.assertIn(report['source_files']['gka25kaa.u6']['member'], ('gka25kaa.u6', 'gea25aaa.u6'))
        self.assertEqual({'main.bin': 524288, 'h8_response.bin': 64, 'cassette.bin': 140, 'serial.bin': 8,
                          'digital_id.bin': 8, 'msu_flash.bin': 524288, 'msu_timekeeper.bin': 8192},
                         {k: len(v) for k, v in payloads.items()})

    def test_original_serial_corruption_rejected_and_cd_required(self):
        payloads, _ = self.original_payloads()
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            for name, data in payloads.items():
                (root / ORIGINAL_NAMES[name]).write_bytes(data)
            with self.assertRaisesRegex(mame.PreparationError, 'CD.*missing'):
                board.audit(ROOT.parent / 'mamedev', root)
            corrupted = bytearray(payloads['serial.bin']); corrupted[3] ^= 1
            (root / 'gea25aaa.u6').write_bytes(corrupted)
            with self.assertRaisesRegex(mame.PreparationError, 'failed size/CRC/SHA1'):
                board.audit_roms(ROOT.parent / 'mamedev', root)

    def test_other_game_cd_cannot_complete_media_audit(self):
        payloads, _ = self.original_payloads()
        wrong = ROOT / 'roms/ddr2m/895jaa02.chd'
        if not wrong.is_file():
            self.skipTest('optional original DDR CD not present')
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            for name, data in payloads.items():
                (root / ORIGINAL_NAMES[name]).write_bytes(data)
            (root / 'pcnfrk4mk').mkdir()
            (root / 'pcnfrk4mk/a25kaa02.chd').symlink_to(wrong)
            with self.assertRaisesRegex(mame.PreparationError, 'CD SHA1/metadata mismatch'):
                board.audit(ROOT.parent / 'mamedev', root)


if __name__ == '__main__':
    unittest.main()
