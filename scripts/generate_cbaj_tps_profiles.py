#!/usr/bin/env python3
"""Embed pinned CBaj TPS profiles, ROM CRC/layout and untouched padding checks."""
import argparse
import json
from pathlib import Path
import prepare_cbaj_tps_board as cbaj
import prepare_mame_psx as mame

ROOT=Path(__file__).resolve().parents[1]

def generate(source):
    rows=[];out=['/* Generated from MAME '+mame.PINNED_MAME_COMMIT,
                  ' * sony/zn.cpp (BSD-3-Clause, smf/R. Belmont). */']
    for name in cbaj.SETS:
        romset,_,config=cbaj.source_profile(source,name)
        checks=[]
        for i,tag in enumerate(cbaj.TAGS):
            for op in romset.regions[tag].operations:
                if isinstance(op,mame.FillOp): continue
                spec=next(s for s in romset.files if s.index==op.file_index)
                checks.append(f'{{{[0,1,3,4,5,6,7][i]}, {op.offset:#x}, {op.length:#x}, {op.group}, {op.stride}, {int(op.swap)}, 0x{spec.crc32}u}}')
        out += [f'static const TpsMediaCheck cbaj_media_{name}[] = {{',',\n'.join(checks),'};']
        fields=[name,config['mame_parent'],config['bios_parent'],'fixed','COH1002M','CBAJ','zn1p']
        rows.append('{'+', '.join(json.dumps(f) for f in fields)+', 67737600, 53693175, 0x400000, 0x200000, 0x80000, 0x2800000, 0, 0x40000, 0x800000}')
    out += ['static const ZnProfile cbaj_profiles[] = {',',\n'.join(rows),'};']
    out += ['static const TpsMediaCheck *const cbaj_media[] = {'+','.join(f'cbaj_media_{n}' for n in cbaj.SETS)+'};',
            'static const size_t cbaj_media_count[] = {'+','.join(f'sizeof(cbaj_media_{n}) / sizeof(cbaj_media_{n}[0])' for n in cbaj.SETS)+'};','']
    return '\n'.join(out)

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--source',type=Path,default=ROOT.parent/'mamedev');p.add_argument('--check',action='store_true');a=p.parse_args()
    path=ROOT/'libpcsxcore/arcade/cbaj_tps_profiles.inc';s=generate(a.source)
    if a.check:
        if path.read_text()!=s: raise SystemExit('CBaj TPS native profiles are stale')
    else: path.write_text(s)
