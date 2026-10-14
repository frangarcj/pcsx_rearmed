#!/usr/bin/env python3
"""Execute CBaj FIFO lanes/status/dequeue, source TPS banks, endpoints and SPU DMA."""
import argparse
import pathlib
import struct
import subprocess
import tempfile
import sys
sys.path.insert(0,str(pathlib.Path(__file__).resolve().parents[1]/'scripts'))
import prepare_cbaj_tps_board as cbaj

def run(harness,root,fixture=True,ram=None):
    cmd=[str(harness),'--cbaj-tps-fixture' if fixture else '--cbaj-tps',str(root),'--frames','3','--output',str(root/'frame.ppm')]
    if ram:cmd+=['--dump-ram',str(ram)]
    return subprocess.run(cmd,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=45)

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--harness',required=True,type=pathlib.Path);a=p.parse_args()
    with tempfile.TemporaryDirectory(prefix='pcsx-cbaj-') as tmp:
        root=pathlib.Path(tmp);config=dict(zip(cbaj.KEYS,('pcsx-aow-cbaj-tps-v1','cbaj','coh1002m','coh1002m','fixed',
             'COH1002M','CBAJ','CXD8530CQ','67737600','CXD8561Q','53693175','4194304','2097152','false')))
        (root/'board.ini').write_text(''.join(f'{k}={v}\n' for k,v in config.items()));code=[]
        def emit(*w):code.extend(w)
        def lui(r,v):emit(0x3c000000|(r<<16)|v)
        def addiu(r,v):emit(0x24000000|(r<<16)|(v&65535))
        def store(r,b,o,w=4):emit(({1:0xa0000000,2:0xa4000000,4:0xac000000}[w])|(b<<21)|(r<<16)|(o&65535))
        def load(r,b,o,w=4):emit(({1:0x90000000,2:0x94000000,4:0x8c000000}[w])|(b<<21)|(r<<16)|(o&65535),0)
        def save(b,o,d,w=4):load(9,b,o,w);store(9,10,d)
        def call(address,d):lui(11,address>>16);emit(0x356b0000|(address&65535),0x0160f809,0);store(5,10,d)
        lui(10,0x8000);lui(8,0x1fb0)
        addiu(9,0x12);store(9,8,0,1);store(9,8,1,1) # high lane ignored
        addiu(9,0x34);store(9,8,0,2)
        lui(9,0x7654);emit(0x3529bba5);store(9,8,0) # only low byte enqueues
        addiu(12,1500);emit(0x258cffff,0x1580fffe,0)
        save(8,3,0x100,1);save(8,0,0x104,2);save(8,3,0x108,1)
        save(8,0,0x10c);save(8,0,0x110,1);save(8,3,0x114,1);save(8,1,0x118,1)
        # Ordinary TPS raw 8MiB selector and executable KSEG mappings.
        addiu(9,1);store(9,8,6,1);lui(11,0xbf00);save(11,0,0x11c);call(0xbf000100,0x140)
        lui(9,2);store(9,8,4);lui(11,0x9f00);save(11,0,0x120);call(0x9f000100,0x144)
        addiu(9,4);store(9,8,6,1);lui(11,0xbf80);save(11,-4,0x124)
        addiu(9,0);store(9,8,6,1);lui(11,0x1f00);addiu(9,0x3344);store(9,11,0);save(11,0,0x128)
        lui(8,0x1f80);save(8,0x1054,0x12c);emit(0x1000ffff,0)
        bios=bytearray(0x80000);struct.pack_into('<'+'I'*len(code),bios,0,*code);(root/'main.bin').write_bytes(bios)
        banked=bytearray(0x2800000)
        for o,v in ((0,0x12345678),(0x800000,0x87654321),(0x1000000,0xaabbccdd),(0x27ffffc,0x13579bdf)):struct.pack_into('<I',banked,o,v)
        for o,v in ((0x800100,0x44),(0x1000100,0x77)):struct.pack_into('<3I',banked,o,0x24050000|v,0x03e00008,0)
        (root/'banked.bin').write_bytes(banked)
        sound=bytearray(0x40000)
        # Real Z80 fixture echoes queued bytes through RAM and the second FIFO.
        sound[:20]=bytes([0x31,0xfe,0xff,0xdb,0x91,0xe6,2,0x28,0xfa,0xdb,0x90,0x32,0,0x80,0x3a,0,0x80,0xd3,0x90,0])
        sound[19:22]=bytes([0xc3,3,0]);(root/'audiocpu.bin').write_bytes(sound)
        (root/'ymz_samples.bin').write_bytes(bytes(0x800000));(root/'misc.bin').write_bytes(bytes(0xc00))
        for n in ('cat702_key0.bin','cat702_key1.bin'):(root/n).write_bytes(bytes(range(8)))
        rejected=run(a.harness,root,False);assert rejected.returncode!=0 and 'source ROM CRC mismatch' in rejected.stdout,rejected.stdout
        result=run(a.harness,root,True,root/'ram.bin');assert result.returncode==0,result.stdout
        expected=(2,0xff12,2,0x02ffff34,0xa5,0,0xff,0x87654321,0xaabbccdd,0x13579bdf,0x12345678,5)
        actual=struct.unpack_from('<12I',(root/'ram.bin').read_bytes(),0x100);assert actual==expected,(actual,expected,result.stdout)
        assert struct.unpack_from('<2I',(root/'ram.bin').read_bytes(),0x140)==(0x44,0x77),result.stdout
        tokens=('SystemZN selftest: PASS','CbajTPS SPU DMA fixture: PASS','bank=0 bank_writes=4 faults=0 media=SYNTHETIC_FIXTURE',
                'FIFO0 depth=1024 used=0 EF/FF/HF=0/1/1 writes=3 reads=3 overflow=0 underflow=0',
                'FIFO1 depth=1024 used=0 EF/FF/HF=0/1/1 writes=3 reads=3 overflow=0 underflow=0')
        assert all(t in result.stdout for t in tokens),result.stdout
        (root/'board.ini').write_text((root/'board.ini').read_text().replace('mame_set=cbaj\n','mame_set=cbajbl\n'))
        rejected=run(a.harness,root);assert rejected.returncode!=0 and 'invalid or unsupported Sony ZN profile' in rejected.stdout,rejected.stdout
        print(result.stdout,end='');print('CBaj native MIPS: PASS FIFO_order=1 low_byte_writes=1 empty_status_source=0 byte_word_dword_reads=1 banks8MiB=1 executed_KSEG=1 last_bank=1 ROM_readonly=1 normal_loader_rejects_synthetic=1')
if __name__=='__main__':main()
