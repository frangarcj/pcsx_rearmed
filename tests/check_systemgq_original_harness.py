#!/usr/bin/env python3
"""Optional original Crypt Killer scored-hit replay; requires audited local media."""
import argparse
import array
import concurrent.futures
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import wave

FRAMES = 7200
SCORE = 0x2377d4
LIVES = 0x2377cc
EEPROM_SHA1 = 'd9ff0458478ee42b7b14f101be96dd2e45edbba3'


def inputs(hits=False,three_players=False):
    rows=[]
    def event(frame,name,value): rows.append((frame,name,value))
    for i,frame in enumerate(range(4380,5980,24)):
        event(frame,'p1gunx',[189,285,381][(frame//24)%3])
        event(frame,'p1guny',[96,120,144][(frame//72)%3])
        event(frame,'p1button1',1);event(frame+6,'p1button1',0)
        if i%6==0:
            event(frame+8,'p1button2',1);event(frame+10,'p1button2',0)
    for frame in [4200,4300,4600,4900]:
        event(frame,'coin1',1);event(frame+16,'coin1',0)
    for frame in [4260,4340,4640,4940]:
        event(frame,'p1start',1);event(frame+16,'p1start',0)
    event(5990,'p1gunx',0);event(5990,'p1guny',255)
    event(5990,'p1button1',1);event(5996,'p1button1',0)
    event(6000,'p1gunx',285);event(6000,'p1guny',120)
    for frame in range(6060,6450,24):
        event(frame,'p1gunx',285);event(frame,'p1guny',120)
        event(frame,'p1button1',1);event(frame+6,'p1button1',0)
    event(6500,'p1button2',1);event(6516,'p1button2',0)
    if hits:
        for frame,y in [(6530,150),(6600,135),(6670,135)]:
            event(frame,'p1gunx',243);event(frame,'p1guny',y)
            event(frame,'p1button1',1);event(frame+16,'p1button1',0)
    if three_players:
        assert hits
        for player in [2,3]:
            for frame in [4200,4300,4600,4900]:
                event(frame,f'coin{player}',1);event(frame+16,f'coin{player}',0)
            for frame in [4260,4340,4640,4940]:
                event(frame,f'p{player}start',1);event(frame+16,f'p{player}start',0)
            event(6380,f'p{player}gunx',243);event(6380,f'p{player}guny',150)
            for frame in [6400+player*16,6530+player*16,6600+player*16,6670+player*16]:
                event(frame,f'p{player}button1',1);event(frame+16,f'p{player}button1',0)
            event(6500,f'p{player}button2',1);event(6516,f'p{player}button2',0)
        for frame in [5000,5060,5120,5180]:
            event(frame,'coin3',1);event(frame+16,'coin3',0)
        for frame in [5240,5360]:
            event(frame,'p3start',1);event(frame+16,'p3start',0)
        for frame in [5400,5460,5520,5580]:
            event(frame,'coin1',1);event(frame+16,'coin1',0)
        for frame in [5640,5760]:
            event(frame,'p3start',1);event(frame+16,'p3start',0)
        event(7100,'p3gunx',330);event(7100,'p3guny',170)
        event(7120,'p3button1',1);event(7136,'p3button1',0)
    return 'frame,control,pressed\n'+''.join(f'{f},{n},{v}\n' for f,n,v in sorted(rows,key=lambda r:r[0]))


def verify(root,three_players=False):
    result={}
    for name,hits in [('control',False),('hits',True)]:
        assert (root/f'{name}.csv').read_text()==inputs(hits),f'{name} input replay differs'
    names=['control-neon','hits-neon','hits-gl']
    if three_players:
        assert (root/'three-player-final.csv').read_text()==inputs(True,True)
        names+=['three-player-final-neon','three-player-final-gl']
    for name in names:
        log=(root/f'{name}.log').read_text()
        multi=name.startswith('three-player')
        events=480 if multi else 396 if name.startswith('hits') else 384
        assert f'cpu=interpreter frames={FRAMES}' in log
        assert f'Arcade inputs: consumed={events}/{events}' in log
        assert 'execution limit reached' not in log and 'execution stopped' not in log
        board=next(line for line in log.splitlines() if line.startswith('SystemGQ:'))
        assert all(token in board for token in ['profile=cryptklr GPU=1','media=SOURCE_VERIFIED',
            'faults=0/00000000','HDD_reads=26892 writes=0 errors=0 commands=414 DMA5=412 remaining=0']),board
        sound=next(line for line in log.splitlines() if line.startswith('GQ sound:'))
        assert 'clipped=0 queued=0/0' in sound,sound
        firmware=next(line for line in log.splitlines() if line.startswith('GQ firmware:'))
        assert 'bus_error=0' in firmware,firmware
        ram=(root/f'{name}.ram').read_bytes();assert len(ram)==4194304
        score=struct.unpack_from('<I',ram,SCORE)[0];lives=struct.unpack_from('<I',ram,LIVES)[0]
        if multi:
            players=[]
            for player,(expected_score,expected_lives,aim) in enumerate([(1000,2,(118,135)),(1000,1,(118,150)),(0,3,(205,170))]):
                offset=player*0x58
                actual=(struct.unpack_from('<I',ram,SCORE+offset)[0],struct.unpack_from('<I',ram,LIVES+offset)[0],struct.unpack_from('<HH',ram,SCORE+8+offset))
                assert actual==(expected_score,expected_lives,aim),(name,player,actual)
                assert struct.unpack_from('<I',ram,LIVES-4+offset)[0]==0x101
                players.append({'score':actual[0],'lives':actual[1],'latched_aim':actual[2]})
        else:
            assert (score,lives)==((2000,2) if name.startswith('hits') else (0,1)),(name,score,lives)
        # Original GQ420 UAA hit/kill awards in the unmodified guest program.
        assert struct.unpack_from('<I',ram,0x3e230)[0]==0x24630064
        assert struct.unpack_from('<I',ram,0x3e280)[0]==0x24630384
        nv=root/('control.nv' if name=='control-neon' else name+'.nv')
        assert len(nv.read_bytes())==128 and hashlib.sha1(nv.read_bytes()).hexdigest()==EEPROM_SHA1
        with wave.open(str(root/f'{name}.wav'),'rb') as wav:
            assert (wav.getframerate(),wav.getnchannels(),wav.getsampwidth())==(48000,2,2)
            frames=wav.getnframes();assert frames==int(re.search(r' frames=(\d+)',sound)[1])
            pcm=array.array('h',wav.readframes(frames))
            if sys.byteorder!='little': pcm.byteswap()
            peak=max(map(abs,pcm));assert 0<peak==int(re.search(r'peak=(\d+)',sound)[1])<32768
        result[name]={'score':score,'lives':lives,'audio_frames':frames,'peak':peak}
        if multi: result[name]['players']=players
    for prefix in ['hits']+(['three-player-final'] if three_players else []):
        for suffix in ['ram','wav','nv']:
            a=(root/f'{prefix}-neon.{suffix}').read_bytes();b=(root/f'{prefix}-gl.{suffix}').read_bytes()
            assert a==b,f'GL/NEON {prefix} {suffix} differ'
            key='' if prefix=='hits' else 'three_player_'
            result[f'{key}equal_{suffix}_sha256']=hashlib.sha256(a).hexdigest()
        assert 'source=OpenGL-back-buffer-before-swap' in (root/f'{prefix}-gl.log').read_text()
    return result


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--evidence-dir',type=Path,required=True)
    p.add_argument('--verify-only',action='store_true')
    p.add_argument('--three-players',action='store_true',help='also replay three active players and independent gun latches')
    p.add_argument('--neon',type=Path);p.add_argument('--gl',type=Path);p.add_argument('--profile',type=Path)
    args=p.parse_args();root=args.evidence_dir.resolve()
    if not args.verify_only:
        if not all([args.neon,args.gl,args.profile]): p.error('replay requires both harnesses and the audited original profile')
        root.mkdir(parents=True,exist_ok=False)
        for name,hits in [('control',False),('hits',True)]: (root/f'{name}.csv').write_text(inputs(hits))
        if args.three_players: (root/'three-player-final.csv').write_text(inputs(True,True))
        seed=args.profile/'default_eeprom.bin'
        assert hashlib.sha1(seed.read_bytes()).hexdigest()==EEPROM_SHA1,'source factory EEPROM differs'
        def run(name,binary):
            csv_name='three-player-final' if name.startswith('three-player') else 'control' if name=='control-neon' else 'hits'
            nv=root/('control.nv' if name=='control-neon' else name+'.nv');shutil.copyfile(seed,nv)
            command=[str(binary.resolve()),'--system-gq',str(args.profile.resolve()),'--frames',str(FRAMES),
                '--inputs',str(root/f'{csv_name}.csv'),'--nvram',str(nv),'--audio',str(root/f'{name}.wav'),
                '--dump-ram',str(root/f'{name}.ram'),'--dump-vram',str(root/f'{name}.vram'),'--output',str(root/f'{name}.ppm')]
            if name.endswith('-gl'): command.append('--capture-gl')
            with (root/f'{name}.log').open('w') as log:
                subprocess.run(command,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=600)
        with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
            cases=[('control-neon',args.neon),('hits-neon',args.neon),('hits-gl',args.gl)]
            if args.three_players: cases += [('three-player-final-neon',args.neon),('three-player-final-gl',args.gl)]
            jobs=[pool.submit(run,name,binary) for name,binary in cases]
            for job in jobs: job.result()
    print(json.dumps(verify(root,args.three_players),indent=2))
    print('Original Crypt Killer positive hit score, passive control, native sound and GL/NEON equality PASS')


if __name__=='__main__': main()
