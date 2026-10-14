#!/usr/bin/env python3
"""Verify the original DDR 2nd Mix cassette/CD, retaining source BAD_DUMP status."""
from dataclasses import replace
from pathlib import Path
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import tempfile
import re
import prepare_mame_psx as mame
import prepare_taito_fx1_board as assembly
import capcom_zn_boards as zn

DISK_SHA = 'cfe3a6f3ed62ba388b07045e29e22472d17dcfe4'
FILES = {'maincpu:rom': 'main.bin', 'h8_response': 'h8_response.bin',
         'cassette:game:eeprom': 'cassette.bin'}


def source_profile(source, name='ddr2m', bios='std'):
    if name != 'ddr2m' or bios != 'std':
        raise mame.PreparationError('only DDR 2nd Mix JAA with standard BIOS is reviewed')
    commit = subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip()
    if commit != mame.PINNED_MAME_COMMIT:
        raise mame.PreparationError('MAME source pin mismatch')
    driver = 'src/mame/konami/ksys573.cpp'
    if subprocess.run(['git', '-C', str(source), 'diff', '--quiet', 'HEAD', '--', driver]).returncode:
        raise mame.PreparationError('System 573 driver differs from pin')
    text = (source / driver).read_text()
    game = zn._game_rows(text)[name]
    if (game['parent'], game['machine'], game['input'], game['state'], game['init'], game['flags']) != (
            'sys573', 'ddr', 'ddr', 'ddr_state', 'init_ddr', 'MACHINE_IMPERFECT_SOUND'):
        raise mame.PreparationError('DDR source registration changed')
    body = zn._function_body(text, r'void\s+ddr_state::{name}\s*\(machine_config\s*&config\)', 'ddr')
    if 'k573a(config)' not in body or 'cassx(config)' not in body:
        raise mame.PreparationError('DDR analog/cassette composition changed')
    romset, parents, _ = mame._find_romset(source, name)
    expected = {'maincpu:rom': 0x80000, 'h8_response': 0x40, 'mcu': 0x8000, 'cassette:game:eeprom': 548}
    if {k: r.size for k, r in romset.regions.items()} != expected:
        raise mame.PreparationError('unexpected DDR source region geometry')
    if len(romset.disks) != 1 or (romset.disks[0].name, romset.disks[0].region,
            romset.disks[0].sha1, romset.disks[0].readonly) != ('895jaa02', 'runtime', DISK_SHA, True):
        raise mame.PreparationError('unexpected DDR source CD declaration')
    if 'DISK_IMAGE_READONLY( "895jaa02", 0, BAD_DUMP SHA1(' + DISK_SHA + ') )' not in text:
        raise mame.PreparationError('source DDR CD BAD_DUMP annotation changed')
    omitted = [f for f in romset.files if f.status == 'no_dump']
    if len(omitted) != 1 or (omitted[0].filename, omitted[0].expected_size) != ('hd6473644h.18e', 0x8000):
        raise mame.PreparationError('unexpected DDR undumped firmware')
    idx = omitted[0].index
    romset = replace(romset, regions={k: r for k, r in romset.regions.items() if k != 'mcu'},
                     files=[f for f in romset.files if f.index != idx],
                     loads=[op for op in romset.loads if op.file_index != idx])
    cassette = [f for f in romset.files if f.filename == 'gn895jaa.u1']
    if len(cassette) != 1 or cassette[0].status != 'bad_dump':
        raise mame.PreparationError('source cassette BAD_DUMP annotation changed')
    return romset, parents, mame._selected_bios(romset, bios)


def prepare(source, rompath, output, name='ddr2m', bios='std'):
    romset, parents, selected = source_profile(source, name, bios)
    regions, origins = assembly.assemble_verified(romset, rompath, parents, selected)
    chd = rompath / name / '895jaa02.chd'
    if not chd.is_file():
        raise mame.PreparationError('original DDR CD is missing')
    tool = shutil.which('chdman')
    if not tool:
        raise mame.PreparationError('chdman required for full CD verification')
    info = subprocess.run([tool, 'info', '-i', str(chd), '-v'], check=True, capture_output=True, text=True).stdout
    sha = re.search(r'^SHA1:\s*(\S+)', info, re.M)
    if not sha or sha[1] != DISK_SHA or "Tag='CHTR'" not in info and "Tag='CHT2'" not in info:
        raise mame.PreparationError('DDR source CD hash/metadata mismatch')
    verified = subprocess.run([tool, 'verify', '-i', str(chd)], check=True, capture_output=True, text=True).stdout
    payloads = {FILES[tag]: data for tag, data in regions.items()}
    if len(origins) != 3:
        raise mame.PreparationError('expected BIOS, H8 response and original cassette')
    config = {'format': 'pcsx-aow-system573-cd-v1', 'mame_set': name, 'mame_parent': 'sys573',
              'bios': selected, 'cpu': 'CXD8530CQ', 'cpu_clock_hz': 67737600,
              'gpu': 'CXD8561Q', 'gpu_clock_hz': 53693175, 'ram_size': 4194304,
              'vram_size': 2097152, 'flash_chips': 8, 'flash_chip_size': 2097152,
              'flash_bus_width': 16, 'cassette': 'X', 'cdrom_present': 'true',
              'board_execution_verified': 'false'}
    specs = {f.index: f for f in romset.files}
    manifest = {'format': config['format'], 'profile': config,
                'mame_source_commit': mame.PINNED_MAME_COMMIT, 'board_execution_verified': False,
                'original_media_status': {'cassette': 'BAD_DUMP', 'CD': 'BAD_DUMP'},
                'h8_firmware_available': False, 'h8_response_source_dump': True,
                'omitted_media': [{'region': 'mcu', 'filename': 'hd6473644h.18e', 'status': 'NO_DUMP'}],
                'source_files': {specs[i].filename: {'origin': origin, 'sha1': specs[i].sha1,
                    'crc32': specs[i].crc32, 'status': specs[i].status} for i, origin in origins.items()},
                'regions': {n: {'size': len(data), 'sha1': hashlib.sha1(data).hexdigest()} for n, data in payloads.items()},
                'disk': {'name': '895jaa02.chd', 'sha1': sha[1], 'full_data_metadata_verified': True,
                         'source_status': 'BAD_DUMP', 'info': info, 'verification': verified},
                'source_defaults': {'onboard_flash': 'eight erased Fujitsu 29F016A chips, no source dump',
                                    'timekeeper': 'M48T58 source default FF SRAM with live RTC counters'},
                'known_limitations': ['Native cassette/CD/analog/game evidence is separate from this media audit',
                                      'BAD_DUMP hashes match the source but do not establish perfect physical dumps']}
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='prepare-system573-cd-', dir=output.parent) as tmp:
        stage = Path(tmp)
        for n, data in payloads.items():
            (stage / n).write_bytes(data)
        shutil.copyfile(chd, stage / chd.name)
        (stage / 'board.ini').write_text(''.join(f'{k}={v}\n' for k, v in config.items()))
        (stage / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
        for n in (*payloads, chd.name, 'board.ini', 'manifest.json'):
            os.replace(stage / n, output / n)
    return manifest


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--set', default='ddr2m', dest='name')
    p.add_argument('--bios', default='std')
    p.add_argument('--mame-source', type=Path, required=True)
    p.add_argument('--rompath', type=Path, required=True)
    p.add_argument('--output-dir', type=Path, required=True)
    a = p.parse_args()
    try:
        prepare(a.mame_source, a.rompath, a.output_dir, a.name, a.bios)
        print('DDR 2nd Mix: original BIOS/H8 response/cassette/CD verified; BAD_DUMP retained; media audit only')
    except (mame.PreparationError, assembly.TaitoFX1PreparationError, subprocess.CalledProcessError) as exc:
        p.exit(1, str(exc) + '\n')
