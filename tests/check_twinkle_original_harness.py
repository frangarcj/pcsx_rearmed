#!/usr/bin/env python3
"""Optional original IIDX2 replay; requires audited media and native runtime NVRAM."""
import argparse
import array
import sys
import concurrent.futures
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import wave


def inputs():
    rows=['frame,control,pressed']
    for frame,name,value in [(5400,'coin1',1),(5412,'coin1',0),(5460,'coin1',1),(5472,'coin1',0),
                             (5520,'p1start',1),(5532,'p1start',0),(5700,'p1button1',1),(5712,'p1button1',0),
                             (5850,'p1button1',1),(5862,'p1button1',0)]:
        rows.append(f'{frame},{name},{value}')
    # Input stress is followed by a measured single-note timing probe.
    for frame in range(6020,7800,2):
        pressed=int(((frame-6020)//2)%2==0)
        rows.extend(f'{frame},p1button{button},{pressed}' for button in range(1,8))
        if pressed:rows.append(f'{frame},p1table,{((frame-6020)//4+1)%256}')
    rows.extend(['7883,p1button7,1','7885,p1button7,0'])
    return '\n'.join(rows)+'\n'


def verify(root,live=False):
    assert (root/'target-note.csv').read_text()==inputs(),'input replay differs'
    result={}
    for suffix in ['ram','vram','nv','speaker.wav']:
        def artifact(gpu):return root/(f'target-{gpu}-speaker.wav' if suffix=='speaker.wav' else f'target-{gpu}.{suffix}')
        data=artifact('neon').read_bytes();assert data==artifact('gl').read_bytes(),f'GL/NEON {suffix} differ'
        result[suffix]=hashlib.sha256(data).hexdigest()
    ram=(root/'target-neon.ram').read_bytes();assert len(ram)==4194304
    score=struct.unpack_from('<I',ram,0x17e1a8)[0]
    assert score==1204,f'expected original GC985 JAA replay score 1204, got {score}'
    for gpu in ['neon','gl']:
        log=(root/f'target-{gpu}.log').read_text()
        assert 'media=SOURCE_SHA_VERIFIED' in log and 'cpu=interpreter frames=7896' in log
        assert 'Arcade inputs: consumed=6687/6687' in log
        assert 'execution limit reached' not in log and 'execution stopped' not in log
        for marker in ['SystemTwinkle: profile=','Twinkle serial:','Twinkle speaker:']:
            line=next(line for line in log.splitlines() if line.startswith(marker))
            assert 'faults=0' in line,(gpu,line)
        sound=next(line for line in log.splitlines() if line.startswith('Twinkle sound:'))
        assert 'bus_error=0' in sound and 'failures=0' in sound
        scsi=next(line for line in log.splitlines() if line.startswith('Twinkle SCSI:'))
        assert all(token in scsi for token in ['NCR_faults=0','target_faults=0','RTC_faults=0'])
        host=next(line for line in log.splitlines() if line.startswith('host='))
        assert int(re.search(r'cycles=(\d+)',host)[1])>2**32
        speaker=next(line for line in log.splitlines() if line.startswith('Twinkle speaker:'))
        assert 'SPU_gain=0.75 SPU_channels=direct CDDA_gain=1 RF_gain=1 RF_effects=unrouted' in speaker
        assert int(re.search(r'pending_SPU=(\d+)',speaker)[1])<=8
        with wave.open(str(root/f'target-{gpu}-speaker.wav'),'rb') as wav:
            assert wav.getframerate()==44100 and wav.getnchannels()==2 and wav.getsampwidth()==2
            assert wav.getnframes()==int(re.search(r' frames=(\d+)',speaker)[1])
            assert int(re.search(r'RF_nonzero=(\d+)',speaker)[1])>0
            pcm=array.array('h',wav.readframes(wav.getnframes()))
            if sys.byteorder!='little':pcm.byteswap()
            assert any(pcm) and max(map(abs,pcm))==int(re.search(r' peak=(\d+)',speaker)[1])
    if live:
        for suffix in ['ram','vram','nv','speaker.wav']:
            baseline=root/('target-gl-speaker.wav' if suffix=='speaker.wav' else f'target-gl.{suffix}')
            actual=root/('live-speaker.wav' if suffix=='speaker.wav' else f'live.{suffix}')
            assert baseline.read_bytes()==actual.read_bytes(),f'live/headless {suffix} differ'
        log=(root/'live.log').read_text()
        assert 'cpu=interpreter frames=7896' in log and 'Arcade inputs: consumed=6687/6687' in log
        speaker=next(line for line in log.splitlines() if line.startswith('Interactive SystemTwinkle speaker:'))
        assert 'error=0' in speaker and int(re.search(r'nonzero=(\d+)',speaker)[1])>0
        assert 'media=SOURCE_SHA_VERIFIED' in log
        assert 'Twinkle live controls: keyboard_events=0 table_updates=0 positions=0/0 errors=0 replay=1' in log
        result['live_headless_equal']=True
    result.update(score=score,score_address='8017e1a8',frames=7896,input_rows=6687)
    return result


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--evidence-dir',type=Path,required=True)
    p.add_argument('--verify-only',action='store_true')
    p.add_argument('--live',action='store_true',help='also run and compare the SDL speaker/presentation path')
    p.add_argument('--neon',type=Path);p.add_argument('--gl',type=Path)
    p.add_argument('--profile',type=Path);p.add_argument('--nvram-seed',type=Path)
    args=p.parse_args();root=args.evidence_dir.resolve()
    if not args.verify_only:
        if not all([args.neon,args.gl,args.profile,args.nvram_seed]):p.error('replay requires both harnesses, original profile and initialized native NVRAM seed')
        root.mkdir(parents=True,exist_ok=False)
        (root/'target-note.csv').write_text(inputs())
        def run(gpu,binary):
            shutil.copyfile(args.nvram_seed,root/f'target-{gpu}.nv')
            command=[str(binary.resolve()),'--twinkle',str(args.profile.resolve()),'--frames','7896',
                     '--nvram',str(root/f'target-{gpu}.nv'),'--inputs',str(root/'target-note.csv'),
                     '--mixed-audio',str(root/f'target-{gpu}-speaker.wav'),'--dump-ram',str(root/f'target-{gpu}.ram'),
                     '--dump-vram',str(root/f'target-{gpu}.vram'),'--output',str(root/f'target-{gpu}.ppm')]
            if gpu=='gl':command.append('--capture-gl')
            with (root/f'target-{gpu}.log').open('w') as log:
                subprocess.run(command,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=600)
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
            futures=[pool.submit(run,'neon',args.neon),pool.submit(run,'gl',args.gl)]
            for task in futures:task.result()
        if args.live:
            shutil.copyfile(args.nvram_seed,root/'live.nv')
            command=[str(args.gl.resolve()),'--twinkle',str(args.profile.resolve()),'--interactive','--frames','7896',
                     '--nvram',str(root/'live.nv'),'--inputs',str(root/'target-note.csv'),
                     '--mixed-audio',str(root/'live-speaker.wav'),'--dump-ram',str(root/'live.ram'),
                     '--dump-vram',str(root/'live.vram'),'--capture-gl','--output',str(root/'live.ppm')]
            with (root/'live.log').open('w') as log:
                subprocess.run(command,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=600)
    print(json.dumps(verify(root,args.live),indent=2))
    print('Original IIDX2 timed note, source audio, 32-bit cycle wrap and GL/NEON equality PASS')

if __name__=='__main__':main()
