#!/usr/bin/env python3
"""Execute Visco fixed/1MiB ROM banking, lanes, inputs, EEPROM and source SPU DMA."""
import argparse
import pathlib
import struct
import subprocess
import tempfile
import sys
sys.path.insert(0,str(pathlib.Path(__file__).resolve().parents[1]/'scripts'))
import prepare_visco_zn_board as visco


def run(harness,root,fixture=True,ram=None):
    command=[str(harness),'--visco-zn-fixture' if fixture else '--visco-zn',str(root),
             '--frames','3','--output',str(root/'frame.ppm')]
    if ram: command += ['--dump-ram',str(ram)]
    return subprocess.run(command,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=45)


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--harness',required=True,type=pathlib.Path);a=p.parse_args()
    with tempfile.TemporaryDirectory(prefix='pcsx-visco-') as temp:
        root=pathlib.Path(temp)
        config=dict(zip(visco.KEYS,('pcsx-aow-visco-zn-v1','aerofgts','coh1002v','coh1002v','fixed',
                    'COH1002V','VISCO','CXD8530CQ','67737600','CXD8561Q','53693175','4194304','2097152','false')))
        (root/'board.ini').write_text(''.join(f'{k}={v}\n' for k,v in config.items()))
        code=[]
        def emit(*words):code.extend(words)
        def lui(reg,imm):emit(0x3c000000|(reg<<16)|imm)
        def addiu(reg,imm):emit(0x24000000|(reg<<16)|(imm&65535))
        def store(reg,base,offset,width=4):emit(({1:0xa0000000,2:0xa4000000,4:0xac000000}[width])|(base<<21)|(reg<<16)|(offset&65535))
        def load(reg,base,offset):emit(0x8c000000|(base<<21)|(reg<<16)|(offset&65535),0)
        def save_load(base,offset,dst):load(9,base,offset);store(9,10,dst)
        def call(address,value_at):
            lui(11,address>>16);emit(0x356b0000|(address&65535),0x0160f809,0);store(5,10,value_at)
        lui(10,0x8000);lui(8,0xbf00);save_load(8,0,0x100)
        lui(8,0xbf28);save_load(8,-4,0x104);call(0xbf000100,0x140)
        # Bank1, through source byte selector; both cached/uncached aliases.
        lui(8,0x1fb0);addiu(9,1);store(9,8,0,1)
        lui(8,0xbfb0);save_load(8,0,0x108);call(0xbfb00100,0x144)
        lui(8,0x9fb0);save_load(8,0,0x10c)
        # High byte and Tecmo's +6 selector cannot select a Visco bank.
        lui(8,0x1fb0);addiu(9,23);store(9,8,1,1);store(9,8,6,1);save_load(8,0,0x110)
        # Ordinary fixed/banked ROM bytes remain immutable.
        addiu(9,0x1234);store(9,8,0x104);save_load(8,0x104,0x114)
        lui(8,0x1f00);store(9,8,0);save_load(8,0,0x118)
        # Raw low-byte bank23, not masked to a narrower bank selector.
        lui(8,0x1fb0);addiu(9,0x17);store(9,8,0,2);save_load(8,0,0x11c)
        lui(8,0xbfc0);save_load(8,-4,0x120);call(0x9fb00100,0x148)
        # Dword only uses its low byte; ZNSECSEL does not affect Visco ROM banks.
        lui(8,0x1fb0);lui(9,0x7654);store(9,8,0)
        lui(8,0x1fa1);addiu(9,0x8e);store(9,8,0x300,1)
        lui(8,0x1fb0);save_load(8,0,0x124)
        # Exact source coin counter bits, edge-triggered and readable.
        lui(8,0x1fa2);addiu(9,0x11);store(9,8,0,1);store(9,8,0,1);save_load(8,0,0x128)
        addiu(9,0);store(9,8,0,1);addiu(9,1);store(9,8,0,1)
        lui(8,0x1f80);save_load(8,0x1054,0x12c)
        emit(0x1000ffff,0)
        bios=bytearray(0x80000);struct.pack_into('<'+'I'*len(code),bios,0,*code);(root/'main.bin').write_bytes(bios)
        fixed=bytearray(0x280000)
        struct.pack_into('<I',fixed,0,0x11223344);struct.pack_into('<I',fixed,0x27fffc,0x99aabbcc)
        struct.pack_into('<3I',fixed,0x100,0x24050022,0x03e00008,0);(root/'fixed.bin').write_bytes(fixed)
        banked=bytearray(0x1800000)
        for offset,value in ((0,0x12345678),(0x100000,0x87654321),(0x1700000,0xaabbccdd),(0x17ffffc,0x13579bdf),(0x100104,0x03e00008)):
            struct.pack_into('<I',banked,offset,value)
        for offset,value in ((0x100100,0x44),(0x1700100,0x77)):
            struct.pack_into('<3I',banked,offset,0x24050000|value,0x03e00008,0)
        (root/'banked.bin').write_bytes(banked)
        for name in visco.FILES[-2:]:(root/name).write_bytes(bytes(range(8)))
        result=run(a.harness,root,False)
        assert result.returncode!=0 and 'source ROM CRC mismatch' in result.stdout,result.stdout
        result=run(a.harness,root,True,root/'ram.bin')
        assert result.returncode==0,result.stdout
        expected=(0x11223344,0x99aabbcc,0x87654321,0x87654321,0x87654321,0x03e00008,0x11223344,
                  0xaabbccdd,0x13579bdf,0x12345678,0x11,5)
        actual=struct.unpack_from('<12I',(root/'ram.bin').read_bytes(),0x100)
        assert actual==expected,(actual,expected,result.stdout)
        assert struct.unpack_from('<3I',(root/'ram.bin').read_bytes(),0x140)==(0x22,0x44,0x77),result.stdout
        tokens=('SystemZN selftest: PASS','ViscoZN SPU DMA fixture: PASS','bank=0 bank_writes=3 faults=0 media=SYNTHETIC_FIXTURE','coin_counters=2/1')
        assert all(t in result.stdout for t in tokens),result.stdout
        # Out-of-range source selector faults rather than wrapping/reusing ROM.
        bad=bytearray(bios);target=code.index(0x24090001);struct.pack_into('<I',bad,target*4,0x24090018)
        (root/'main.bin').write_bytes(bad);invalid=run(a.harness,root)
        assert invalid.returncode!=0 and 'bank=24' in invalid.stdout and 'faults=8' in invalid.stdout,invalid.stdout
        # BIOS roots cannot impersonate games, including on the synthetic path.
        (root/'board.ini').write_text((root/'board.ini').read_text().replace('mame_set=aerofgts\n','mame_set=coh1002v\n'))
        rejected=run(a.harness,root)
        assert rejected.returncode!=0 and 'invalid or unsupported Sony ZN profile' in rejected.stdout,rejected.stdout
        print(result.stdout,end='');print('Visco native MIPS: PASS fixed2.5MiB=1 bank1MiB=24 executed_banks=1 aliases=1 selector_low_byte=1 unused_selectors_ignored=1 last_bank=1 ROM_readonly=1 coin_edges=1 NV2080_roundtrip=1 invalid_bank_rejected=1 normal_loader_rejects_synthetic=1')

if __name__=='__main__':main()
