#!/usr/bin/env python3
"""Prepare source-verified Gallop Racer 2 common/link TPS media, without downloads."""
from __future__ import annotations
import argparse
from dataclasses import replace
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import prepare_mame_psx as mame
import capcom_zn_boards as zn
import prepare_taito_fx1_board as assembly

SETS = ('glpracr2', 'glpracr2j', 'glpracr2l')
TAGS = ('maincpu:rom', 'bankedroms', 'cat702_1', 'cat702_2')
FILES = ('main.bin', 'banked.bin', 'cat702_key0.bin', 'cat702_key1.bin')
def media_tags(name): return TAGS + (('linkcpu',) if name == 'glpracr2l' else ())
def media_files(name): return FILES + (('linkcpu.bin',) if name == 'glpracr2l' else ())
KEYS = ('format', 'mame_set', 'mame_parent', 'bios_parent', 'bios', 'board_model',
        'board_family', 'cpu_device', 'cpu_clock_hz', 'gpu_device', 'gpu_clock_hz',
        'main_ram_size', 'vram_size', 'board_execution_verified')


def source_profile(source: Path, name: str):
    if name not in SETS:
        raise mame.PreparationError('only glpracr2/glpracr2j/glpracr2l are reviewed; other TPS variants need their own devices')
    commit = subprocess.run(['git', '-C', str(source), 'rev-parse', 'HEAD'],
                            check=True, capture_output=True, text=True).stdout.strip()
    if commit != mame.PINNED_MAME_COMMIT:
        raise mame.PreparationError('MAME checkout differs from the pinned source')
    unchanged = subprocess.run(['git', '-C', str(source), 'diff', '--quiet', 'HEAD', '--', 'src/mame/sony/zn.cpp'])
    if unchanged.returncode:
        raise mame.PreparationError('Sony ZN source differs from the pinned commit')
    text = (source / 'src/mame/sony/zn.cpp').read_text()
    game = zn._game_rows(text)[name]
    link = name == 'glpracr2l'
    expected = ('coh1002ml', 'glpracr2', 'glpracr2l_state') if link else ('coh1002m', 'glpracr2', 'tecmo_zn_state')
    if (game['machine'], game['input'], game['state']) != expected:
        raise mame.PreparationError('TPS machine/input/state differs from the reviewed common map')
    body = zn._function_body(text, r'void\s+{name}\s*\(\s*machine_config\s*&config\s*\)', 'coh1002m')
    if 'zn1_2mb_vram(config)' not in body or 'cat702<0>(config)' not in body or 'cat702<1>(config)' not in body:
        raise mame.PreparationError('TPS source board composition changed')
    if link:
        body = zn._function_body(text, r'void\s+{name}\s*\(\s*machine_config\s*&config\s*\)', 'coh1002ml')
        if 'coh1002m(config)' not in body or 'Z80(config, m_linkcpu, 4000000)' not in body:
            raise mame.PreparationError('TPS link CPU composition changed')
        # Source exposes no ports or connection to the main CPU. Preserve that
        # explicit incomplete hardware map; never claim working cabinet link.
        port_body = zn._function_body(text, r'void\s+{name}\s*\(\s*address_map\s*&map\s*\)', 'link_port_map')
        if port_body.strip() != 'map.global_mask(0xff);':
            raise mame.PreparationError('TPS source link ports require new review')
    tags = media_tags(name)
    romset, parents, _ = mame._find_romset(source, name)
    if (set(romset.regions) != set(tags) | {'upd78081'} or romset.disks or romset.bios):
        raise mame.PreparationError('unexpected TPS ROM regions or storage/BIOS variant')
    # The parent source ZN machine instantiates znmcu_device, not the undumped
    # uPD78081. Omit only this exact unused declaration, never emit firmware.
    region = romset.regions['upd78081']
    specs = [next(f for f in romset.files if f.index == op.file_index)
             for op in region.operations if isinstance(op, mame.LoadOp)]
    if (region.size != 0x2000 or len(region.operations) != 1 or len(specs) != 1 or
        (specs[0].filename, specs[0].expected_size, specs[0].status) !=
        ('78081g503.ic655', 0x2000, 'no_dump')):
        raise mame.PreparationError('unreviewed undumped TPS source declaration')
    omitted = {s.index for s in specs}
    romset = replace(romset, regions={k:v for k,v in romset.regions.items() if k in tags},
                     files=[f for f in romset.files if f.index not in omitted],
                     loads=[op for op in romset.loads if op.file_index not in omitted])
    sizes = (0x80000, 0x2800000, 8, 8) + ((0x40000,) if link else ())
    if tuple(romset.regions[t].size for t in tags) != sizes:
        raise mame.PreparationError('TPS source region geometry changed')
    for tag in tags:
        for op in romset.regions[tag].operations:
            if not isinstance(op, mame.LoadOp):
                raise mame.PreparationError('unsupported TPS source assembly operation')
            spec = next(f for f in romset.files if f.index == op.file_index)
            if op.source_offset or op.length != spec.expected_size or spec.status != 'good':
                raise mame.PreparationError('TPS source load is not an audited full good dump')
    values = ('pcsx-aow-tecmo-tps-v1', name, game['parent'], 'coh1002m', 'fixed',
              'COH1002M', 'TPS', 'CXD8530CQ', '67737600', 'CXD8561Q', '53693175',
              '4194304', '2097152', 'false')
    return romset, parents, dict(zip(KEYS, values))


def prepare(name: str, source: Path, rompath: Path, output: Path, archive: Path | None = None):
    romset, parents, config = source_profile(source, name)
    regions, origins = assembly.assemble_verified(romset, rompath, parents, None, archive)
    by_index = {s.index:s for s in romset.files}
    payloads = {filename:regions[tag] for tag,filename in zip(media_tags(name), media_files(name))}
    manifest = {'format':'pcsx-aow-tecmo-tps-v1', 'mame_source_commit':mame.PINNED_MAME_COMMIT,
                'mame_driver':'sony/zn.cpp', 'profile':config, 'board_execution_verified':False,
                'omitted_media':{'region':'upd78081','filename':'78081g503.ic655',
                                 'reason':'source configuration uses behavioral znmcu_device; no firmware substitute'},
                'source_files':{by_index[i].filename:o for i,o in origins.items()},
                'regions':{n:{'size':len(b),'sha1':hashlib.sha1(b).hexdigest()} for n,b in payloads.items()}}
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='prepare-tecmo-tps-',dir=output.parent) as tmp:
        stage=Path(tmp)
        for n,b in payloads.items(): (stage/n).write_bytes(b)
        (stage/'board.ini').write_text(''.join(f'{k}={config[k]}\n' for k in KEYS))
        (stage/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
        for n in [*payloads,'board.ini','manifest.json']: os.replace(stage/n,output/n)
    return manifest


if __name__ == '__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--set',required=True,dest='name');p.add_argument('--mame-source',required=True,type=Path)
    p.add_argument('--rompath',required=True,type=Path);p.add_argument('--output-dir',required=True,type=Path)
    p.add_argument('--archive',type=Path);a=p.parse_args()
    try:
        result=prepare(a.name,a.mame_source,a.rompath,a.output_dir,a.archive)
        print(f"Tecmo TPS {a.name}: verified {len(result['source_files'])} source ROMs; prepared {a.output_dir}")
    except (mame.PreparationError,assembly.TaitoFX1PreparationError) as e: p.exit(1,f'{e}\n')
