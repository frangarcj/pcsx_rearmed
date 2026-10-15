#!/usr/bin/env python3
"""Exercise read-only RAM snapshot aliases and reject MMIO/out-of-RAM reads."""
import argparse
import csv
from pathlib import Path
import struct
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--harness', required=True, type=Path)
    parser.add_argument('--system573', required=True, type=Path,
                        help='audited original 4 MiB System 573 profile')
    args = parser.parse_args()
    binary = args.harness.resolve()
    profile = args.system573.resolve()
    with tempfile.TemporaryDirectory(prefix='pcsx-snapshot-ram-') as directory:
        root = Path(directory)
        trace, dump = root/'trace.csv', root/'ram.bin'
        base = [str(binary), '--system573', str(profile), '--frames', '120',
                '--output', str(root/'frame.ppm'), '--dump-ram', str(dump),
                '--cycle-snapshots', str(trace), '--snapshot-start', '1',
                '--snapshot-interval', '564480']
        addresses = [0x80000080, 0xa0000080, 0x00000080, 0x803ffffc]
        watch = [arg for address in addresses
                 for arg in ('--snapshot-ram', hex(address))]
        run = subprocess.run(base + watch, capture_output=True, text=True, timeout=30)
        assert run.returncode == 0, run.stderr
        rows = list(csv.DictReader(trace.open()))
        assert rows and len(rows) < 200000
        ram = dump.read_bytes()
        assert len(ram) == 0x400000
        keys = [f'ram_{address:08x}' for address in addresses]
        for row in rows:
            assert len({row[key] for key in keys[:3]}) == 1, row
        assert any(int(row[keys[0]], 16) != 0 for row in rows)
        for address, key in zip(addresses, keys):
            assert int(rows[-1][key], 16) == struct.unpack_from(
                '<I', ram, address & 0x1fffffff)[0], key
        invalid = ['0x1f801070', '0x803493f9', '0xc0349538', '0x80400000',
                   '-4', '', '0x100000080']
        for address in invalid:
            run = subprocess.run(base + ['--snapshot-ram', address],
                                 capture_output=True, text=True, timeout=30)
            assert run.returncode == 2 and '--snapshot-ram' in run.stderr, (
                address, run.returncode, run.stderr)
        run = subprocess.run(base + ['--snapshot-ram', '0x80000080'] * 17,
                             capture_output=True, text=True, timeout=30)
        assert run.returncode == 2 and '--snapshot-ram' in run.stderr
    print('Original 4 MiB RAM observation, KSEG aliases, dump agreement and address guards PASS')


if __name__ == '__main__':
    main()
