#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Extract the pinned MAME ZS01 pin protocol, without the device framework."""
import argparse
import hashlib
from pathlib import Path
import re

PIN = 'deb2f54fe7300e7a192690318beb860159bfd52a'
HASHES = {
    'zs01.cpp': 'd42c96be99fcd5ffd2368c261231236a2290bd3f5023dd28c81922779fe5ca17',
    'zs01.h': '05c6004a84a0ce22c5fdda2b6e5d8efa4a7cace285131f136ee2bb842d3241c7',
}


def generate(source):
    files = {}
    for name, digest in HASHES.items():
        data = (source / 'src/mame/konami' / name).read_bytes()
        if hashlib.sha256(data).hexdigest() != digest:
            raise ValueError(f'{name} differs from reviewed MAME {PIN}')
        files[name] = data.decode()
    header = files['zs01.h']
    enums = header[header.index('\tenum size_t'):header.index('\t// internal state')]
    members = header[header.index('\tint m_cs;'):header.index('\n};')]
    methods = files['zs01.cpp']
    methods = methods[methods.index('void zs01_device::device_reset()'):
                      methods.index('void zs01_device::nvram_default()')]
    methods = re.sub(r'\b(?:LOGSIGNALS|LOG|logerror)\s*\(.*?\);', '', methods, flags=re.S)
    methods = methods.replace('zs01_device::', 'ZS01::')
    methods = methods.replace('m_ds2401->direct_read(', 'serial_byte(')
    declarations = '''struct ZS01 {
    uint8_t serial[8];
    uint8_t serial_byte(unsigned index) { return serial[index]; }
    void device_reset();
    void write_cs(int);
    void write_rst(int);
    void write_scl(int);
    void write_sda(int);
    int read_sda();
    void decrypt(uint8_t *, uint8_t *, int, uint8_t *, uint8_t);
    void decrypt2(uint8_t *, uint8_t *, int, uint8_t *, uint8_t);
    void encrypt(uint8_t *, uint8_t *, int, uint8_t *, uint32_t);
    uint16_t calc_crc(uint8_t *, uint32_t);
    int data_offset();
'''
    output = ('// SPDX-License-Identifier: BSD-3-Clause\n// copyright-holders:smf\n'
              f'// Generated from MAME {PIN}; do not edit.\n' +
              declarations + enums + members + '\n};\n' + methods)
    return '\n'.join(line.rstrip() for line in output.splitlines()).rstrip() + '\n'


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mame_source', type=Path)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    output = Path(__file__).with_name('konami_zs01_mame.inc')
    generated = generate(args.mame_source)
    if args.check:
        if not output.is_file() or output.read_text() != generated:
            parser.exit(1, 'generated ZS01 source differs\n')
        print('ZS01 source pin and generated protocol match')
    else:
        output.write_text(generated)
