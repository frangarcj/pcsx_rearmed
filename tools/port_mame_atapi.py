#!/usr/bin/env python3
"""Import pinned MAME ATAPI/SPC/MMC command bodies for the CR-589 adapter."""
from pathlib import Path
import argparse
import subprocess
import hashlib
import re

ROOT = Path(__file__).resolve().parents[1]
PIN = 'deb2f54fe7300e7a192690318beb860159bfd52a'
FILES = {'t10spc': 'machine', 't10mmc': 'machine', 'atapihle': 'bus/ata'}


def replace(source, old, new):
    if source.count(old) != 1:
        raise SystemExit('source anchor changed: ' + old[:100])
    return source.replace(old, new, 1)


def source(repo, folder, name):
    return subprocess.check_output(['git', '-C', str(repo), 'show',
                                    f'{PIN}:src/devices/{folder}/{name}']).decode()


def generate(repo):
    output = {}
    provenance = []
    for name, folder in FILES.items():
        for ext in ('h', 'cpp'):
            raw = source(repo, folder, name + '.' + ext)
            provenance.append(f'{folder}/{name}.{ext} SHA256 {hashlib.sha256(raw.encode()).hexdigest()}')
            text = raw.replace('#include "emu.h"', '#include "../atapi_mame_compat.h"')
            text = text.replace('#include "multibyte.h"', '#include "../atapi_mame_compat.h"')
            text = text.replace('#pragma once', '#pragma once\n#include "../atapi_mame_compat.h"')
            text = text.replace('#include "imagedev/cdromimg.h"', '').replace('#include "sound/cdda.h"', '')
            text = text.replace('#include "machine/atahle.h"', '#include "../ata_mame/atahle_port.h"')
            text = text.replace('#include "machine/t10spc.h"', '#include "t10spc_port.h"')
            text = text.replace('m_device->logerror(', 'atapi_source_log(m_device, ')
            text = text.replace('device.save_item(', 'save_item(')
            for base in FILES:
                text = text.replace(f'#include "{base}.h"', f'#include "{base}_port.h"')
            if name == 't10spc' and ext == 'h':
                text = replace(text, 'uint8_t command[ 32 ];', 'uint8_t command[ 32 ]{};')
            if name == 'atapihle':
                signature = 'atapi_hle_device(const machine_config &mconfig, device_type type, const char *tag, device_t *owner, uint32_t clock)'
                text = replace(text, signature, 'atapi_hle_device()')
                if ext == 'cpp':
                    text = replace(text, 'ata_hle_device_base(mconfig, type, tag, owner, clock)', 'ata_hle_device_base()')
                    a = text.index('void atapi_hle_device::set_is_ready(')
                    b = text.index('void atapi_hle_device::process_buffer()', a)
                    text = text[:a] + 'void atapi_hle_device::set_is_ready(bool state) { m_is_ready = state; }\n\n' + text[b:]
                    text = text.replace('logerror( ', 'atapi_source_log(this, ')
            output[name + '_port.' + ext] = text
    # Preserve the CR-589 command/identity methods, replacing only device setup
    # and stream-based NVRAM with the standalone adapter's explicit byte API.
    raw = source(repo, 'bus/ata', 'cr589.cpp')
    provenance.append('bus/ata/cr589.cpp SHA256 ' + hashlib.sha256(raw.encode()).hexdigest())
    a = raw.index('void matsushita_cr589_device::ExecCommand()')
    b = raw.index('// device type definition', a)
    body = raw[a:b].replace('logerror(', 'atapi_source_log(this, ')
    # Guard source buffer operations. Valid accesses keep the original offset
    # increments and 32-byte write-buffer prefix convention unchanged.
    body = replace(body, '\t\tbufferOffset = get_u24be( &command[3] );\n\t\tm_phase = SCSI_PHASE_DATAOUT;',
                   '\t\tbufferOffset = get_u24be( &command[3] );\n\t\tif (!valid_buffer_range(bufferOffset, get_u24be(&command[6]))) return;\n\t\tm_phase = SCSI_PHASE_DATAOUT;')
    body = replace(body, '\t\tbufferOffset = get_u24be( &command[3] );\n\t\tm_phase = SCSI_PHASE_DATAIN;',
                   '\t\tbufferOffset = get_u24be( &command[3] );\n\t\tif (!valid_buffer_range(bufferOffset, get_u24be(&command[6]))) return;\n\t\tm_phase = SCSI_PHASE_DATAIN;')
    body = replace(body, '\t\t\tmemcpy( &buffer[bufferOffset], data + 32, dataLength - 32 );',
                   '\t\t\tif (dataLength < 32 || !valid_buffer_range(bufferOffset, dataLength)) return;\n\t\t\tmemcpy( &buffer[bufferOffset], data + 32, dataLength - 32 );')
    body = replace(body, '\t\t\tif( memcmp( data, &buffer[identity_offset], 28 ) == 0 )',
                   '\t\t\tif (dataLength < 28) { malformed_buffer(); return; }\n\t\t\tif( memcmp( data, &buffer[identity_offset], 28 ) == 0 )')
    body = body.replace('t10mmc::ReadData( data, dataLength );', 'safe_mmc_read(data, dataLength);')
    body = body.replace('t10mmc::WriteData( data, dataLength );', 'safe_mmc_write(data, dataLength);')
    a = raw.index('void matsushita_cr589_device::device_start()')
    b = raw.index('void matsushita_cr589_device::device_reset()', a)
    start = raw[a:b]
    start = start.replace('atapi_cdrom_device::device_start();', 'atapi_hle_device::device_start();')
    start = start.replace('save_item(NAME(download));', '').replace('save_item(NAME(buffer));', '').replace('save_item(NAME(bufferOffset));', '')
    output['cr589_methods.inc'] = ('/* Source MAME CR-589 command bodies, BSD-3-Clause, smf. */\n' +
                                  'static constexpr int identity_offset = 0x3ab;\n' +
                                  'static constexpr char download_identity[] = "MATSHITA CD98Q4 DOWNLOADGS0N";\n' + body + start)
    output['PROVENANCE.txt'] = 'MAME ' + PIN + '\n' + '\n'.join(provenance) + '\n'
    return output


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source', type=Path, default=ROOT.parent / 'mamedev')
    p.add_argument('--check', action='store_true')
    args = p.parse_args()
    directory = ROOT / 'libpcsxcore/arcade/atapi_mame'
    directory.mkdir(parents=True, exist_ok=True)
    for name, content in generate(args.source).items():
        path = directory / name
        if args.check:
            if not path.exists() or path.read_text() != content:
                raise SystemExit('stale ATAPI source: ' + name)
        else:
            path.write_text(content)
