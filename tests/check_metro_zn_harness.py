#!/usr/bin/env python3
"""Run generated MIPS Metro bus/cabinet checks without any game or H8 firmware."""
import argparse
import struct
import subprocess
import sys
import tempfile
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import prepare_metro_zn_board as metro


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--harness', type=Path, required=True)
    args = p.parse_args()
    source = Path(__file__).resolve().parents[2] / 'mamedev'
    with tempfile.TemporaryDirectory(prefix='pcsx-metro-') as temp:
        root = Path(temp)
        _, _, config = metro.source_profile(source, 'bam2')
        ini = ''.join(f'{k}={v}\n' for k, v in config.items())
        (root / 'board.ini').write_text(ini)
        (root / 'main.bin').write_bytes(bytes(0x80000))
        data = bytearray(0x2c00000)
        for page in range(11):
            struct.pack_into('<I', data, page * 0x400000, 0x11110000 + page)
        (root / 'banked.bin').write_bytes(data)
        (root / 'cat702_key0.bin').write_bytes(bytes(range(8)))
        (root / 'cat702_key1.bin').write_bytes(bytes(range(8, 16)))
        csv = root / 'inputs.csv'
        csv.write_text('frame,control,pressed\n0,cabinet_generic,1\n'
                       '0,p1left,1\n0,p1button1,1\n0,p2down,1\n'
                       '0,coin1,1\n0,p1start,1\n'
                       '1,p1left,0\n1,p1button1,0\n1,p2down,0\n1,coin1,0\n1,p1start,0\n')

        def run(selector='--metro-zn-fixture', extra=()):
            return subprocess.run([str(args.harness), selector, str(root), '--board-test',
                                   '--frames', '2', '--inputs', str(csv),
                                   '--dump-ram', str(root / 'ram.bin'), '--output', str(root / 'out.ppm'), *extra],
                                  capture_output=True, text=True, timeout=30)

        test = run()
        output = test.stdout + test.stderr
        assert test.returncode == 0, output
        words = struct.unpack_from('<10I', (root / 'ram.bin').read_bytes(), 0x100)
        expected = (0x11110000, 0x11110001, 0x1111000a, 1, 1, 4, 0xeb, 0xfd, 0xee, 0x4d455452)
        assert words == expected, (words, output)
        for token in ['SystemZN selftest: PASS', 'MetroZN SPU DMA fixture: PASS',
                      'media=SYNTHETIC_FIXTURE', 'H8=NO_DUMP ATA_transport=UNIMPLEMENTED',
                      'faults=0', 'consumed=11/11']:
            assert token in output, (token, output)
        # This generated material cannot pass the original-ROM loader.
        original = run('--metro-zn')
        assert original.returncode != 0 and 'source ROM CRC mismatch' in original.stderr, original.stderr
        csv.write_text(csv.read_text().replace('0,cabinet_generic,1\n', ''))
        generic = run(extra=('--metro-cabinet', 'generic'))
        assert generic.returncode == 0, generic.stderr
        assert struct.unpack_from('<10I', (root / 'ram.bin').read_bytes(), 0x100) == expected
        csv.write_text(csv.read_text().replace('p2down', 'p2pedal'))
        dedicated = run(extra=('--metro-cabinet', 'dedicated'))
        assert dedicated.returncode == 0, dedicated.stderr
        assert struct.unpack_from('<10I', (root / 'ram.bin').read_bytes(), 0x100) == (*expected[:6], 0xcb, 0xfd, 0xef, expected[9])
        assert 'DSW=fe' in dedicated.stderr
        for name in ['p1button3', 'p1pedal', 'p3enter', 'coin3']:
            csv.write_text(f'frame,control,pressed\n0,cabinet_generic,1\n0,{name},1\n')
            bad = run()
            assert bad.returncode != 0, (name, bad.stdout, bad.stderr)
        for change in [ini + 'format=pcsx-aow-metro-zn-v1\n',
                       ini.replace('mame_set=bam2\n', ''), ini + 'h8_firmware=fake.bin\n']:
            (root / 'board.ini').write_text(change)
            bad = run()
            assert bad.returncode != 0 and 'invalid or unsupported Sony ZN profile' in bad.stderr, bad.stderr
        print(output, end='')
        print('Metro native MIPS ROM banks, provisional status, source cabinet/DMA and strict media/config rejection PASS')


if __name__ == '__main__':
    main()
