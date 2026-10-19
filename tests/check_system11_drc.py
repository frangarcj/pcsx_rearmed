#!/usr/bin/env python3
"""Instruction-driven System 11 DRC checks; generated media, no game ROMs.

Exercises both GPU models, ROM8/ROM8_64/unbanked layouts, bank restoration,
cross-window delay slots, RAM SMC and bidirectional C76 shared memory.
One temporary profile at a time, under 40 MiB of disk input.
"""
import argparse
import json
import re
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile

import check_system11_harness as s


def make_media(directory, name):
    profiles = json.loads((s.ROOT / 'scripts/system11_boards.json').read_text())['profiles']
    profile = next(row for row in profiles if row['set'] == name)
    layout = profile['banking']
    s.create_fixture(directory, profile['model'])
    fields = {'format': 'pcsx-aow-system11-v1', 'mame_set': name,
              'mame_parent': profile.get('parent') or 'none', 'board_model': profile['model'],
              'banking': layout, 'keycus': profile['keycus'], 'lightgun': 'false',
              'mame_machine': profile['machine'], 'mame_input': profile['input'],
              'mame_flags': str(profile['flags']), 'board_execution_verified': 'false'}
    (directory / 'board.ini').write_text(''.join(f'{k}={v}\n' for k, v in fields.items()))
    banked = bytearray(0 if layout == 'NONE' else (0x2000000 if layout == 'ROM8_64' else 0x1000000))
    for bank in range(len(banked) // 0x100000):
        struct.pack_into('<4I', banked, bank * 0x100000 + 0x100,
                         s._addiu(17, 0, bank + 1), 0x03e00008, 0, 0)
        struct.pack_into('<2I', banked, bank * 0x100000 + 0xffff8,
                         s._addiu(17, 0, 0x70 + bank), 0x03e00008)
    (directory / 'banked.bin').write_bytes(banked)
    code = [s._lui(16, 0x8030), s._lui(8, 0xbfa1)]

    def select(window, bank):
        selector = ((bank & 12) << 4) | (bank & 3) if layout == 'ROM8' else ((bank & 24) << 3) | (bank & 7)
        code.extend([s._addiu(9, 0, selector), s._sh(9, 8, 0x20 + window * 2)])

    def call(address, slot):
        code.extend([s._lui(10, address >> 16), s._ori(10, 10, address & 0xffff),
                     s._jalr(10), 0, s._sw(17, 16, slot)])

    if layout != 'NONE':
        for window in range(8):
            address = (0x9f000100 if window & 1 else 0xbf000100) + window * 0x100000
            for step, bank in enumerate((window, window + 8, window)):
                select(window, bank); call(address, window * 16 + step * 4)
        # Cached and uncached keys for the same banked window.
        select(0, 1); call(0x9f000100, 0xa0)
        select(0, 0); call(0x9f000100, 0xa4)
        if layout == 'ROM8_64':
            # The upper latch alone does not change the selected bank.
            code += [s._lui(11, 0xbf08), s._sh(0, 11, 2)]
            call(0xbf000100, 0xb0)
            select(0, 0); call(0xbf000100, 0xb4)
            code += [s._sh(0, 11, 0)]
        select(0, 0); call(0xbf0ffff8, 0xb8)
        select(0, 1); call(0xbf0ffff8, 0xbc)

    # All MMIO widths must reach the actual C76 share, not a direct shortcut.
    code += [s._lui(13, 0xbfa0), s._ori(13, 13, 0x4100),
             s._lui(11, 0x1122), s._ori(11, 11, 0x3344), s._sw(11, 13, 0),
             s._lw(12, 13, 0), 0, s._sw(12, 16, 0x80),
             s._lhu(12, 13, 2), 0, s._sw(12, 16, 0x84),
             s._i(0x24, 13, 12, 0), 0, s._sw(12, 16, 0x88),
             s._lui(10, 0xbff0), s._lw(12, 10, 0), 0, s._sw(12, 16, 0x8c),
             s._lui(11, 0x51a7), s._ori(11, 11, 0xface), s._sw(11, 16, 0x100),
             s._lui(10, 0x8010), s._lw(12, 10, 0x100), 0, s._sw(12, 16, 0x104)]
    # A function in upper RAM is rewritten after its first compiled execution.
    code += [s._lui(14, 0x8030), s._ori(14, 14, 0x200)]
    for offset, word in enumerate((s._addiu(17, 0, 0x31), 0x03e00008, 0)):
        code += [s._lui(11, word >> 16), s._ori(11, 11, word & 0xffff), s._sw(11, 14, offset * 4)]
    code += [s._jalr(14), 0, s._sw(17, 16, 0x90)]
    word = s._addiu(17, 0, 0x42)
    code += [s._lui(11, word >> 16), s._ori(11, 11, word & 0xffff), s._sw(11, 14, 0),
             s._jalr(14), 0, s._sw(17, 16, 0x94)]
    poll = 0xbfc00000 + len(code) * 4
    code += [s._lhu(12, 13, 0x10), 0, s._sw(12, 16, 0x98), s._j(poll), 0]
    rom = bytearray(0x400000)
    struct.pack_into(f'<{len(code)}I', rom, 0, *code)
    struct.pack_into('<I', rom, 0x300000, 0xd15c0de0)
    (directory / 'main.bin').write_bytes(rom)
    # Let the real M37710 program echo a PSX-written word across the share.
    bios = bytearray(s.make_c76_bios())
    branch = bios.rfind(b'\x80', 0, 0x100)
    target = branch + 2 + struct.unpack('b', bios[branch + 1:branch + 2])[0]
    extra = b'\xad\x00\x41\x8d\x10\x41'  # LDA $4100; STA $4110
    displacement = target - (branch + len(extra) + 2)
    assert -128 <= displacement < 0
    bios[branch:branch + len(extra) + 2] = extra + bytes((0x80, displacement & 0xff))
    (directory / 'c76bios.bin').write_bytes(bios)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--core', type=Path, required=True)
    parser.add_argument('--host', type=Path, required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='system11-drc-') as temp:
        for name in ('tekken', 'tekken2', 'myangel3', 'starswep'):
            profile = Path(temp) / name
            make_media(profile, name)
            for cpu in ('interpreter', 'ari64'):
                observations = []
                for threaded in (False, True):
                    command = [str(args.host.resolve()), str(args.core.resolve()),
                               str(profile), name, cpu] + (['threaded'] if threaded else [])
                    result = subprocess.run(command, check=True, timeout=90,
                                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
                    print(result.stdout, end='')
                    observations.append(re.findall(r'RESULT .*', result.stdout))
                assert len(observations[0]) == 2 and observations[0] == observations[1], observations
                print(f'{name} {cpu}: serial/threaded audio frame counts and hashes identical')
            shutil.rmtree(profile)


if __name__ == '__main__':
    main()
