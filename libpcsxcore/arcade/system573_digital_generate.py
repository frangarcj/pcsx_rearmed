#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Extract pinned MAME digital I/O/FPGA methods and register dispatch."""
import argparse
import hashlib
from pathlib import Path
import re

PIN = 'deb2f54fe7300e7a192690318beb860159bfd52a'
HASHES = {
    'k573fpga.cpp': '7f7c3b05578e343b4e94cb8e610b99c8928afc26310850cca78210822b3ca631',
    'k573fpga.h': 'fa10aaa0ec27ee18e9b650e1fa7285d8f07c817c45706c76993848f5e5244574',
    'k573dio.cpp': '2ecb9b41507aef5b552d1268b780a0999bed64009fcbfe64dc5457de7b98b9a5',
    'k573dio.h': '406a5fc01422a7e952ae313773f7490e1bcde889e3fa298e85a11cf1731390f8',
}


def generate(source):
    files = {}
    for name, sha in HASHES.items():
        raw = (source / 'src/mame/konami' / name).read_bytes()
        if hashlib.sha256(raw).hexdigest() != sha:
            raise ValueError(f'{name} differs from reviewed MAME {PIN}')
        files[name] = raw.decode()
    h = files['k573fpga.h']
    methods = h[h.index('\tvoid mpeg_frame_sync'):h.index('\nprotected:')]
    fields = h[h.index('\tenum {'):h.index('\n};')]
    fields = fields.replace('required_shared_ptr<uint16_t> ram;', 'uint16_t *ram;')
    fields = fields.replace('required_device<mas3507d_device> mas3507d;', 'MasProxy *mas3507d;')
    fpga = ('struct DigitalFpga {\n' + methods + '\nvoid device_reset();\nvoid update_stream();\n'
            'void update_counter();\nvoid update_mp3_decode_state();\n'
            'uint16_t decrypt_common(uint16_t,uint16_t);\n'
            'uint16_t decrypt_default(uint16_t);\nuint16_t decrypt_ddrsbm(uint16_t);\n'
            'DigitalMachine *clock;\nDigitalMachine &machine() { return *clock; }\n'
            'DigitalTimer *m_stream_timer;\nunsigned *faults;\n'
            'uint64_t words_decrypted=0, bytes_sent=0, stream_hash=0xcbf29ce484222325ull;\n'
            'bool read_ram(uint32_t index,uint16_t &out) {\n'
            'if(index>=0x1000000) { *faults|=2; return false; }\n'
            'out=ram[index]; return true; }\n' + fields + '\n};\n')
    body = files['k573fpga.cpp']
    body = body[body.index('void k573fpga_device::device_reset()'):body.index('DEFINE_DEVICE_TYPE(', body.index('void k573fpga_device::device_reset()'))]
    body = body.replace('TIMER_CALLBACK_MEMBER(k573fpga_device::update_stream)', 'void k573fpga_device::update_stream()')
    body = body.replace('uint16_t src = ram[mp3_cur_addr >> 1];',
                        'uint16_t src; if (!read_ram(mp3_cur_addr >> 1,src)) return;\n\t\twords_decrypted++;')
    body = body.replace('mas3507d->sid_w(mp3_data & 0xff);',
                        'mas3507d->sid_w(mp3_data & 0xff);\n\tbytes_sent++;\n'
                        '\tstream_hash = (stream_hash ^ (mp3_data & 0xff)) * 0x100000001b3ull;')
    body = body.replace('return counter_value * 44100;', 'return wrapped_counter(counter_value * 44100);')
    body = body.replace('return diff * 44100;', 'return wrapped_counter(diff * 44100);')
    body = body.replace('k573fpga_device::', 'DigitalFpga::')
    body = re.sub(r'\bLOG\s*\(.*?\);', '', body, flags=re.S)
    fpga = (fpga + body).replace('attotime', 'DigitalTime')
    h = files['k573dio.h']
    methods = h[h.index('\tuint16_t a00_r();'):h.index('\nprotected:')]
    fields = h[h.index('\tuint32_t ram_adr'):h.index('\n};')].replace('u16 crypto_key1;', 'uint16_t crypto_key1;')
    dio = ('struct DigitalDio {\n' + methods + '\nvoid device_reset();\nuint16_t *ram;\n'
           'DigitalFpga *k573fpga;\nDigitalSerial *digital_id;\nDigitalOutput output_cb;\n' + fields + '\n};\n')
    body = files['k573dio.cpp']
    reset = body[body.index('void k573dio_device::device_reset()'):body.index('ROM_START( k573dio )')]
    body = reset + body[body.index('uint16_t k573dio_device::a00_r()'):]
    body = re.sub(r'\bLOG[A-Z0-9]*\s*\(.*?\);', '', body, flags=re.S)
    dio += body.replace('k573dio_device::', 'DigitalDio::')
    mapping = files['k573dio.cpp']
    pattern = (r'^\s+map\((0x[0-9a-f]+), 0x[0-9a-f]+\)\.(r|w|rw)'
               r'\(FUNC\(k573dio_device::(\w+)\)(?:, FUNC\(k573dio_device::(\w+)\))?\);')
    rows = re.findall(pattern, mapping, re.M)
    if len(rows) != 39:
        raise ValueError(f'digital register map changed: {len(rows)} entries')
    dispatch = ''
    for operation in ['read', 'write']:
        dispatch += f'static bool digital_{operation}_reg(DigitalDio &d,unsigned offset,uint16_t &data) {{\nswitch(offset) {{\n'
        for address, mode, first, second in rows:
            if operation == 'read' and 'r' in mode:
                dispatch += f'case {address}: data=d.{first}();return true;\n'
            elif operation == 'write' and 'w' in mode:
                method = second if mode == 'rw' else first
                dispatch += f'case {address}: d.{method}(data);return true;\n'
        dispatch += 'default:return false;\n}\n}\n'
    prefix = ('// SPDX-License-Identifier: BSD-3-Clause\n// copyright-holders:smf,windyfairy\n'
              f'// Generated from MAME {PIN}; do not edit.\n')
    output = prefix + fpga + dio + dispatch
    return '\n'.join(line.rstrip() for line in output.splitlines()).rstrip() + '\n'


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mame_source', type=Path)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    path = Path(__file__).with_name('system573_digital_mame.inc')
    output = generate(args.mame_source)
    if args.check:
        if not path.is_file() or path.read_text() != output:
            parser.exit(1, 'stale digital I/O/FPGA source\n')
        print('Digital I/O/FPGA methods and register map match pinned source')
    else:
        path.write_text(output)
