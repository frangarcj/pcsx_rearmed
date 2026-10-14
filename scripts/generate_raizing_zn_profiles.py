#!/usr/bin/env python3
"""Embed pinned Raizing ZN profiles, ROM CRC/layout and untouched padding checks."""
import argparse
import json
from pathlib import Path
import prepare_raizing_zn_board as raizing
import prepare_mame_psx as mame

ROOT=Path(__file__).resolve().parents[1]

def generate(source):
    rows=[];out=['/* Generated from MAME '+mame.PINNED_MAME_COMMIT,
                  ' * sony/zn.cpp (BSD-3-Clause, smf/Philip Bennett). */']
    for name in raizing.SETS:
        romset,_,config=raizing.source_profile(source,name)
        checks=[]
        for i,tag in enumerate(raizing.TAGS):
            for op in romset.regions[tag].operations:
                spec=next(s for s in romset.files if s.index==op.file_index)
                checks.append(f'{{{[0,1,2,3,4,5,6][i]}, {op.offset:#x}, {op.length:#x}, {op.group}, {op.stride}, {int(op.swap)}, 0x{spec.crc32}u}}')
        out += [f'static const TpsMediaCheck raizing_media_{name}[] = {{',',\n'.join(checks),'};']
        fields=[name,config['mame_parent'],config['bios_parent'],'fixed','COH1002E','RAIZING','bldyror2' if name == 'bldyror2' else 'znt2p']
        rows.append('{'+', '.join(json.dumps(f) for f in fields)+', 67737600, 53693175, 0x400000, 0x200000, 0x80000, 0x1800000, 0x800, 0x100000, 0x400000}')
    out += ['static const ZnProfile raizing_profiles[] = {',',\n'.join(rows),'};']
    out += ['static const TpsMediaCheck *const raizing_media[] = {'+','.join(f'raizing_media_{n}' for n in raizing.SETS)+'};',
            'static const size_t raizing_media_count[] = {'+','.join(f'sizeof(raizing_media_{n}) / sizeof(raizing_media_{n}[0])' for n in raizing.SETS)+'};','']
    return '\n'.join(out)

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--source',type=Path,default=ROOT.parent/'mamedev');p.add_argument('--check',action='store_true');a=p.parse_args()
    path=ROOT/'libpcsxcore/arcade/raizing_zn_profiles.inc';s=generate(a.source)
    if a.check:
        if path.read_text()!=s: raise SystemExit('Raizing ZN native profiles are stale')
    else: path.write_text(s)
