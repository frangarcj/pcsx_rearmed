#!/usr/bin/env python3
"""Execute Atlus 8MiB banks, 16-bit latch lanes/IRQ3, coin GPIO and source SPU DMA."""
import argparse
import pathlib
import struct
import subprocess
import tempfile
import sys
sys.path.insert(0,str(pathlib.Path(__file__).resolve().parents[1]/'scripts'))
import prepare_atlus_zn_board as atlus

def run(harness,root,fixture=True,ram=None):
    cmd=[str(harness),'--atlus-zn-fixture' if fixture else '--atlus-zn',str(root),
         '--frames','3','--output',str(root/'frame.ppm')]
    if ram:cmd+=['--dump-ram',str(ram)]
    return subprocess.run(cmd,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=45)

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--harness',required=True,type=pathlib.Path);a=p.parse_args()
    with tempfile.TemporaryDirectory(prefix='pcsx-atlus-') as tmp:
        root=pathlib.Path(tmp);config=dict(zip(atlus.KEYS,('pcsx-aow-atlus-zn-v1','hvnsgate','coh1001l','coh1001l','fixed',
             'COH1001L','ATLUS','CXD8530CQ','67737600','CXD8561Q','53693175','4194304','2097152','false')))
        (root/'board.ini').write_text(''.join(f'{k}={v}\n' for k,v in config.items()));code=[]
        def emit(*words):code.extend(words)
        def lui(r,v):emit(0x3c000000|(r<<16)|v)
        def addiu(r,v):emit(0x24000000|(r<<16)|(v&65535))
        def store(r,b,o,w=4):emit(({1:0xa0000000,2:0xa4000000,4:0xac000000}[w])|(b<<21)|(r<<16)|(o&65535))
        def load(r,b,o):emit(0x8c000000|(b<<21)|(r<<16)|(o&65535),0)
        def save(b,o,d):load(9,b,o);store(9,10,d)
        def delay():addiu(12,1000);emit(0x258cffff,0x1580fffe,0)
        def call(address,d):lui(11,address>>16);emit(0x356b0000|(address&65535),0x0160f809,0);store(5,10,d)
        lui(10,0x8000);lui(8,0x1fb0)
        addiu(9,0x1234);store(9,8,0,2);delay() # pure word command
        lui(9,1);emit(0x3529a1b2);store(9,8,0);delay() # one dword writes both latch and bank
        lui(11,0xbf00);save(11,0,0x100);call(0xbf000100,0x140)
        lui(8,0x1fb0);addiu(9,0x56);store(9,8,1,1);delay() # full latch replacement with shifted high byte
        addiu(9,0x21);store(9,8,0,1);delay() # full replacement with low byte
        addiu(9,0xff);store(9,8,2,1) # low2 bank3
        lui(11,0x9f00);save(11,0,0x104);call(0x9f000100,0x144)
        lui(11,0xbf80);save(11,-4,0x108)
        # Neither Tecmo nor Raizing selectors change this daughterboard's bank.
        lui(8,0x1fb0);addiu(9,0);store(9,8,6,1)
        lui(8,0x1fa1);store(9,8,0x300,1);lui(11,0x1f00);save(11,0,0x10c)
        # 16-bit bank write still only uses its first byte; ROM writes are ignored.
        lui(8,0x1fb0);addiu(9,0x80);store(9,8,2,2)
        lui(11,0x1f00);addiu(9,0x3344);store(9,11,0);save(11,0,0x110)
        # Source four counter inputs / active-high lockout wiring.
        lui(8,0x1fa2);addiu(9,0xaa);store(9,8,0,1);save(8,0,0x114)
        addiu(9,0x55);store(9,8,0,1);store(9,8,0,1);addiu(9,0);store(9,8,0,1)
        addiu(9,0x11);store(9,8,0,1)
        lui(8,0x1f80);save(8,0x1054,0x118);emit(0x1000ffff,0)
        bios=bytearray(0x80000);struct.pack_into('<'+'I'*len(code),bios,0,*code);(root/'main.bin').write_bytes(bios)
        banked=bytearray(0x2000000)
        for o,v in ((0,0x12345678),(0x800000,0x87654321),(0x1800000,0xaabbccdd),(0x1fffffc,0x13579bdf)):
            struct.pack_into('<I',banked,o,v)
        for o,v in ((0x800100,0x44),(0x1800100,0x77)):struct.pack_into('<3I',banked,o,0x24050000|v,0x03e00008,0)
        (root/'banked.bin').write_bytes(banked)
        sound=bytearray(0x40000);struct.pack_into('>2I',sound,0,0x710000,0x100);struct.pack_into('>I',sound,27*4,0x200)
        struct.pack_into('>3H',sound,0x100,0x4e72,0x2000,0x60fa)
        struct.pack_into('>HIH',sound,0x200,0x3039,0x100000,0x4e73);(root/'audiocpu.bin').write_bytes(sound)
        (root/'ymz_samples.bin').write_bytes(bytes([255])*0x400000+bytes(0x400000))
        for n in atlus.FILES[-2:]:(root/n).write_bytes(bytes(range(8)))
        rejected=run(a.harness,root,False);assert rejected.returncode!=0 and 'source ROM CRC mismatch' in rejected.stdout,rejected.stdout
        result=run(a.harness,root,True,root/'ram.bin');assert result.returncode==0,result.stdout
        expected=(0x87654321,0xaabbccdd,0x13579bdf,0xaabbccdd,0x12345678,0xaa,5)
        assert struct.unpack_from('<7I',(root/'ram.bin').read_bytes(),0x100)==expected,result.stdout
        assert struct.unpack_from('<2I',(root/'ram.bin').read_bytes(),0x140)==(0x44,0x77),result.stdout
        tokens=('SystemZN selftest: PASS','AtlusZN SPU DMA fixture: PASS','bank=0 bank_writes=3 faults=0 media=SYNTHETIC_FIXTURE',
                'commands=4 latch=0021 reads=4 IRQ3_ack=4 IRQ2_ack=0 pending=0 IRQ2=0','coin_counters=2/2/1/1 lockout=0')
        assert all(t in result.stdout for t in tokens),result.stdout
        (root/'board.ini').write_text((root/'board.ini').read_text().replace('mame_set=hvnsgate\n','mame_set=coh1001l\n'))
        rejected=run(a.harness,root);assert rejected.returncode!=0 and 'invalid or unsupported Sony ZN profile' in rejected.stdout,rejected.stdout
        print(result.stdout,end='');print('Atlus native MIPS: PASS bank8MiB=4 executed_banks=1 aliases=1 selector_low2=1 last_bank=1 ROM_readonly=1 latch16_lanes=replace dword_latch_and_bank=1 IRQ3_ack=read coin_GPIO=1 normal_loader_rejects_synthetic=1')
if __name__=='__main__':main()
