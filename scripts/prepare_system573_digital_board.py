#!/usr/bin/env python3
"""Prepare the original KAA native CPU/MMIO profile; require a full CD audit."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import audit_system573_digital_media as media
import prepare_mame_psx as mame


def configuration():
    return {'format': 'pcsx-aow-system573-digital-v1', 'mame_set': 'pcnfrk4mk',
            'mame_parent': 'pcnfrk4m', 'bios': 'std', 'cpu': 'CXD8530CQ',
            'cpu_clock_hz': 67737600, 'gpu': 'CXD8561Q', 'gpu_clock_hz': 53693175,
            'ram_size': 4194304, 'vram_size': 2097152, 'flash_chips': 8,
            'flash_chip_size': 2097152, 'flash_bus_width': 16, 'cassette': 'ZI',
            'cdrom_present': 'true', 'board_execution_verified': 'false'}


def prepare(source, rompath, output):
    payloads, report = media.audit(source, rompath, roms_only=False)
    # No ROM-only escape hatch can publish a runnable normal-game directory.
    if not report['complete_media_verified'] or not report['disk']['verified']:
        raise mame.PreparationError('complete original KAA CD audit required')
    report.update(format='pcsx-aow-system573-digital-v1', profile=configuration(),
                  native_profile_available=True, board_execution_verified=False,
                  native_runtime_stage='CPU/MMIO/drum controls/NVRAM; digital audio clock and speaker mix pending')
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='prepare-system573-digital-', dir=output.parent) as temp:
        root = Path(temp)
        for name, data in payloads.items():
            (root / name).write_bytes(data)
        shutil.copyfile(report['disk']['origin'], root / 'a25kaa02.chd')
        (root / 'board.ini').write_text(''.join(f'{k}={v}\n' for k,v in configuration().items()))
        (root / 'manifest.json').write_text(json.dumps(report, indent=2) + '\n')
        output.mkdir(parents=True, exist_ok=True)
        for name in (*payloads, 'a25kaa02.chd', 'board.ini', 'manifest.json'):
            os.replace(root / name, output / name)
    return report


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--mame-source', required=True, type=Path)
    p.add_argument('--rompath', required=True, type=Path)
    p.add_argument('--output-dir', required=True, type=Path)
    args = p.parse_args()
    try:
        prepare(args.mame_source, args.rompath, args.output_dir)
        print('Original KAA chips and full CD verified; CPU/MMIO profile prepared; gameplay and digital speaker audio unverified')
    except (mame.PreparationError, subprocess.CalledProcessError) as exc:
        p.exit(1, str(exc) + '\n')
