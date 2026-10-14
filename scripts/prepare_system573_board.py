#!/usr/bin/env python3
"""Audit the first System 573 flash representative separately from execution evidence."""
from __future__ import annotations
import argparse
from dataclasses import replace
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import prepare_mame_psx as mame
import prepare_taito_fx1_board as assembly
import capcom_zn_boards as zn

FLASH_TAGS = tuple('29f016a.' + socket for socket in
                   ('31m', '27m', '31l', '27l', '31j', '27j', '31h', '27h'))
FILES = {'maincpu:rom': 'main.bin', 'h8_response': 'h8_response.bin',
         **{tag: f'flash{i}.bin' for i, tag in enumerate(FLASH_TAGS)},
         'm48t58': 'timekeeper.bin'}


def source_profile(source: Path, name: str, bios: str = 'std'):
    if name != 'hyperbbc' or bios != 'std':
        raise mame.PreparationError('only Hyper Bishi Bashi Champ EAA with standard BIOS is reviewed')
    commit = subprocess.run(['git', '-C', str(source), 'rev-parse', 'HEAD'],
                            check=True, capture_output=True, text=True).stdout.strip()
    if commit != mame.PINNED_MAME_COMMIT:
        raise mame.PreparationError('MAME checkout differs from pinned source')
    driver = 'src/mame/konami/ksys573.cpp'
    if subprocess.run(['git', '-C', str(source), 'diff', '--quiet', 'HEAD', '--', driver]).returncode:
        raise mame.PreparationError('System 573 source differs from pinned commit')
    text = (source / driver).read_text()
    game = zn._game_rows(text)[name]
    if (game['parent'], game['machine'], game['input'], game['state'], game['init'],
            game['rotation'], game['flags']) != (
            'sys573', 'hyperbbc', 'hyperbbc', 'ksys573_state', 'init_hyperbbc',
            'ROT0', 'MACHINE_IMPERFECT_SOUND'):
        raise mame.PreparationError('unreviewed System 573 game declaration')
    body = zn._function_body(text, r'void\s+ksys573_state::{name}\s*\(\s*machine_config\s*&config\s*\)', name)
    if not all(s in body for s in ('konami573(config, true)', 'cassy(config)', 'hyperbbc_cassette_install(device)')):
        raise mame.PreparationError('System 573 flash/cassette configuration changed')
    romset, parents, _ = mame._find_romset(source, name)
    expected = {'maincpu:rom': 0x80000, 'h8_response': 0x40, 'mcu': 0x8000,
                **{tag: 0x200000 for tag in FLASH_TAGS}, 'm48t58': 0x2000}
    if {tag: region.size for tag, region in romset.regions.items()} != expected or romset.disks:
        raise mame.PreparationError('unexpected System 573 flash ROM geometry/media')
    omitted = [f for f in romset.files if f.status == 'no_dump']
    if len(omitted) != 1 or (omitted[0].filename, omitted[0].expected_size) != ('hd6473644h.18e', 0x8000):
        raise mame.PreparationError('unexpected undumped System 573 firmware')
    index = omitted[0].index
    ops = romset.regions['mcu'].operations
    if len(ops) != 1 or not isinstance(ops[0], mame.LoadOp) or ops[0].file_index != index:
        raise mame.PreparationError('unexpected H8 firmware declaration')
    romset = replace(romset, regions={k: v for k, v in romset.regions.items() if k != 'mcu'},
                     files=[f for f in romset.files if f.index != index],
                     loads=[op for op in romset.loads if op.file_index != index])
    selected = mame._selected_bios(romset, bios)
    return romset, parents, selected


def prepare(name: str, source: Path, rompath: Path, output: Path, bios: str = 'std'):
    romset, parents, selected = source_profile(source, name, bios)
    regions, origins = assembly.assemble_verified(romset, rompath, parents, selected)
    payloads = {FILES[tag]: data for tag, data in regions.items()}
    if len(origins) != 11 or any(f.status != 'good' for f in romset.files):
        raise mame.PreparationError('expected eleven source-authenticated dumped chips')
    config = {'format': 'pcsx-aow-system573-flash-v1', 'mame_set': name,
              'mame_parent': 'sys573', 'bios': selected, 'cpu': 'CXD8530CQ',
              'cpu_clock_hz': 67737600, 'gpu': 'CXD8561Q', 'gpu_clock_hz': 53693175,
              'ram_size': 4194304, 'vram_size': 2097152, 'flash_chips': 8,
              'flash_chip_size': 2097152, 'flash_bus_width': 16,
              'cassette': 'Y', 'cdrom_present': 'false', 'board_execution_verified': 'false'}
    specs = {f.index: f for f in romset.files}
    manifest = {'format': config['format'], 'profile': config,
                'mame_source_commit': mame.PINNED_MAME_COMMIT, 'mame_driver': romset.source_driver,
                'mame_flags': 'MACHINE_IMPERFECT_SOUND', 'board_execution_verified': False,
                'h8_firmware_available': False, 'h8_response_source_dump': True,
                'omitted_media': [{'region': 'mcu', 'filename': 'hd6473644h.18e',
                                   'size': 0x8000, 'status': 'NO_DUMP',
                                   'reason': 'No firmware substituted; source uses dumped H8 response bytes'}],
                'cassette_eeprom_dump_present': False,
                'known_limitations': ['Native board/gameplay evidence is separate from media preparation',
                                      'Hyperbbc source declares cassette Y but no EEPROM dump; its program does not check this chip',
                                      'This flash-only representative does not validate analog/digital daughterboards or CD-ROM'],
                'source_files': {specs[i].filename: {'origin': origin, 'size': specs[i].expected_size,
                                                  'crc32': specs[i].crc32, 'sha1': specs[i].sha1}
                                 for i, origin in origins.items()},
                'regions': {n: {'size': len(data), 'sha1': hashlib.sha1(data).hexdigest()}
                            for n, data in payloads.items()}}
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='prepare-system573-', dir=output.parent) as temp:
        stage = Path(temp)
        for n, data in payloads.items():
            (stage / n).write_bytes(data)
        (stage / 'board.ini').write_text(''.join(f'{k}={v}\n' for k, v in config.items()))
        (stage / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
        for n in (*payloads, 'board.ini', 'manifest.json'):
            os.replace(stage / n, output / n)
    return manifest


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--set', required=True, dest='name')
    parser.add_argument('--mame-source', required=True, type=Path)
    parser.add_argument('--rompath', required=True, type=Path)
    parser.add_argument('--output-dir', required=True, type=Path)
    parser.add_argument('--bios', default='std')
    args = parser.parse_args()
    try:
        report = prepare(args.name, args.mame_source, args.rompath, args.output_dir, args.bios)
        print(f"System 573 {args.name}: eleven verified chips, eight flash seeds and original timekeeper; media audit only")
    except (mame.PreparationError, assembly.TaitoFX1PreparationError) as exc:
        parser.exit(1, str(exc) + '\n')
