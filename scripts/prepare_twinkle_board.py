#!/usr/bin/env python3
"""Prepare original IIDX2 inputs for native Twinkle bring-up, not compatibility certification."""
import argparse
import json
import os
from pathlib import Path
import shutil
import tempfile
import subprocess
import audit_twinkle_media as media


def configuration():
    return dict(format='pcsx-aow-twinkle-v1', mame_set='bmiidx2', mame_parent='gq863',
                machine='twinklei', ram_bytes='4194304', vram_bytes='2097152',
                bios='bios.bin', soundcpu='soundcpu.bin', security='security.bin',
                program_cd='gc985a01.chd', video_cd='gc985a04.chd', sound_hdd='985hdda01.chd')


def prepare(source, rompath, output):
    if output.exists():
        raise media.mame.PreparationError("Twinkle output already exists; preserve runtime NVRAM separately")
    payloads, report = media.audit(source, rompath)
    report.update(format='pcsx-aow-twinkle-v1', profile=configuration(), native_profile_available=True,
                  board_execution_verified=False,
                  native_runtime_stage='interpreter/RAM/GPU/sound/RTC/SCSI bring-up; FDC/UART/DVD accesses still stop explicitly')
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='twinkle-prepare-', dir=output.parent) as temp:
        root = Path(temp)
        for name, data in payloads.items():
            (root / name).write_bytes(data)
        for name, disk in report['disks'].items():
            original = Path(disk['origin'])
            try:
                os.link(original, root / (name+'.chd'))
            except OSError:
                shutil.copy2(original, root / (name+'.chd'))
        (root / 'board.ini').write_text(''.join(f'{k}={v}\n' for k,v in configuration().items()))
        (root / 'manifest.json').write_text(json.dumps(report, indent=2)+'\n')
        root.rename(output)
    return report


if __name__ == '__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--mame-source',type=Path,required=True);p.add_argument('--rompath',type=Path,required=True)
    p.add_argument('--output-dir',type=Path,required=True);args=p.parse_args()
    try:
        report=prepare(args.mame_source,args.rompath,args.output_dir)
        print('Original Twinkle media prepared; native main-board validation remains open')
    except (media.mame.PreparationError,OSError,subprocess.CalledProcessError) as exc:
        p.exit(1,str(exc)+'\n')
