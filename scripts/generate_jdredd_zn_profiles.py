#!/usr/bin/env python3
"""Embed reviewed Judge Dredd source ROM CRC/layout and profile."""
import argparse
import json
from pathlib import Path
import prepare_jdredd_zn_board as jd
import prepare_mame_psx as mame
ROOT=Path(__file__).resolve().parents[1]
def generate(source):
    romset,_,config=jd.source_profile(source,'jdredd');checks=[]
    for i,tag in enumerate(jd.TAGS):
        for op in romset.regions[tag].operations:
            spec=next(f for f in romset.files if f.index==op.file_index)
            checks.append(f'{{{[0,1,5,6][i]}, {op.offset:#x}, {op.length:#x}, {op.group}, {op.stride}, {int(op.swap)}, 0x{spec.crc32}u}}')
    fields=['jdredd',config['mame_parent'],'coh1000a','fixed','COH1000A','JDREDD','jdredd']
    return '\n'.join(['/* Generated from MAME '+mame.PINNED_MAME_COMMIT+' sony/zn.cpp (BSD-3-Clause, smf/R. Belmont). */','static const TpsMediaCheck jdredd_media_jdredd[] = {',',\n'.join(checks),'};',
        'static const ZnProfile jdredd_profiles[] = {','{'+', '.join(json.dumps(f) for f in fields)+', 67737600, 53693175, 0x400000, 0x200000, 0x80000, 0x200000, 0, 0, 0}','};',
        'static const TpsMediaCheck *const jdredd_media[] = {jdredd_media_jdredd};','static const size_t jdredd_media_count[] = {sizeof(jdredd_media_jdredd)/sizeof(jdredd_media_jdredd[0])};',''])
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--source',type=Path,default=ROOT.parent/'mamedev');p.add_argument('--check',action='store_true');a=p.parse_args();path=ROOT/'libpcsxcore/arcade/jdredd_zn_profiles.inc';s=generate(a.source)
    if a.check:
        if path.read_text()!=s:raise SystemExit('Judge native profile is stale')
    else:path.write_text(s)
