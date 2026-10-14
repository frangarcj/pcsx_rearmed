#!/usr/bin/env python3
"""Verify Metro Dance Tengoku Mix ROM/HDD/CD media; never invent H8 firmware."""
from __future__ import annotations
import argparse
from dataclasses import replace
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import prepare_mame_psx as mame
import capcom_zn_boards as zn
import prepare_taito_fx1_board as assembly
from prepare_tecmo_tps_board import KEYS

SETS = ('bam2', 'bam2a')
TAGS = ('maincpu:rom', 'bankedroms', 'cat702_1', 'cat702_2')
FILES = ('main.bin', 'banked.bin', 'cat702_key0.bin', 'cat702_key1.bin')
DISKS = {'bam2': ('bam2', 'ata:0:hdd', '634d9a745a82c567fc4d7ce48e3570d88326c5f9'),
         'bam2a': ('bam2cd', 'ata:0:cdrom', 'cdedc8f9eb67bed267c899d679f986bf1748bd00')}
OMITTED = {'upd78081': ('78081g503.ic655', 0x2000), 'h83644': ('hd64f3644.u2', 0x8000)}


def source_profile(source: Path, name: str):
    if name not in SETS:
        raise mame.PreparationError('only bam2/bam2a Metro ZN configurations are reviewed')
    commit = subprocess.run(['git', '-C', str(source), 'rev-parse', 'HEAD'],
                            check=True, capture_output=True, text=True).stdout.strip()
    if commit != mame.PINNED_MAME_COMMIT:
        raise mame.PreparationError('MAME checkout differs from pinned source')
    if subprocess.run(['git', '-C', str(source), 'diff', '--quiet', 'HEAD', '--',
                       'src/mame/sony/zn.cpp']).returncode:
        raise mame.PreparationError('Sony ZN source differs from pinned commit')
    text = (source / 'src/mame/sony/zn.cpp').read_text()
    game = zn._game_rows(text)[name]
    machine = 'bam2hdd' if name == 'bam2' else 'bam2cdrom'
    if (game['machine'], game['input'], game['state'], game['rotation']) != (machine, 'bam2', 'bam2_state', 'ROT0'):
        raise mame.PreparationError('Metro source machine/input/state changed')
    if game['flags'] != 'MACHINE_SUPPORTS_SAVE | MACHINE_IMPERFECT_SOUND | MACHINE_NOT_WORKING':
        raise mame.PreparationError('Metro source status changed; requires new review')
    body = zn._function_body(text, r'void\s+{name}\s*\(\s*machine_config\s*&config\s*\)', 'bam2')
    if not all(s in body for s in ('zn1_2mb_vram(config)', 'cat702<0>(config)',
                                  'cat702<1>(config)', 'ATA_INTERFACE(config, m_ata)')):
        raise mame.PreparationError('Metro board composition changed')
    variant = zn._function_body(text, r'void\s+{name}\s*\(\s*machine_config\s*&config\s*\)', machine)
    device = 'hdd' if name == 'bam2' else 'cdrom'
    if 'bam2(config)' not in variant or f'set_default_option("{device}")' not in variant:
        raise mame.PreparationError('Metro source drive variant changed')
    romset, parents, _ = mame._find_romset(source, name)
    if set(romset.regions) != set(TAGS) | set(OMITTED) or len(romset.disks) != 1 or romset.bios:
        raise mame.PreparationError('unexpected Metro ROM/media declarations')
    disk = romset.disks[0]
    if (disk.name, disk.region, disk.sha1, disk.index, disk.readonly) != (*DISKS[name], 0, False):
        raise mame.PreparationError('Metro disk declaration changed')
    omitted_indices = set()
    for tag, (filename, size) in OMITTED.items():
        region = romset.regions[tag]
        specs = [next(f for f in romset.files if f.index == op.file_index)
                 for op in region.operations if isinstance(op, mame.LoadOp)]
        if (region.size != size or len(region.operations) != 1 or len(specs) != 1 or
                (specs[0].filename, specs[0].expected_size, specs[0].status) != (filename, size, 'no_dump')):
            raise mame.PreparationError('unreviewed Metro undumped firmware declaration')
        omitted_indices.add(specs[0].index)
    romset = replace(romset, regions={k: v for k, v in romset.regions.items() if k in TAGS},
                     files=[f for f in romset.files if f.index not in omitted_indices],
                     loads=[op for op in romset.loads if op.file_index not in omitted_indices])
    if tuple(romset.regions[t].size for t in TAGS) != (0x80000, 0x2c00000, 8, 8):
        raise mame.PreparationError('Metro source ROM geometry changed')
    for tag in TAGS:
        for op in romset.regions[tag].operations:
            if not isinstance(op, mame.LoadOp):
                raise mame.PreparationError('unsupported Metro ROM assembly operation')
            spec = next(f for f in romset.files if f.index == op.file_index)
            if op.source_offset or op.length != spec.expected_size or spec.status != 'good':
                raise mame.PreparationError('Metro requires full good ROM dumps')
    values = ('pcsx-aow-metro-zn-v1', name, game['parent'], 'coh1002e', 'fixed',
              'COH1002E', 'METRO', 'CXD8530CQ', '67737600', 'CXD8561Q', '53693175',
              '4194304', '2097152', 'false')
    return romset, parents, dict(zip(KEYS, values))


def verify_chd(path: Path, name: str):
    tool = shutil.which('chdman')
    if not tool:
        raise mame.PreparationError('chdman is required for full Metro disk verification')
    try:
        info = subprocess.run([tool, 'info', '-i', str(path), '-v'], check=True,
                              capture_output=True, text=True).stdout
        verified = subprocess.run([tool, 'verify', '-i', str(path)], check=True,
                                  capture_output=True, text=True)
    except (OSError, subprocess.CalledProcessError) as exc:
        raise mame.PreparationError('Metro CHD verification failed: ' + str(getattr(exc, 'stderr', '') or exc)) from exc
    sha = re.search(r'^SHA1:\s*(\S+)', info, re.M)
    if not sha or sha[1] != DISKS[name][2]:
        raise mame.PreparationError('Metro CHD source SHA1 mismatch')
    geometry = re.search(r'CYLS:(\d+),HEADS:(\d+),SECS:(\d+),BPS:(\d+)', info)
    if name == 'bam2':
        if not geometry or int(geometry[4]) != 512:
            raise mame.PreparationError('Metro HDD requires source hard-disk metadata and 512-byte sectors')
    elif not re.search(r"Tag='CHT[2R]'", info):
        raise mame.PreparationError('Metro CD requires source CD track metadata')
    return {'name': DISKS[name][0], 'region': DISKS[name][1], 'sha1': sha[1],
            'source_readonly_declaration': False, 'data_and_metadata_verified': True,
            'info': info, 'verification': verified.stdout.strip()}


def prepare(name: str, source: Path, rompath: Path, output: Path, archive: Path | None = None):
    romset, parents, config = source_profile(source, name)
    regions, origins = assembly.assemble_verified(romset, rompath, parents, None, archive)
    chd = rompath / name / (DISKS[name][0] + '.chd')
    if not chd.is_file():
        raise mame.PreparationError('required original Metro disk is missing: ' + str(chd))
    report = verify_chd(chd, name)
    payloads = {filename: regions[tag] for tag, filename in zip(TAGS, FILES)}
    by_index = {spec.index: spec for spec in romset.files}
    manifest = {'format': config['format'], 'mame_source_commit': mame.PINNED_MAME_COMMIT,
                'mame_driver': 'sony/zn.cpp', 'profile': config,
                'mame_flags': 'MACHINE_SUPPORTS_SAVE | MACHINE_IMPERFECT_SOUND | MACHINE_NOT_WORKING',
                'board_execution_verified': False, 'h8_firmware_available': False,
                'known_limitations': ['Pinned MCU handlers are provisional; no H8 execution or ATA transport',
                                      'H8 verifies original Quantum Fireball CR 4.3AT drive identity'],
                'omitted_media': [{'region': t, 'filename': n, 'size': s, 'status': 'NO_DUMP',
                                   'reason': 'No firmware substituted; limitation retained'}
                                  for t, (n, s) in OMITTED.items()],
                'source_files': {by_index[i].filename: o for i, o in origins.items()},
                'regions': {n: {'size': len(b), 'sha1': hashlib.sha1(b).hexdigest()} for n, b in payloads.items()},
                'disk': report}
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='prepare-metro-zn-', dir=output.parent) as temp:
        stage = Path(temp)
        for n, b in payloads.items():
            (stage / n).write_bytes(b)
        shutil.copyfile(chd, stage / chd.name)
        (stage / 'board.ini').write_text(''.join(f'{k}={config[k]}\n' for k in KEYS))
        (stage / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
        for n in (*payloads, chd.name, 'board.ini', 'manifest.json'):
            os.replace(stage / n, output / n)
    return manifest


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--set', required=True, dest='name')
    parser.add_argument('--mame-source', required=True, type=Path)
    parser.add_argument('--rompath', required=True, type=Path)
    parser.add_argument('--output-dir', required=True, type=Path)
    parser.add_argument('--archive', type=Path)
    args = parser.parse_args()
    try:
        result = prepare(args.name, args.mame_source, args.rompath, args.output_dir, args.archive)
        print(f"Metro {args.name}: {len(result['source_files'])} verified chips and original disk; H8 remains undumped")
    except (mame.PreparationError, assembly.TaitoFX1PreparationError) as exc:
        parser.exit(1, str(exc) + '\n')
