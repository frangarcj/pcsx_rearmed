#!/usr/bin/env python3
"""Embed source ROM geometry/digests for the two reviewed Metro configurations."""
import argparse
import json
from pathlib import Path
import prepare_metro_zn_board as metro
import prepare_mame_psx as mame
ROOT = Path(__file__).resolve().parents[1]


def generate(source):
    out = ['/* Generated from MAME ' + mame.PINNED_MAME_COMMIT,
           ' * sony/zn.cpp (BSD-3-Clause, smf/R. Belmont). H8 remains NO_DUMP. */']
    rows = []
    for name in metro.SETS:
        romset, _, config = metro.source_profile(source, name)
        checks = []
        for index, tag in enumerate(metro.TAGS):
            for op in romset.regions[tag].operations:
                spec = next(f for f in romset.files if f.index == op.file_index)
                checks.append(f'{{{[0, 1, 5, 6][index]}, {op.offset:#x}, {op.length:#x}, '
                              f'{op.group}, {op.stride}, {int(op.swap)}, 0x{spec.crc32}u}}')
        out += [f'static const TpsMediaCheck metro_media_{name}[] = {{', ',\n'.join(checks), '};']
        fields = [name, config['mame_parent'], 'coh1002e', 'fixed', 'COH1002E', 'METRO', 'bam2']
        rows.append('{' + ', '.join(json.dumps(f) for f in fields) +
                    ', 67737600, 53693175, 0x400000, 0x200000, 0x80000, 0x2c00000, 0, 0, 0}')
    out += ['static const ZnProfile metro_profiles[] = {', ',\n'.join(rows), '};',
            'static const TpsMediaCheck *const metro_media[] = {' +
            ','.join(f'metro_media_{n}' for n in metro.SETS) + '};',
            'static const size_t metro_media_count[] = {' +
            ','.join(f'sizeof(metro_media_{n}) / sizeof(metro_media_{n}[0])' for n in metro.SETS) + '};', '']
    return '\n'.join(out)


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source', type=Path, default=ROOT.parent / 'mamedev')
    p.add_argument('--check', action='store_true')
    args = p.parse_args()
    path = ROOT / 'libpcsxcore/arcade/metro_zn_profiles.inc'
    content = generate(args.source)
    if args.check:
        if path.read_text() != content:
            raise SystemExit('Metro ZN native profiles are stale')
    else:
        path.write_text(content)
