#!/usr/bin/env python3
"""Embed pinned Atlus ZN profiles, ROM CRC/layout and untouched padding checks."""
import argparse
import json
from pathlib import Path
import prepare_atlus_zn_board as atlus
import prepare_mame_psx as mame

ROOT=Path(__file__).resolve().parents[1]

def generate(source):
    rows=[];out=['/* Generated from MAME '+mame.PINNED_MAME_COMMIT,
                  ' * sony/zn.cpp (BSD-3-Clause, smf/R. Belmont). */']
    for name in atlus.SETS:
        romset,_,config=atlus.source_profile(source,name)
        checks=[]
        for i,tag in enumerate(atlus.TAGS):
            for op in romset.regions[tag].operations:
                if isinstance(op,mame.FillOp): continue
                spec=next(s for s in romset.files if s.index==op.file_index)
                checks.append(f'{{{[0,1,3,4,5,6][i]}, {op.offset:#x}, {op.length:#x}, {op.group}, {op.stride}, {int(op.swap)}, 0x{spec.crc32}u}}')
        out += [f'static const TpsMediaCheck atlus_media_{name}[] = {{',',\n'.join(checks),'};']
        fields=[name,config['mame_parent'],config['bios_parent'],'fixed','COH1001L','ATLUS','zn2p']
        rows.append('{'+', '.join(json.dumps(f) for f in fields)+', 67737600, 53693175, 0x400000, 0x200000, 0x80000, 0x2000000, 0, 0x40000, 0x800000}')
    out += ['static const ZnFillCheck atlus_fills[] = {{4, 0, 0x400000, 0xff}};']
    out += ['static const ZnProfile atlus_profiles[] = {',',\n'.join(rows),'};']
    out += ['static const TpsMediaCheck *const atlus_media[] = {'+','.join(f'atlus_media_{n}' for n in atlus.SETS)+'};',
            'static const size_t atlus_media_count[] = {'+','.join(f'sizeof(atlus_media_{n}) / sizeof(atlus_media_{n}[0])' for n in atlus.SETS)+'};','']
    return '\n'.join(out)

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--source',type=Path,default=ROOT.parent/'mamedev');p.add_argument('--check',action='store_true');a=p.parse_args()
    path=ROOT/'libpcsxcore/arcade/atlus_zn_profiles.inc';s=generate(a.source)
    if a.check:
        if path.read_text()!=s: raise SystemExit('Atlus ZN native profiles are stale')
    else: path.write_text(s)
