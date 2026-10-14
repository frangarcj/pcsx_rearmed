#!/usr/bin/env python3
"""Embed reviewed NBA Jam Extreme source ROM CRC/layout and profile."""
import argparse
import json
from pathlib import Path
import prepare_nbajam_zn_board as nba
import prepare_mame_psx as mame
ROOT=Path(__file__).resolve().parents[1]
def generate(source):
    romset,_,config=nba.source_profile(source,'nbajamex');checks=[]
    for i,tag in enumerate(nba.TAGS):
        for op in romset.regions[tag].operations:
            spec=next(f for f in romset.files if f.index==op.file_index)
            checks.append(f'{{{[0,1,3,5,6][i]}, {op.offset:#x}, {op.length:#x}, {op.group}, {op.stride}, {int(op.swap)}, 0x{spec.crc32}u}}')
    fields=['nbajamex',config['mame_parent'],'coh1000a','fixed','COH1000A','NBAJAM','nbajamex']
    return '\n'.join(['/* Generated from MAME '+mame.PINNED_MAME_COMMIT+' sony/zn.cpp (BSD-3-Clause, smf/R. Belmont). */','static const TpsMediaCheck nbajam_media_nbajamex[] = {',',\n'.join(checks),'};',
        'static const ZnProfile nbajam_profiles[] = {','{'+', '.join(json.dumps(f) for f in fields)+', 67737600, 53693175, 0x400000, 0x200000, 0x80000, 0x2000000, 0, 0x800000, 0}','};',
        'static const TpsMediaCheck *const nbajam_media[] = {nbajam_media_nbajamex};','static const size_t nbajam_media_count[] = {sizeof(nbajam_media_nbajamex)/sizeof(nbajam_media_nbajamex[0])};',''])
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--source',type=Path,default=ROOT.parent/'mamedev');p.add_argument('--check',action='store_true');a=p.parse_args();path=ROOT/'libpcsxcore/arcade/nbajam_zn_profiles.inc';s=generate(a.source)
    if a.check:
        if path.read_text()!=s:raise SystemExit('NBA Jam native profile is stale')
    else:path.write_text(s)
