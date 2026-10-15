#!/usr/bin/env python3
"""Audit original IIDX 2nd style media; creates no runnable native profile."""
import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess

import prepare_mame_psx as mame

SET = 'bmiidx2'
DRIVER = 'src/mame/konami/twinkle.cpp'
CHIPS = {
    '863a03.7b': (524288, '81498f73', '3599b40a5872eab3a00d345287635355fcb25a71'),
    '863a05.2x': (524288, '6f42a09e', 'cab5209f90f47b9ee6e721479913ad74e3ba84b1'),
    '985a02': (256, '059c1f99', 'f094a12c9a56d351667746a765804c94cb3f96bb'),
}
DISKS = {
    'gc985a01': ('cdrom', '0b783f11317f64552ebf3323459139529e7f315f'),
    'gc985a04': ('cdrom1', '4306417f61eb1ea92894d288cdb7c385eb4610f2'),
    '985hdda01': ('ata:0:hdd', 'a5b9ec9a5afa38f36af529d3aea5355ea7d022ca'),
}
REGIONS = {'maincpu:rom': ('bios.bin', 524288), 'audiocpu': ('soundcpu.bin', 524288),
           'security': ('security.bin', 256)}


def source_profile(source, name=SET):
    if name != SET:
        raise mame.PreparationError('only original IIDX 2nd style GC985 JAA is reviewed')
    pin = subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip()
    if pin != mame.PINNED_MAME_COMMIT or subprocess.run(
            ['git', '-C', str(source), 'diff', '--quiet', 'HEAD', '--', DRIVER]).returncode:
        raise mame.PreparationError('Twinkle source differs from reviewed MAME pin')
    text = (source / DRIVER).read_text()
    if not re.search(r'GAMEL\(\s*1999,\s*bmiidx2,\s*gq863,\s*twinklei,\s*twinklei,'
                     r'\s*twinkle_state,\s*empty_init,.*?MACHINE_IMPERFECT_SOUND,\s*layout_bmiidx\s*\)', text):
        raise mame.PreparationError('Twinkle source registration changed')
    romset, parents, _ = mame._find_romset(source, name)
    if ({f.filename: (f.expected_size, f.crc32, f.sha1) for f in romset.files} != CHIPS or
            any(f.status != 'good' for f in romset.files) or
            {k: v.size for k, v in romset.regions.items()} != {k: v[1] for k, v in REGIONS.items()} or
            {d.name: (d.region, d.sha1) for d in romset.disks} != DISKS or
            any(not d.readonly for d in romset.disks) or mame._lineage(name, parents) != ['gq863']):
        raise mame.PreparationError('Twinkle ROM/disk declarations changed')
    return romset, parents


def verify_disk(path, name, tool):
    if not path.is_file():
        raise mame.PreparationError('missing original Twinkle media: ' + path.name)
    info = subprocess.check_output([tool, 'info', '-i', str(path), '-v'], text=True)
    sha = re.search(r'^SHA1:\s*(\S+)', info, re.M)
    if not sha or sha[1] != DISKS[name][1]:
        raise mame.PreparationError('Twinkle source CHD SHA1 mismatch: ' + path.name)
    geometry = None
    if name == '985hdda01':
        rows = re.findall(r'CYLS:(\d+),HEADS:(\d+),SECS:(\d+),BPS:(\d+)', info)
        logical = re.search(r'^Logical size:\s*([\d,]+) bytes', info, re.M)
        if len(rows) != 1 or not logical:
            raise mame.PreparationError('Twinkle HDD requires original GDDD geometry')
        c, h, s, b = map(int, rows[0])
        if min(c, h, s) <= 0 or b != 512 or c*h*s*b != int(logical[1].replace(',', '')):
            raise mame.PreparationError('Twinkle HDD geometry/sector size mismatch')
        geometry = dict(cylinders=c, heads=h, sectors_per_track=s, bytes_per_sector=b)
    elif not any(t in info for t in ("Tag='CHTR'", "Tag='CHT2'")):
        raise mame.PreparationError('Twinkle CD requires original track metadata')
    verification = subprocess.check_output([tool, 'verify', '-i', str(path)], text=True,
                                         stderr=subprocess.STDOUT)
    return dict(origin=str(path.resolve()), source_sha1=sha[1], verified=True,
                geometry=geometry, info=info, verification=verification)


def audit(source, rompath, roms_only=False):
    romset, parents = source_profile(source)
    media, origins = mame._load_rom_files(romset, rompath, ['gq863'], None)
    regions = mame.assemble_regions(romset, media)
    payloads = {REGIONS[k][0]: data for k, data in regions.items()}
    report = dict(format='pcsx-aow-twinkle-media-audit-v1', set=SET,
                  mame_source_commit=mame.PINNED_MAME_COMMIT,
                  parent_chain=mame._lineage(SET, parents), source_flags='MACHINE_IMPERFECT_SOUND',
                  security='M24C02-W original 256-byte image', rom_chips_verified=True,
                  complete_media_verified=False, native_profile_available=False,
                  board_execution_verified=False,
                  soundcpu_layout='source ROM_LOAD16_WORD_SWAP; big-endian MC68000 bus bytes',
                  source_files={f.filename: dict(origin=origins[f.index], size=f.expected_size,
                                crc32=f.crc32, sha1=f.sha1, status=f.status) for f in romset.files},
                  disks={n: dict(region=v[0], source_sha1=v[1], verified=False) for n, v in DISKS.items()})
    if not roms_only:
        tool = shutil.which('chdman')
        if not tool:
            raise mame.PreparationError('chdman required for full Twinkle media verification')
        for name in DISKS:
            report['disks'][name].update(verify_disk(rompath / SET / (name+'.chd'), name, tool))
        report['complete_media_verified'] = True
    return payloads, report


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--mame-source', type=Path, required=True)
    p.add_argument('--rompath', type=Path, required=True)
    p.add_argument('--output-dir', type=Path, required=True)
    p.add_argument('--roms-only', action='store_true', help='explicit incomplete chip-only audit')
    args = p.parse_args()
    try:
        payloads, report = audit(args.mame_source, args.rompath, args.roms_only)
        args.output_dir.mkdir(parents=True, exist_ok=True)
        for name, data in payloads.items():
            (args.output_dir / name).write_bytes(data)
        (args.output_dir / 'audit.json').write_text(json.dumps(report, indent=2)+'\n')
        print(json.dumps(report, indent=2))
    except (mame.PreparationError, OSError, subprocess.CalledProcessError) as exc:
        p.exit(1, str(exc)+'\n')
