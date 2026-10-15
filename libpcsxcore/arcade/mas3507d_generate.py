#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Import pinned MAME MAS3507D/MP3 methods and the same minimp3 implementation."""
import argparse
import hashlib
from pathlib import Path
import re

PIN = 'deb2f54fe7300e7a192690318beb860159bfd52a'
HASHES = {
    'src/devices/sound/mas3507d.cpp': '40438658f31e1a313042fb66292aa6ef0128d3d6a978fcb0c93c7acc085272fb',
    'src/devices/sound/mas3507d.h': '63fbf65e6d5ffa163d7a543c26ac8f1ac7f51f0b309cc97832cb8c4fa2acd3be',
    'src/devices/sound/mp3_audio.cpp': '1f534cbecfd764434670f58e83868706bbdab68a6782cd561688d4d3ddeee1ae',
    'src/devices/sound/mp3_audio.h': 'd39efac2de65ad25d9abcbcc7f0351fafc8670e3cc2c7ec2c82df02b5432d579',
    '3rdparty/minimp3/minimp3.h': '5fb296a790734772b65a5514438cc06d14f24e539c13475ff2a7f737681c82c0',
}


def generate(source):
    files = {}
    for name, sha in HASHES.items():
        raw = (source / name).read_bytes()
        if hashlib.sha256(raw).hexdigest() != sha:
            raise ValueError(f'{name} differs from reviewed MAME {PIN}')
        files[Path(name).name] = raw.decode()
    prefix = ('// SPDX-License-Identifier: BSD-3-Clause\n'
              '// copyright-holders:Olivier Galibert,windyfairy\n'
              f'// Generated from MAME {PIN}; do not edit.\n')
    mp3_header = files['mp3_audio.h']
    mp3_header = mp3_header[mp3_header.index('class mp3_audio'):mp3_header.rindex('#endif')]
    mp3_header = mp3_header.replace('\tvoid register_save(device_t &host);\n', '')
    mp3_methods = files['mp3_audio.cpp']
    mp3_methods = mp3_methods[mp3_methods.index('#define MINIMP3_IMPLEMENTATION'):]
    mp3_methods = mp3_methods.replace('"minimp3/minimp3.h"', '"minimp3.h"')
    a = mp3_methods.index('void mp3_audio::register_save(')
    b = mp3_methods.index('void mp3_audio::clear()', a)
    mp3_methods = mp3_methods[:a] + mp3_methods[b:]
    header = files['mas3507d.h']
    header = header[header.index('\tint i2c_scl_r();'):header.index('\n};')]
    header = re.sub(r'\s*virtual void device_start\(\) override ATTR_COLD;', '', header)
    header = re.sub(r'\n(?:protected|private):', '', header)
    header = header.replace('virtual ', '').replace(' override ATTR_COLD', '').replace(' override', '')
    header = header.replace('devcb_write_line', 'MasPin').replace('sound_stream', 'MasStream')
    header = header.replace('MasStream_update', 'sound_stream_update')
    methods = files['mas3507d.cpp']
    methods = methods[methods.index('void mas3507d_device::device_reset()'):]
    methods = re.sub(r'\bLOG[A-Z]*\s*\(.*?\);', '', methods, flags=re.S)
    methods = methods.replace('mas3507d_device::', 'MasDevice::').replace('sound_stream', 'MasStream')
    methods = methods.replace('MasStream_update', 'sound_stream_update')
    body = prefix + mp3_header + mp3_methods + '\nstruct MasDevice {\n' + header + '\n};\n' + methods
    body = '\n'.join(line.rstrip() for line in body.splitlines()).rstrip() + '\n'
    provenance = 'MAME ' + PIN + '\n' + '\n'.join(n + ' SHA256 ' + s for n, s in HASHES.items()) + '\n'
    return {'mas_methods.inc': body, 'minimp3.h': files['minimp3.h'], 'PROVENANCE.txt': provenance}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mame_source', type=Path)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    directory = Path(__file__).with_name('mas3507d_mame')
    generated = generate(args.mame_source)
    if not args.check:
        directory.mkdir(parents=True, exist_ok=True)
    for name, content in generated.items():
        path = directory / name
        if args.check:
            if not path.is_file() or path.read_text() != content:
                parser.exit(1, 'stale MAS3507D source: ' + name + '\n')
        else:
            path.write_text(content)
    if args.check:
        print('MAS3507D, MP3 and minimp3 source pins match')
