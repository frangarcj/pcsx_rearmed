#!/usr/bin/env python3
"""Execute TPS banking/lanes/ROM protection and source SPU DMA through native MIPS."""
import argparse
import pathlib
import struct
import subprocess
import tempfile
import sys
sys.path.insert(0,str(pathlib.Path(__file__).resolve().parents[1]/'scripts'))
import prepare_tecmo_tps_board as tps


def run(harness,package,fixture,ram=None):
    command=[str(harness),'--tecmo-tps-fixture' if fixture else '--tecmo-tps',str(package),
             '--frames','3','--output',str(package/'frame.ppm')]
    if ram: command += ['--dump-ram',str(ram)]
    return subprocess.run(command,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=45)


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--harness',required=True,type=pathlib.Path);a=p.parse_args()
    with tempfile.TemporaryDirectory(prefix='pcsx-tps-') as temp:
        root=pathlib.Path(temp)
        # No real media: ROM words and keys are generated solely for this fixture.
        config=dict(zip(tps.KEYS,('pcsx-aow-tecmo-tps-v1','glpracr2','coh1002m','coh1002m','fixed',
                    'COH1002M','TPS','CXD8530CQ','67737600','CXD8561Q','53693175','4194304','2097152','false')))
        (root/'board.ini').write_text(''.join(f'{k}={v}\n' for k,v in config.items()))
        code=[]
        def emit(*words):code.extend(words)
        def lui(reg,imm):emit(0x3c000000|(reg<<16)|imm)
        def addiu(reg,imm):emit(0x24000000|(reg<<16)|(imm&65535))
        def store(reg,base,offset,width=4):emit(({1:0xa0000000,2:0xa4000000,4:0xac000000}[width])|(base<<21)|(reg<<16)|(offset&65535))
        def load(reg,base,offset):emit(0x8c000000|(base<<21)|(reg<<16)|(offset&65535),0)
        lui(10,0x8000);lui(8,0x1fa0);load(9,8,0);store(9,10,0x100)
        lui(8,0x1fb0);addiu(9,1);store(9,8,6,1)
        lui(11,0xbf00);load(9,11,0);store(9,10,0x104)
        lui(11,0x9f00);load(9,11,0);store(9,10,0x108)
        lui(11,0xbf00);emit(0x356b0100,0x0160f809,0);store(5,10,0x120)
        lui(9,2);store(9,8,4) # lane 2 selects bank 2
        lui(11,0x1f40);load(9,11,0);store(9,10,0x10c)
        lui(11,0xbf00);emit(0x356b0100,0x0160f809,0);store(5,10,0x124)
        addiu(9,4);store(9,8,6,1);lui(11,0xbf80);load(9,11,-4);store(9,10,0x110)
        addiu(9,0);store(9,8,6,1);lui(11,0xbf00);addiu(9,0x1234);store(9,11,0)
        load(9,11,0);store(9,10,0x114);lui(11,0xbf20);load(9,11,0);store(9,10,0x118)
        lui(11,0x1f80);load(9,11,0x1054);store(9,10,0x11c)
        emit(0x1000ffff,0)
        bios=bytearray(0x80000);struct.pack_into('<'+'I'*len(code),bios,0,*code);(root/'main.bin').write_bytes(bios)
        banked=bytearray(0x2800000)
        for offset,word in [(0,0x12345678),(0x800000,0x87654321),(0x1400000,0xaabbccdd),(0x27ffffc,0x13579bdf)]:struct.pack_into('<I',banked,offset,word)
        for offset,value in [(0x800100,0x44),(0x1000100,0x77)]:
            struct.pack_into('<3I',banked,offset,0x24050000|value,0x03e00008,0)
        (root/'banked.bin').write_bytes(banked)
        for name in tps.FILES[2:]:(root/name).write_bytes(bytes(range(8)))
        rejected=run(a.harness,root,False)
        assert rejected.returncode!=0 and 'source ROM CRC mismatch' in rejected.stdout,rejected.stdout
        result=run(a.harness,root,True,root/'ram.bin')
        assert result.returncode==0,result.stdout
        tokens=['SystemZN selftest: PASS','TecmoTPS SPU DMA fixture: PASS','bank=0 bank_writes=4 faults=0 media=SYNTHETIC_FIXTURE']
        assert all(token in result.stdout for token in tokens),result.stdout
        ram=(root/'ram.bin').read_bytes()
        expected=(0xff,0x87654321,0x87654321,0xaabbccdd,0x13579bdf,0x12345678,0,5)
        actual=struct.unpack_from('<8I',ram,0x100)
        assert actual==expected,(actual,expected,result.stdout)
        assert struct.unpack_from('<2I',ram,0x120)==(0x44,0x77),result.stdout
        # An unsupported daughterboard cannot use the common-map fixture path.
        (root/'board.ini').write_text((root/'board.ini').read_text().replace('mame_set=glpracr2\n','mame_set=glpracr2l\n'))
        rejected=run(a.harness,root,True)
        assert rejected.returncode!=0 and 'invalid or unsupported Sony ZN profile' in rejected.stdout,rejected.stdout
        print(result.stdout,end='');print('Tecmo TPS native MIPS: PASS bank8MiB=1 executed_bank_switch=1 aliases=1 write32_lane2=1 last_bank=1 ROM_readonly=1 padding=1 SIO_idle=5 normal_loader_rejects_synthetic=1')

if __name__=='__main__':main()
