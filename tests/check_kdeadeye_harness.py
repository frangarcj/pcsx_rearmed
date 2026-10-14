#!/usr/bin/env python3
"""Exercise Dead Eye MMIO through a generated MIPS guest, not game firmware."""
import argparse
import hashlib
import struct
import subprocess
import tempfile
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--harness', type=Path, required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='pcsx-deadeye-') as temp:
        root = Path(temp)
        (root / 'disc.bin').write_bytes(bytes((i * 7 + 3) & 255 for i in range(8 * 2048)))
        (root / 'disc.cue').write_text('FILE "disc.bin" BINARY\n'
                                     '  TRACK 01 MODE1/2048\n    INDEX 01 00:00:00\n')
        subprocess.run(['chdman', 'createcd', '-i', str(root / 'disc.cue'),
                        '-o', str(root / 'disc.chd'), '-c', 'none'], check=True, capture_output=True)
        disc_hash = hashlib.sha256((root / 'disc.chd').read_bytes()).digest()
        csv = root / 'inputs.csv'
        csv.write_text('frame,control,pressed\n0,p1gunx,76\n0,p1guny,0\n'
                       '0,p2gunx,443\n0,p2guny,239\n0,p1button1,1\n'
                       '0,coin1,1\n0,p1start,1\n0,test,1\n'
                       '0,coin2,1\n0,p2start,1\n'
                       '1,p1button1,0\n1,coin1,0\n1,p1start,0\n1,test,0\n'
                       '1,coin2,0\n1,p2start,0\n')

        def run(inputs=csv):
            return subprocess.run([str(args.harness), '--system-gv-kdeadeye-fixture',
                                   str(root / 'disc.chd'), '--frames', '2', '--inputs', str(inputs),
                                   '--dump-ram', str(root / 'ram.bin'), '--output', str(root / 'out.ppm')],
                                  capture_output=True, text=True, timeout=30)

        first = run()
        output = first.stdout + first.stderr
        assert first.returncode == 0, output
        words = struct.unpack_from('<11I', (root / 'ram.bin').read_bytes(), 0x100)
        assert words[:5] == (0x4c, 0, 0x1bb, 0xef, 0xfffe), (words, output)
        # Bit 13 of P1 is the original EEPROM DO pin, independently of buttons.
        assert words[5] | 0x2000 == 0xffffe9ff, (words, output)
        assert words[6:] == (0xfffff9ff, 0xffffffff, 0x00ed00b0, 0x1234, 0x44454144), (words, output)
        assert 'faults=00000000/00000000' in output and 'buttons=ffff faults=0 warm=0' in output, output
        state = root / 'kdeadeye.fixture.flash.nvram'
        saved = state.read_bytes()
        assert len(saved) == 64 + 0x80000 and saved[:16] == b'PCSX-DEAD-TEST01'
        assert hashlib.sha1(saved[64:]).digest() == saved[36:56]
        assert saved[64 + 0x100:64 + 0x102] == b'\x12\x34'
        second = run()
        output2 = second.stdout + second.stderr
        assert second.returncode == 0 and 'warm=1' in output2, output2
        assert state.read_bytes() == saved
        assert hashlib.sha256((root / 'disc.chd').read_bytes()).digest() == disc_hash
        for name, value in [('p1left', 1), ('p1button2', 1), ('p3start', 1),
                            ('p3gunx', 1), ('p1gunx', 65536)]:
            csv.write_text(f'frame,control,pressed\n0,{name},{value}\n')
            rejected = run()
            assert rejected.returncode != 0, (name, rejected.stdout, rejected.stderr)
            assert state.read_bytes() == saved
        for mutated in [saved[:64], saved[:16] + bytes([saved[16] ^ 1]) + saved[17:],
                        saved[:-1] + bytes([saved[-1] ^ 1])]:
            state.write_bytes(mutated)
            bad = run()
            assert bad.returncode and 'invalid or wrong-profile flash.nvram' in bad.stderr, bad.stderr
            assert state.read_bytes() == mutated
        print(output, end='')
        print(output2, end='')
        print('Dead Eye native MIPS gun/trigger/unused ports, Sharp commands, warm flash and invalid input/state rejection PASS')


if __name__ == '__main__':
    main()
