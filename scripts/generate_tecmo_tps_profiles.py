#!/usr/bin/env python3
"""Embed pinned common TPS profiles, ROM CRC/layout and untouched padding checks."""
import argparse
import json
from pathlib import Path
import prepare_tecmo_tps_board as tps
import prepare_mame_psx as mame

ROOT=Path(__file__).resolve().parents[1]

def generate(source):
    rows=[];out=['/* Generated from MAME '+mame.PINNED_MAME_COMMIT,
                  ' * sony/zn.cpp (BSD-3-Clause, smf/Philip Bennett). */']
    for name in tps.SETS:
        romset,_,config=tps.source_profile(source,name)
        checks=[]
        for i,tag in enumerate(tps.media_tags(name)):
            for op in romset.regions[tag].operations:
                spec=next(s for s in romset.files if s.index==op.file_index)
                checks.append(f'{{{[0,1,5,6,3][i]}, {op.offset:#x}, {op.length:#x}, {op.group}, {op.stride}, {int(op.swap)}, 0x{spec.crc32}u}}')
        out += [f'static const TpsMediaCheck tps_media_{name}[] = {{',',\n'.join(checks),'};']
        fields=[name,config['mame_parent'],'coh1002m','fixed','COH1002M','TPS','glpracr2']
        rows.append('{'+', '.join(json.dumps(f) for f in fields)+', 67737600, 53693175, 0x400000, 0x200000, 0x80000, 0x2800000, 0, '+('0x40000' if name == 'glpracr2l' else '0')+', 0}')
    out += ['static const ZnProfile tps_profiles[] = {',',\n'.join(rows),'};']
    out += ['static const TpsMediaCheck *const tps_media[] = {'+','.join(f'tps_media_{n}' for n in tps.SETS)+'};',
            'static const size_t tps_media_count[] = {'+','.join(f'sizeof(tps_media_{n}) / sizeof(tps_media_{n}[0])' for n in tps.SETS)+'};','']
    return '\n'.join(out)

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--source',type=Path,default=ROOT.parent/'mamedev');p.add_argument('--check',action='store_true');a=p.parse_args()
    path=ROOT/'libpcsxcore/arcade/tecmo_tps_profiles.inc';s=generate(a.source)
    if a.check:
        if path.read_text()!=s: raise SystemExit('Tecmo TPS native profiles are stale')
    else: path.write_text(s)
