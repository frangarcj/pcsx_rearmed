#!/usr/bin/env python3
"""Native MIPS map/media/NVRAM vectors; synthetic guest, not game evidence."""
import argparse
import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

sys.path.insert(0,str(pathlib.Path(__file__).resolve().parents[1]/'scripts'))
from prepare_twinkle_board import configuration

def guest(fault=None):
    words=[]
    def li(r,v): words.extend([0x3c000000|r<<16|v>>16,0x34000000|r<<21|r<<16|v&65535])
    def store(v,a,w=4):
        li(8,a);li(9,v);words.append({1:0xa0000000,2:0xa4000000,4:0xac000000}[w]|8<<21|9<<16)
    def read(a,slot,w=4):
        li(8,a);words.extend([{1:0x90000000,2:0x94000000,4:0x8c000000}[w]|8<<21|9<<16,0,0xac000000|10<<21|9<<16|(0x100+slot*4)])
    def wait(n):
        li(11,n);words.extend([0x256bffff,0x1560fffe,0])
    def ncr(r,v): store(v,0xbf200000+r*2,1)
    li(10,0x80000000)
    store(0xff,0xbf210001,1);read(0xbf210001,30,1)
    store(0x5a,0xbf2105fe,1);read(0xbf2105fe,31,2);read(0xbf2105ff,32,1);read(0xbf2105fc,33,1)
    store(0x1234,0x803ff000);read(0x803ff000,0)
    store(0x56,0xbf000006,1);read(0xbf000006,1,2);read(0xbf000007,2,1)
    store(0x5a,0xbf2a0040,1);read(0xbf2a0006,3,1)
    store(0xc7,0xbf2a0006,1);read(0xbf2a0006,4,2)
    store(0x0d,0xbf290000,1);read(0xbf290002,5,1);read(0xbf290002,6,1)
    for slot,index in [(7,7),(8,0x0f),(9,0x17),(10,0x1f)]:
        store(index,0xbf220002,1);read(0xbf220004,slot,4)
    read(0xbf240000,11)
    store(0x0005004e,0xbf801048);read(0xbf801048,12)
    # Actual NCR/SCSI READ(10), source target 4, one real CHD sector via DMA5.
    for v in (0x28,0,0,0,0,0,0,0,1,0): ncr(2,v)
    ncr(8,7);ncr(4,4);ncr(3,0x41);wait(15000);read(0xbf20000a,13,1)
    ncr(11,8);ncr(0,0);ncr(1,8);ncr(14,0)
    store(0x00800000,0xbf8010f0);store(0x80300000,0xbf8010d0)
    store(0x00010200,0xbf8010d4);store(0x01000200,0xbf8010d8)
    ncr(3,0x90);wait(60000);read(0xbf8010d8,14)
    store(0x54474f4b,0x8000013c)
    if fault=="bad-dma":
        store(0x803ffffc,0xbf8010d0);store(0x00010002,0xbf8010d4);store(0x01000200,0xbf8010d8)
    elif fault: read(fault,16,1)
    if not fault:
        def pins(c,d): store((c<<4)|(d<<3),0xbf270000,1)
        def start():
            pins(0,1);pins(1,1);pins(1,0);pins(0,0)
        def stop():
            pins(0,0);pins(1,0);pins(1,1)
        def put(v,slot):
            for i in range(7,-1,-1):
                d=(v>>i)&1;pins(0,d);pins(1,d);pins(0,d)
            pins(0,1);pins(1,1);read(0xbf280000,slot);pins(0,1)
        start();put(0xa0,24);put(0x7a,25);put(0x5c,26);stop()
        start();put(0xa0,27);put(0x7a,28);start();put(0xa1,29)
        for i in range(8):
            pins(1,1);read(0xbf280000,16+i);pins(0,1)
        pins(0,1);pins(1,1);pins(0,1);stop()
    words.extend([0x1000ffff,0])
    return struct.pack('<'+'I'*len(words),*words)

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--harness',required=True,type=pathlib.Path)
    p.add_argument('--reject-chd',action='store_true');args=p.parse_args();binary=args.harness.resolve()
    with tempfile.TemporaryDirectory(prefix='pcsx-twinkle-native-') as temp:
        root=pathlib.Path(temp);(root/'board.ini').write_text(''.join(f'{k}={v}\n' for k,v in configuration().items()))
        def bios(fault=None):
            program=guest(fault);(root/'bios.bin').write_bytes(program+bytes(524288-len(program)))
        bios();sound=bytearray(524288);struct.pack_into('>II',sound,0,0x140000,8);sound[8:10]=bytes.fromhex('60fe')
        (root/'soundcpu.bin').write_bytes(sound);(root/'security.bin').write_bytes(bytes(range(256)))
        state=root/'state.nv'
        def run(selector='--twinkle-fixture',extra=()):
            return subprocess.run([str(binary),selector,str(root),'--frames','3','--inputs',str(root/'inputs.csv'),
                '--dump-ram',str(root/'ram.bin'),'--output',str(root/'out.ppm'),*extra],capture_output=True,text=True,timeout=30)
        (root/'inputs.csv').write_text('frame,control,pressed\n0,p1start,1\n0,coin1,1\n0,p1button1,1\n0,p1table,123\n0,p2table,45\n0,volume1,10\n')
        if args.reject_chd:
            r=run();assert r.returncode==2 and 'HAVE_CHD=1' in r.stderr,r.stderr
            print('Twinkle unavailable without CHD PASS');return
        sector=bytes((i*13+3)&255 for i in range(2048))
        (root/'cd.bin').write_bytes(sector*8);(root/'disc.cue').write_text('FILE "cd.bin" BINARY\n TRACK 01 MODE1/2048\n INDEX 01 00:00:00\n')
        subprocess.run(['chdman','createcd','-i',str(root/'disc.cue'),'-o',str(root/'gc985a01.chd'),'-c','none'],check=True,capture_output=True)
        shutil.copyfile(root/'gc985a01.chd',root/'gc985a04.chd')
        (root/'hd.bin').write_bytes(bytes(512*32))
        subprocess.run(['chdman','createhd','-i',str(root/'hd.bin'),'-o',str(root/'985hdda01.chd'),'-chs','1,1,32','-ss','512','-c','none'],check=True,capture_output=True)
        r=run(extra=('--nvram',str(state)));out=r.stdout+r.stderr;assert not r.returncode,out
        data=(root/'ram.bin').read_bytes();actual=struct.unpack_from('<16I',data,0x100)
        expected=(0x1234,0x56,0,0,0xc7,0,0x80,0xfe,45,123,10,0xbffe,0x0005004e,0x18,0x200,0x54474f4b)
        assert actual==expected,(actual,expected,out)
        assert struct.unpack_from("<4I",data,0x178)==(0,0x5a,0,0xf0)
        assert len(data)==4194304 and data[0x300000:0x300800]==sector,out
        assert 'bytes=2048 pending=0 NCR_faults=0 target_faults=0 RTC_faults=0' in out,out
        nv=state.read_bytes();assert len(nv)==4440 and nv[:13]==b'TWINKNV1BMII2'
        assert struct.unpack_from('<II',nv,16)==(4408,zlib.crc32(nv[32:]))
        security=bytearray(range(256));security[0x7a]=0x5c
        assert nv[32+56+0x5a*32+3]==0xc7 and nv[-256:]==security
        bits=struct.unpack_from('<8I',data,0x140);assert sum(((v>>12)&1)<<(7-i) for i,v in enumerate(bits))==0x5c
        assert struct.unpack_from('<6I',data,0x160)==(0,)*6
        r=run(extra=('--nvram',str(state)));assert not r.returncode,r.stdout+r.stderr
        actual=struct.unpack_from('<16I',(root/'ram.bin').read_bytes(),0x100)
        assert actual[3]==0xc7 and actual[5]==0x80,(actual,r.stdout+r.stderr)
        corrupt=bytearray(nv);corrupt[200]^=1;state.write_bytes(corrupt)
        r=run(extra=('--nvram',str(state)));assert r.returncode and 'rejected SystemTwinkle board NVRAM payload' in r.stderr,r.stderr
        assert state.read_bytes()==corrupt
        # Normal profile never accepts generated chips; unsupported devices stop explicitly.
        r=run('--twinkle');assert r.returncode and 'original chip size/SHA1 mismatch' in r.stderr,r.stderr
        for address,token in [(0xbf2107e8,'unsupported Super-I/O floppy MSR')]:
            bios(address);r=run();assert r.returncode and token in r.stderr,r.stdout+r.stderr
        bios('bad-dma');r=run();assert r.returncode and 'invalid SCSI DMA range' in r.stderr,r.stdout+r.stderr
        bios();r=run(extra=('--frames','90'));assert r.returncode and 'watchdog expired' in r.stderr,r.stdout+r.stderr
        bios();r=run(extra=('--board-test',));assert not r.returncode,r.stdout+r.stderr
        assert 'SystemTwinkle SPU DMA' in r.stderr,r.stdout+r.stderr
        print(out,end='');print('Twinkle MIPS RAM/mailbox/RTC/input/SIO/SCSI DMA, cold/warm NVRAM, strict chips and unsupported peripheral rejection PASS')

if __name__=='__main__':main()
