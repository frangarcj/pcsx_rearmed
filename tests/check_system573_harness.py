#!/usr/bin/env python3
"""Generated MIPS integration checks, never original System 573 game evidence."""
import argparse
import struct
import subprocess
import tempfile
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def guest():
    words = []
    def li(reg, value):
        words.extend([0x3c000000 | reg << 16 | (value >> 16),
                      0x34000000 | reg << 21 | reg << 16 | (value & 0xffff)])
    def store(value, address, width=4):
        li(8, address); li(9, value)
        words.append(({1: 0xa0000000, 2: 0xa4000000, 4: 0xac000000}[width]) | 8 << 21 | 9 << 16)
    def read(address, slot):
        li(8, address)
        words.extend([0x8c000000 | 8 << 21 | 9 << 16, 0,
                      0xac000000 | 10 << 21 | 9 << 16 | (0x100 + 4 * slot)])
    li(10, 0x80000000)
    read(0xbf000000, 0)
    store(3, 0xbf500000, 2); read(0xbf000000, 1); read(0xbf500000, 2)
    store(0xaaaa, 0xbf000aaa, 2); store(0x5555, 0xbf000554, 2); store(0x9090, 0xbf000aaa, 2)
    read(0xbf000000, 3); store(0xf0f0, 0xbf000000, 2)
    store(0xaabbccdd, 0xbf620000); store(0, 0xbf620001, 1); read(0xbf620000, 4)
    store(1, 0xbf400001, 1); read(0xbf400004, 5)
    read(0xbf400008, 6); read(0xbf480000, 7)
    store(0x53353733, 0x80000120)
    words.extend([0x1000ffff, 0])
    return struct.pack('<' + 'I' * len(words), *words)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--harness', required=True, type=Path)
    args = p.parse_args()
    with tempfile.TemporaryDirectory(prefix='pcsx-system573-fixture-') as temp:
        root = Path(temp)
        (root / 'board.ini').write_text('format=pcsx-aow-system573-flash-v1\nmame_set=hyperbbc\nmame_parent=sys573\nbios=std\n'
                                      'cpu=CXD8530CQ\ncpu_clock_hz=67737600\ngpu=CXD8561Q\ngpu_clock_hz=53693175\n'
                                      'ram_size=4194304\nvram_size=2097152\nflash_chips=8\nflash_chip_size=2097152\n'
                                      'flash_bus_width=16\ncassette=Y\ncdrom_present=false\nboard_execution_verified=false\n')
        program = guest()
        (root / 'main.bin').write_bytes(program + bytes(0x80000 - len(program)))
        (root / 'h8_response.bin').write_bytes(bytes(range(64)))
        for i in range(8):
            (root / f'flash{i}.bin').write_bytes(bytes([(i + 1) * 0x11]) * 0x200000)
        (root / 'timekeeper.bin').write_bytes(bytes(8192))
        (root / 'inputs.csv').write_text('frame,control,pressed\n0,p3button2,1\n1,p3button2,0\n')
        nv = root / 'state.bin'
        def run(selector='--system573-fixture', extra=()):
            return subprocess.run([str(args.harness), selector, str(root), '--frames', '2', '--board-test',
                                   '--inputs', str(root / 'inputs.csv'), '--dump-ram', str(root / 'ram.bin'),
                                   '--output', str(root / 'out.ppm'), '--nvram', str(nv), *extra],
                                  capture_output=True, text=True, timeout=30)
        cold = run()
        output = cold.stdout + cold.stderr
        assert cold.returncode == 0, output
        expected = (0x22112211, 0x88778877, 3, 0xadad0404, 0x00bb00dd, 0x1f091017, 0xf7ff, 0xffffffff, 0x53353733)
        assert struct.unpack_from('<9I', (root / 'ram.bin').read_bytes(), 0x100) == expected, output
        for token in ['System573 native selftest PASS', 'System573 SPU DMA fixture: PASS', 'System573 large SPU DMA clock: PASS',
                      'media=SYNTHETIC_FIXTURE H8_response=GENERATED', 'faults=0', 'consumed=2/2']:
            assert token in output, (token, output)
        snapshot = nv.read_bytes()
        assert len(snapshot) == 32 + 0x1000000 + 8192 + 132
        assert snapshot[:16] == b'S573NV1\0hyperbbc'
        assert struct.unpack_from('<II', snapshot, 16) == (len(snapshot), zlib.crc32(snapshot[32:]))
        assert snapshot[32:32 + 0x200000] == (root / 'flash0.bin').read_bytes()
        assert snapshot[32 + 0x1000000:32 + 0x1000000 + 2] == b'\xdd\xbb'
        # A legitimate serialized mutable flash differs from its original seed.
        updated = bytearray(snapshot); updated[32 + 0x200000 - 1] = 0x10
        struct.pack_into('<I', updated, 20, zlib.crc32(updated[32:]))
        nv.write_bytes(updated)
        warm = run()
        assert warm.returncode == 0 and 'NVRAM=RESTORED' in warm.stderr, warm.stderr
        assert nv.read_bytes()[32 + 0x200000 - 1] == 0x10
        # Original-game entry point must never accept generated BIOS/chips.
        original = run('--system573')
        assert original.returncode != 0 and 'original main.bin size/CRC mismatch' in original.stderr
        for image in (snapshot[:-1], b'X' + snapshot[1:], snapshot[:100] + b'X' + snapshot[101:]):
            nv.write_bytes(image); bad = run(); assert bad.returncode != 0, bad.stderr
        nv.write_bytes(snapshot)
        ini = (root / 'board.ini').read_text()
        for contents in (ini + 'bios=std\n', ini.replace('cdrom_present=false', 'cdrom_present=true'), ini + 'h8_firmware=fake.bin\n'):
            (root / 'board.ini').write_text(contents)
            bad = run(); assert bad.returncode != 0 and 'invalid or unsupported original profile' in bad.stderr
        print(output, end='')
        print('System573 native MIPS bank/Fujitsu/RTC/H8/3P/empty ATA/SPU DMA, complete checksummed flash/NVRAM restore and strict source rejection PASS')


if __name__ == '__main__':
    main()
