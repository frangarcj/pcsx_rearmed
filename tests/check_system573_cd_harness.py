#!/usr/bin/env python3
"""Original DDR loader/NVRAM checks; two-frame runs do not prove gameplay."""
import argparse
import struct
import subprocess
import tempfile
import zlib
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--harness', required=True, type=Path)
    parser.add_argument('--media', required=True, type=Path)
    parser.add_argument('--installed-nvram', type=Path)
    parser.add_argument('--reject-cd', action='store_true')
    args = parser.parse_args()
    args.harness = args.harness.resolve()
    with tempfile.TemporaryDirectory(prefix='pcsx-ddr-original-check-') as directory:
        root = Path(directory)
        for name in ('main.bin', 'h8_response.bin', 'cassette.bin', '895jaa02.chd'):
            (root / name).symlink_to((args.media / name).resolve())
        original_ini = (args.media / 'board.ini').read_text()
        (root / 'board.ini').write_text(original_ini)
        controls = ['p1left', 'p1right', 'p1up', 'p1down', 'p2left', 'p2right', 'p2up', 'p2down',
                    'p1button4', 'p1button5', 'p1button6', 'p2button4', 'p2button5', 'p2button6', 'boot_cd']
        rows = [(0, c, 1) for c in controls] + [(1, c, 0) for c in controls]
        (root / 'inputs.csv').write_text('frame,control,pressed\n' + ''.join(f'{f},{c},{v}\n' for f,c,v in rows))
        state = root / 'state.nv'
        def run(extra=()):
            result = subprocess.run([str(args.harness), '--system573', str(root), '--frames', '2',
                                     '--output', str(root / 'out.ppm'), *extra],
                                    capture_output=True, text=True, timeout=30)
            return result, result.stdout + result.stderr
        if args.reject_cd:
            result, output = run()
            assert result.returncode and 'requires HAVE_CHD' in output, output
            print('DDR original CD configuration fails closed without HAVE_CHD PASS')
            return
        assert args.installed_nvram, '--installed-nvram required for positive persistence checks'
        seed = args.installed_nvram.read_bytes()
        assert len(seed) == 32 + 0x1000000 + 8192 + 548 + 65536
        assert seed[:16] == b'S573NV2\0ddr2m\0\0\0'
        assert struct.unpack_from('<II', seed, 16) == (len(seed), zlib.crc32(seed[32:]))
        state.write_bytes(seed)
        result, output = run(('--nvram', str(state), '--board-test', '--inputs', str(root / 'inputs.csv')))
        assert not result.returncode, output
        for token in ('DDR native input selftest PASS', 'NVRAM=RESTORED', 'faults=0', 'consumed=30/30'):
            assert token in output, (token, output)
        snapshot = state.read_bytes()
        # RTC counters refresh from the host; flash, cassette and writable drive buffer persist.
        assert snapshot[32:32+0x1000000] == seed[32:32+0x1000000]
        assert snapshot[32+0x1002000:] == seed[32+0x1002000:]
        for contents in (seed[:-1], b'X' + seed[1:], seed[:24] + b'X' + seed[25:],
                         seed[:100] + bytes([seed[100]^1]) + seed[101:]):
            state.write_bytes(contents)
            result, output = run(('--nvram', str(state)))
            assert result.returncode and 'NVRAM' in output, output
        for name in ('main.bin', 'h8_response.bin', 'cassette.bin'):
            target = root / name
            target.unlink()
            contents = bytearray((args.media / name).read_bytes());contents[-1] ^= 1
            target.write_bytes(contents)
            result, output = run()
            assert result.returncode and f'original {name} size/CRC mismatch' in output, output
            target.unlink();target.symlink_to((args.media / name).resolve())
        for contents in (original_ini + 'cassette=X\n', original_ini.replace('cassette=X', 'cassette=Y'),
                         original_ini.replace('mame_set=ddr2m', 'mame_set=ddr2ml')):
            (root / 'board.ini').write_text(contents)
            result, output = run()
            assert result.returncode and 'invalid or unsupported original profile' in output, output
        print('DDR original BIOS/H8/cassette identity, 2P stage/panel/DIP controls, full flash/cassette/drive NVRAM and malformed profile/media/state rejection PASS')


if __name__ == '__main__':
    main()
