#!/usr/bin/env python3
"""Prepare pinned Primal Rage II ZN ROM/EEPROM/CHD media, without downloads."""
from __future__ import annotations
import argparse
from dataclasses import replace
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import prepare_mame_psx as mame
import capcom_zn_boards as zn
import prepare_taito_fx1_board as assembly
from prepare_tecmo_tps_board import KEYS

SETS = ('primrag2',)
TAGS = ('maincpu:rom', 'roms', 'at28c16', 'cat702_1', 'cat702_2')
FILES = ('main.bin', 'fixed.bin', 'eeprom_default.bin', 'cat702_key0.bin', 'cat702_key1.bin')
CHD_SHA1 = 'bc615068ddf4fd967f770ee01c02f285c052c4c5'

def source_profile(source: Path, name: str):
    if name not in SETS:
        raise mame.PreparationError('only Primal Rage II 0.36a is reviewed')
    if subprocess.run(['git','-C',str(source),'rev-parse','HEAD'],check=True,capture_output=True,text=True).stdout.strip() != mame.PINNED_MAME_COMMIT:
        raise mame.PreparationError('MAME checkout differs from pinned source')
    if subprocess.run(['git','-C',str(source),'diff','--quiet','HEAD','--','src/mame/sony/zn.cpp']).returncode:
        raise mame.PreparationError('Sony ZN source differs from pinned commit')
    text=(source/'src/mame/sony/zn.cpp').read_text();game=zn._game_rows(text)[name]
    if (game['machine'],game['input'],game['state'],game['rotation']) != ('coh1000w','primrag2','primrag2_state','ROT0'):
        raise mame.PreparationError('Primal source machine/input/state changed')
    body=zn._function_body(text,r'void\s+{name}\s*\(\s*machine_config\s*&config\s*\)','coh1000w')
    required=('zn1_2mb_vram(config)', 'set_default_size("8M")', 'from_msec(600)', 'VT83C461(config, m_vt83c461)', 'install_read_handler(5', 'install_write_handler(5')
    if not all(s in body for s in required):
        raise mame.PreparationError('Primal board composition requires new review')
    romset,parents,_=mame._find_romset(source,name)
    if set(romset.regions) != set(TAGS)|{'upd78081'} or romset.bios or len(romset.disks)!=1:
        raise mame.PreparationError('unexpected Primal regions/media')
    disk=romset.disks[0]
    if (disk.name,disk.region,disk.index,disk.sha1,disk.readonly) != ('primrag2','ata:0:hdd',0,CHD_SHA1,False):
        raise mame.PreparationError('Primal disk declaration changed')
    unused=romset.regions['upd78081']
    specs=[next(f for f in romset.files if f.index==op.file_index) for op in unused.operations if isinstance(op,mame.LoadOp)]
    if unused.size!=0x2000 or len(unused.operations)!=1 or len(specs)!=1 or (specs[0].filename,specs[0].expected_size,specs[0].status)!=('78081g503.ic655',0x2000,'no_dump'):
        raise mame.PreparationError('unreviewed Primal undumped declaration')
    omitted=specs[0].index
    romset=replace(romset,regions={k:v for k,v in romset.regions.items() if k in TAGS},files=[f for f in romset.files if f.index!=omitted],loads=[op for op in romset.loads if op.file_index!=omitted])
    if tuple(romset.regions[t].size for t in TAGS)!=(0x80000,0x200000,0x800,8,8):
        raise mame.PreparationError('Primal ROM geometry changed')
    for tag in TAGS:
        for op in romset.regions[tag].operations:
            if not isinstance(op,mame.LoadOp):raise mame.PreparationError('unsupported Primal assembly operation')
            spec=next(f for f in romset.files if f.index==op.file_index)
            if op.source_offset or op.length!=spec.expected_size or spec.status!='good':
                raise mame.PreparationError('Primal source requires a complete good dump')
    values=('pcsx-aow-primal-zn-v1',name,game['parent'],'coh1000w','fixed','COH1000W','PRIMAL','CXD8530CQ','67737600','CXD8561Q','53693175','8388608','2097152','false')
    return romset,parents,dict(zip(KEYS,values))

def verify_chd(path: Path):
    tool=shutil.which('chdman')
    if not tool:raise mame.PreparationError('chdman is required to verify Primal HDD data/metadata SHA1')
    try:
        info=subprocess.run([tool,'info','-i',str(path),'-v'],check=True,capture_output=True,text=True).stdout
        verified=subprocess.run([tool,'verify','-i',str(path)],check=True,capture_output=True,text=True)
    except (OSError,subprocess.CalledProcessError) as e:
        raise mame.PreparationError('Primal CHD verification failed: '+str(getattr(e,'stderr','') or e)) from e
    sha=re.search(r'^SHA1:\s*(\S+)',info,re.M)
    geometry=re.search(r'CYLS:(\d+),HEADS:(\d+),SECS:(\d+),BPS:(\d+)',info)
    if not sha or sha[1]!=CHD_SHA1 or not geometry:
        raise mame.PreparationError('Primal CHD SHA1 or HDD metadata mismatches source')
    cyl,heads,sectors,bps=map(int,geometry.groups())
    if not (0<cyl<=65535 and 0<heads<=255 and 0<sectors<=255 and bps==512 and cyl*heads*sectors<=0x7fffffff):
        raise mame.PreparationError('Primal HDD geometry unsupported by source ATA backend')
    return {'sha1':sha[1],'geometry':{'cylinders':cyl,'heads':heads,'sectors_per_track':sectors,'sector_bytes':bps},'data_and_metadata_verified':True,'verification':verified.stdout.strip()}

def prepare(name: str,source: Path,rompath: Path,output: Path):
    romset,parents,config=source_profile(source,name)
    regions,origins=assembly.assemble_verified(romset,rompath,parents,None,None)
    chd=rompath/name/'primrag2.chd'
    if not chd.is_file():raise mame.PreparationError('required original primrag2/primrag2.chd is missing')
    report=verify_chd(chd);by_index={s.index:s for s in romset.files}
    payloads={fn:regions[tag] for tag,fn in zip(TAGS,FILES)}
    manifest={'format':'pcsx-aow-primal-zn-v1','mame_source_commit':mame.PINNED_MAME_COMMIT,'mame_driver':'sony/zn.cpp','profile':config,'board_execution_verified':False,
              'omitted_media':{'region':'upd78081','filename':'78081g503.ic655','reason':'source uses behavioral znmcu_device; no firmware substitution'},
              'source_files':{by_index[i].filename:o for i,o in origins.items()},'regions':{n:{'size':len(b),'sha1':hashlib.sha1(b).hexdigest()} for n,b in payloads.items()},'hdd':report}
    output.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='prepare-primal-',dir=output.parent) as td:
        stage=Path(td)
        for n,b in payloads.items():(stage/n).write_bytes(b)
        shutil.copyfile(chd,stage/'primrag2.chd')
        (stage/'board.ini').write_text(''.join(f'{k}={config[k]}\n' for k in KEYS))
        (stage/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
        for n in (*payloads,'primrag2.chd','board.ini','manifest.json'):os.replace(stage/n,output/n)
    return manifest

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--set',required=True,dest='name');p.add_argument('--mame-source',required=True,type=Path);p.add_argument('--rompath',required=True,type=Path);p.add_argument('--output-dir',required=True,type=Path);a=p.parse_args()
    try:
        result=prepare(a.name,a.mame_source,a.rompath,a.output_dir)
        print(f"Primal ZN {a.name}: {len(result['source_files'])} source chips and original HDD SHA1/data/metadata verified; {a.output_dir}")
    except (mame.PreparationError,assembly.TaitoFX1PreparationError) as e:p.exit(1,f'{e}\n')
