#!/usr/bin/env python3
"""Embed pinned Visco ZN profiles, ROM CRC/layout and untouched padding checks."""
import argparse
import json
from pathlib import Path
import prepare_visco_zn_board as visco
import prepare_mame_psx as mame

ROOT=Path(__file__).resolve().parents[1]

def generate(source):
    rows=[];out=['/* Generated from MAME '+mame.PINNED_MAME_COMMIT,
                  ' * sony/zn.cpp (BSD-3-Clause, smf/Philip Bennett). */']
    for name in visco.SETS:
        romset,_,config=visco.source_profile(source,name)
        checks=[]
        for i,tag in enumerate(visco.TAGS):
            for op in romset.regions[tag].operations:
                spec=next(s for s in romset.files if s.index==op.file_index)
                checks.append(f'{{{[0,1,2,5,6][i]}, {op.offset:#x}, {op.length:#x}, {op.group}, {op.stride}, {int(op.swap)}, 0x{spec.crc32}u}}')
        out += [f'static const TpsMediaCheck visco_media_{name}[] = {{',',\n'.join(checks),'};']
        fields=[name,config['mame_parent'],'coh1002v','fixed','COH1002V','VISCO','aerofgts']
        rows.append('{'+', '.join(json.dumps(f) for f in fields)+', 67737600, 53693175, 0x400000, 0x200000, 0x80000, 0x1800000, 0x280000, 0, 0}')
    out += ['static const ZnProfile visco_profiles[] = {',',\n'.join(rows),'};']
    out += ['static const TpsMediaCheck *const visco_media[] = {'+','.join(f'visco_media_{n}' for n in visco.SETS)+'};',
            'static const size_t visco_media_count[] = {'+','.join(f'sizeof(visco_media_{n}) / sizeof(visco_media_{n}[0])' for n in visco.SETS)+'};','']
    return '\n'.join(out)

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--source',type=Path,default=ROOT.parent/'mamedev');p.add_argument('--check',action='store_true');a=p.parse_args()
    path=ROOT/'libpcsxcore/arcade/visco_zn_profiles.inc';s=generate(a.source)
    if a.check:
        if path.read_text()!=s: raise SystemExit('Visco ZN native profiles are stale')
    else: path.write_text(s)
