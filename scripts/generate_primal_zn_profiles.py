#!/usr/bin/env python3
"""Embed reviewed Primal Rage II source ROM CRC/layout and profile."""
import argparse
import json
from pathlib import Path
import prepare_primal_zn_board as primal
import prepare_mame_psx as mame
ROOT=Path(__file__).resolve().parents[1]
def generate(source):
    romset,_,config=primal.source_profile(source,'primrag2');checks=[]
    for i,tag in enumerate(primal.TAGS):
        for op in romset.regions[tag].operations:
            spec=next(f for f in romset.files if f.index==op.file_index)
            checks.append(f'{{{[0,1,2,5,6][i]}, {op.offset:#x}, {op.length:#x}, {op.group}, {op.stride}, {int(op.swap)}, 0x{spec.crc32}u}}')
    fields=['primrag2',config['mame_parent'],'coh1000w','fixed','COH1000W','PRIMAL','primrag2']
    return '\n'.join(['/* Generated from MAME '+mame.PINNED_MAME_COMMIT+' sony/zn.cpp (BSD-3-Clause, smf/R. Belmont). */','static const TpsMediaCheck primal_media_primrag2[] = {',',\n'.join(checks),'};',
        'static const ZnProfile primal_profiles[] = {','{'+', '.join(json.dumps(f) for f in fields)+', 67737600, 53693175, 0x800000, 0x200000, 0x80000, 0x200000, 0x800, 0, 0}','};',
        'static const TpsMediaCheck *const primal_media[] = {primal_media_primrag2};','static const size_t primal_media_count[] = {sizeof(primal_media_primrag2)/sizeof(primal_media_primrag2[0])};',''])
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--source',type=Path,default=ROOT.parent/'mamedev');p.add_argument('--check',action='store_true');a=p.parse_args();path=ROOT/'libpcsxcore/arcade/primal_zn_profiles.inc';s=generate(a.source)
    if a.check:
        if path.read_text()!=s:raise SystemExit('Primal native profile is stale')
    else:path.write_text(s)
