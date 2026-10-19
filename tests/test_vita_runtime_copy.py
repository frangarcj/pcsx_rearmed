"""Private emulator copies must never share writable inodes with originals."""
import ctypes
import errno
import importlib.util
from pathlib import Path
import stat
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    'vita_runtime_copy', ROOT / 'tools/vita-harness/check_system12_drc_vita3k.py')
runner = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(runner)


class RuntimeCopyTests(unittest.TestCase):
    def test_copy_is_independent_and_preserves_source_permissions(self):
        with tempfile.TemporaryDirectory() as temporary:
            source, destination = Path(temporary) / 'source', Path(temporary) / 'copy'
            source.write_bytes(b'original firmware')
            source.chmod(0o640)
            runner.copy_runtime_file(source, destination)
            self.assertEqual(destination.read_bytes(), source.read_bytes())
            self.assertNotEqual(source.stat().st_ino, destination.stat().st_ino)
            destination.chmod(0o600)
            destination.write_bytes(b'private guest write')
            self.assertEqual(source.read_bytes(), b'original firmware')
            self.assertEqual(stat.S_IMODE(source.stat().st_mode), 0o640)

    def test_unsupported_cloning_falls_back_to_independent_copy(self):
        def unsupported(*_):
            ctypes.set_errno(errno.EXDEV)
            return -1
        with tempfile.TemporaryDirectory() as temporary:
            source, destination = Path(temporary) / 'source', Path(temporary) / 'copy'
            source.write_bytes(b'firmware')
            with patch.object(runner, '_native_clonefile', return_value=unsupported):
                runner.copy_runtime_file(source, destination)
            self.assertEqual(destination.read_bytes(), b'firmware')
            self.assertNotEqual(source.stat().st_ino, destination.stat().st_ino)

    def test_no_full_copy_retry_after_disk_exhaustion(self):
        def full(*_):
            ctypes.set_errno(errno.ENOSPC)
            return -1
        with tempfile.TemporaryDirectory() as temporary:
            source, destination = Path(temporary) / 'source', Path(temporary) / 'copy'
            source.write_bytes(b'firmware')
            with patch.object(runner, '_native_clonefile', return_value=full), \
                    patch.object(runner.shutil, 'copy2') as fallback:
                with self.assertRaises(OSError) as error:
                    runner.copy_runtime_file(source, destination)
                self.assertEqual(error.exception.errno, errno.ENOSPC)
                fallback.assert_not_called()
            self.assertFalse(destination.exists())

    def test_firmware_readonly_mode_does_not_modify_originals(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            original, isolated = root / 'original', root / 'private'
            for device in ('vs0', 'os0', 'sa0'):
                (original / device).mkdir(parents=True)
                (original / device / 'module').write_bytes(b'firmware')
            config = root / 'template.yml'
            config.write_text('pref-path: original/\n')
            runner.make_isolated_config(root / 'private.yml', isolated, original, config)
            for device in ('vs0', 'os0', 'sa0'):
                source, copy = original / device / 'module', isolated / device / 'module'
                self.assertEqual(copy.read_bytes(), source.read_bytes())
                self.assertNotEqual(copy.stat().st_ino, source.stat().st_ino)
                self.assertTrue(source.stat().st_mode & stat.S_IWUSR)
                self.assertFalse(copy.stat().st_mode & stat.S_IWUSR)


if __name__ == '__main__':
    unittest.main()
