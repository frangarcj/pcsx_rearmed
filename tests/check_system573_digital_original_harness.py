#!/usr/bin/env python3
"""Verify original KAA installation, scored drums and optional live playback."""
import argparse
import array
import hashlib
import json
from pathlib import Path
import re
import struct
import sys
import wave
import zlib

CHIPS = ['31m','27m','31l','27l','31j','27j','31h','27h']
NV_SIZE = 16851116
INSTALL_INPUTS = 'frame,control,pressed\n0,boot_cd,1\n2400,test,1\n2460,test,0\n'


def nvram(path):
    data=path.read_bytes()
    assert len(data)==NV_SIZE and data[:16]==b'S573NV3\0PF4KAA\0\0',path
    assert struct.unpack_from('<II',data,16)==(NV_SIZE,zlib.crc32(data[32:])),path
    assert data[24:32]==bytes(8),path
    return data


def diagnostic(log,prefix):
    return next(line for line in log.splitlines() if line.startswith(prefix))


def verify(root,reference):
    assert (root/'cold-update.csv').read_text()==INSTALL_INPUTS
    result={};images={};rams={}
    for gpu in ['neon','gl']:
        log=(root/f'install-{gpu}.log').read_text()
        assert 'cpu=interpreter frames=14400' in log and 'Arcade inputs: consumed=3/3' in log
        assert 'execution stopped' not in log and 'execution limit reached' not in log
        board=diagnostic(log,'System573:')
        assert all(token in board for token in ['profile=pcnfrk4mk','faults=0',
            'media=SOURCE_CRC_VERIFIED','cassette_seed=SOURCE_DUMP',
            'CDROM=SOURCE_BAD_DUMP','NVRAM=SOURCE_SEED']),board
        assert 'seven original chip CRCs and original CD SHA matched' in log
        digital=diagnostic(log,'System573 digital:')
        assert all(token in digital for token in ['faults=0','clock_faults=0','nonzero=0']),digital
        cd=diagnostic(log,'System573 CD:')
        assert 'faults=0' in cd and 'packets=547 sectors=8426' in cd and 'DMA_transfers=8426 DMA_words=4314112' in cd,cd
        host=diagnostic(log,'host=');cycles=int(re.search(r'cycles=(\d+)',host)[1])
        assert cycles>2**32
        image=nvram(root/f'install-{gpu}.nv');images[gpu]=image
        chips={}
        for i,name in enumerate(CHIPS):
            actual=image[32+i*0x200000:32+(i+1)*0x200000]
            expected=(reference/f'29f016a.{name}').read_bytes()
            assert len(expected)==0x200000 and actual==expected,(gpu,name)
            # This original installation leaves both J chips erased; their
            # exact FF contents also match the independent MAME result.
            assert (actual==b'\xff'*len(actual))==(name in ['31j','27j']),(gpu,name)
            chips[name]=hashlib.sha256(actual).hexdigest()
        offset=32+0x1002000
        for name,size in [('cassette_game_eeprom',140),('ata_0_cr589',65536)]:
            actual=image[offset:offset+size];expected=(reference/name).read_bytes()
            assert len(expected)==size and actual==expected,(gpu,name)
            chips[name]=hashlib.sha256(actual).hexdigest();offset+=size
        drive=image[-65536:]
        assert drive[0x3ab:0x3ab+28]==b'MATSHITACD-ROM CR-589   gscb'
        ram=(root/f'install-{gpu}.ram').read_bytes();assert len(ram)==4194304;rams[gpu]=ram
        result[gpu]={'cycles':cycles,'programmed_sha256':chips,
                     'ram_sha256':hashlib.sha256(ram).hexdigest(),
                     'nvram_sha256':hashlib.sha256(image).hexdigest()}
    capture=diagnostic((root/'install-gl.log').read_text(),'capture=')
    assert any(source in capture for source in ['source=OpenGL-back-buffer-before-swap',
        'source=OpenGL-front-buffer-last-presentation']),capture
    # Calendar fields are seeded from host time on cold start; preserve any
    # differences in these whole-machine artifacts rather than masking bytes.
    result['whole_ram_equal']=rams['neon']==rams['gl']
    result['whole_nvram_equal']=images['neon']==images['gl']
    return result


def player_inputs():
    events=[(0,'boot_cd',0),(1800,'test',1),(1860,'test',0)]
    for frame in [10000,10060]: events.extend([(frame,'coin1',1),(frame+20,'coin1',0)])
    for frame in [10120,10480,10720,12060,12420,12720]:
        events.extend([(frame,'p1start',1),(frame+20,'p1start',0)])
    for frame in range(12800,13780,8):
        for button in range(1,7): events.extend([(frame,f'p1button{button}',1),(frame+4,f'p1button{button}',0)])
    return 'frame,control,pressed\n'+''.join(f'{f},{n},{v}\n' for f,n,v in sorted(events))


def pcm(path):
    with wave.open(str(path),'rb') as wav:
        assert (wav.getframerate(),wav.getnchannels(),wav.getsampwidth())==(44100,2,2),path
        frames=wav.getnframes();samples=array.array('h',wav.readframes(frames))
    if sys.byteorder!='little': samples.byteswap()
    return frames,samples


def verify_player(root,live=False):
    assert (root/'player-pads.csv').read_text()==player_inputs()
    scores=[0x3493fc,0x349400,0x349410,0x349538]
    before=(root/'player-menu-neon.ram').read_bytes()
    assert all(struct.unpack_from('<I',before,offset)[0]==0 for offset in scores)
    control_inputs='\n'.join(row for row in player_inputs().splitlines() if ',p1button' not in row)+'\n'
    assert (root/'player-no-pads.csv').read_text()==control_inputs
    control_log=(root/'player-no-pads-neon.log').read_text()
    assert 'cpu=interpreter frames=13800' in control_log and 'Arcade inputs: consumed=19/19' in control_log
    assert 'execution limit reached' not in control_log and 'execution stopped' not in control_log
    assert 'NVRAM=RESTORED' in diagnostic(control_log,'System573:')
    for prefix in ['System573:','System573 digital:','System573 CD:','System573 speaker:']:
        assert 'faults=0' in diagnostic(control_log,prefix)
    control_ram=(root/'player-no-pads-neon.ram').read_bytes()
    assert len(control_ram)==4194304
    assert all(struct.unpack_from('<I',control_ram,offset)[0]==0 for offset in scores)
    names=['player-pads-neon','player-pads-gl']+(['live-replay'] if live else [])
    result={'no_pad_control_score':0};rams={};images={}
    for name in names:
        log=(root/f'{name}.log').read_text()
        assert 'cpu=interpreter frames=13800' in log and 'Arcade inputs: consumed=1495/1495' in log
        assert 'execution limit reached' not in log and 'execution stopped' not in log
        board=diagnostic(log,'System573:')
        assert all(token in board for token in ['profile=pcnfrk4mk','faults=0','media=SOURCE_CRC_VERIFIED',
            'cassette_seed=SOURCE_DUMP','CDROM=SOURCE_BAD_DUMP','NVRAM=RESTORED']),board
        digital=diagnostic(log,'System573 digital:')
        assert all(token in digital for token in ['FPGA=e000 MPEG=4000','faults=0','clock_faults=0','input_rate=44100']),digital
        assert int(re.search(r'nonzero=(\d+)',digital)[1])>1000000,digital
        cd=diagnostic(log,'System573 CD:')
        assert all(token in cd for token in ['sectors=13260','faults=0','DMA_transfers=13260 DMA_words=6789120']),cd
        speaker=diagnostic(log,'System573 speaker:')
        assert all(token in speaker for token in ['SPU_gain=1 CDDA_gain=1','faults=0']),speaker
        assert int(re.search(r'SPU_nonzero=(\d+)',speaker)[1])>0
        assert int(re.search(r'pending_SPU=(\d+)',speaker)[1])<=8
        assert 'gain=1' in diagnostic(log,'System573 digital speaker:')
        ram=(root/f'{name}.ram').read_bytes();assert len(ram)==4194304;rams[name]=ram
        assert all(struct.unpack_from('<I',ram,offset)[0]==1150 for offset in scores),name
        # Original program's getter: lui v0,8035; lw v0,9538(v0).
        assert struct.unpack_from('<II',ram,0x8a04c)==(0x3c028035,0x8c429538)
        images[name]=nvram(root/f'{name}.nv')
        frames,samples=pcm(root/f'{name}-speaker.wav')
        assert frames==int(re.search(r' frames=(\d+)',speaker)[1])
        peak=max(map(abs,samples));assert peak==int(re.search(r'peak=(\d+)',speaker)[1])>0
        if name=='live-replay':
            host_speaker=diagnostic(log,'Interactive System573 speaker:')
            assert 'error=0' in host_speaker and f'frames={frames} ' in host_speaker,host_speaker
        result[name]={'score':1150,'speaker_frames':frames,'peak':peak,
            'clipped_channels':int(re.search(r'clipped=(\d+)',speaker)[1]),
            'ram_sha256':hashlib.sha256(ram).hexdigest(),'nvram_sha256':hashlib.sha256(images[name]).hexdigest()}
    for suffix in ['spu.wav','speaker.wav']:
        baseline=(root/f'player-pads-neon-{suffix}').read_bytes()
        assert baseline==(root/f'player-pads-gl-{suffix}').read_bytes(),suffix
        result[f'headless_equal_{suffix}_sha256']=hashlib.sha256(baseline).hexdigest()
    if live:
        frames,raw=pcm(root/'player-pads-gl-spu.wav');live_frames,live_raw=pcm(root/'live-replay-spu.wav')
        mixed_frames,mixed=pcm(root/'player-pads-gl-speaker.wav');live_mixed_frames,live_mixed=pcm(root/'live-replay-speaker.wav')
        assert frames==live_frames==mixed_frames==live_mixed_frames
        raw_changed=mixed_changed=0;first=last=None;residuals={}
        for i,(a,b,c,d) in enumerate(zip(raw,live_raw,mixed,live_mixed)):
            raw_changed+=a!=b;mixed_changed+=c!=d
            if c!=d: assert a!=b,('mixed change outside raw SPU support',i)
            residual=(d-c)-(b-a)
            assert abs(residual)<=1,('unaccounted source-mix difference',i,residual)
            if a!=b or c!=d:
                if first is None: first=i
                last=i;residuals[residual]=residuals.get(residual,0)+1
        result['live_audio_comparison']={'raw_differing_channel_samples':raw_changed,
            'mixed_differing_channel_samples':mixed_changed,'mix_minus_spu_delta_residuals':residuals,
            'first_difference_seconds':first/88200 if first is not None else None,
            'last_difference_seconds':last/88200 if last is not None else None,
            'raw_sha256':hashlib.sha256((root/'live-replay-spu.wav').read_bytes()).hexdigest(),
            'mixed_sha256':hashlib.sha256((root/'live-replay-speaker.wav').read_bytes()).hexdigest()}
    a=rams['player-pads-neon'];b=rams['player-pads-gl']
    result['ram_differing_bytes']=sum(x!=y for x,y in zip(a,b))
    a=images['player-pads-neon'];b=images['player-pads-gl']
    different=[i for i,(x,y) in enumerate(zip(a,b)) if x!=y]
    # Cold host calendar seeds remain live through the source timekeeper.
    # Require every other byte of persisted board state to match exactly.
    assert all(20<=i<24 or 0x1002019<=i<=0x100201f for i in different),different
    result['nvram_differing_offsets']=[hex(i) for i in different]
    if live:
        a=images['player-pads-gl'];b=images['live-replay']
        different=[i for i,(x,y) in enumerate(zip(a,b)) if x!=y]
        assert all(20<=i<24 or 0x1002019<=i<=0x100201f for i in different),different
        result['live_nvram_differing_offsets']=[hex(i) for i in different]
    return result


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--evidence-dir',required=True,type=Path)
    p.add_argument('--reference-nvram',type=Path,help='independent MAME installation directory')
    p.add_argument('--player',action='store_true',help='also verify the original scored drum replay')
    p.add_argument('--live',action='store_true',help='also require the same replay through SDL playback')
    args=p.parse_args();root=args.evidence_dir.resolve()
    reference=args.reference_nvram or root/'mame-install/nvram/pcnfrk4mk'
    result=verify(root,reference)
    if args.player or args.live: result['player']=verify_player(root,args.live)
    print(json.dumps(result,indent=2))
    print('Original KAA native cold install, eight flashes, ZS01 and updated CR-589 match independent MAME PASS')
    if args.player or args.live:
        print('Original KAA scored six-pad replay and source MPEG/SPU mix PASS'+(' with live SDL' if args.live else ''))


if __name__=='__main__': main()
