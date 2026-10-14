#!/usr/bin/env python3
"""Execute Raizing bank/security lanes, source IRQ/latch and SPU DMA via MIPS."""
import argparse
import pathlib
import struct
import subprocess
import tempfile
import sys
sys.path.insert(0,str(pathlib.Path(__file__).resolve().parents[1]/'scripts'))
import prepare_raizing_zn_board as raizing


def run(harness,package,fixture,ram=None):
    command=[str(harness),'--raizing-zn-fixture' if fixture else '--raizing-zn',str(package),
             '--frames','3','--output',str(package/'frame.ppm')]
    if ram: command += ['--dump-ram',str(ram)]
    return subprocess.run(command,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=45)


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--harness',required=True,type=pathlib.Path);a=p.parse_args()
    with tempfile.TemporaryDirectory(prefix='pcsx-raizing-') as temp:
        root=pathlib.Path(temp)
        config=dict(zip(raizing.KEYS,('pcsx-aow-raizing-zn-v1','bldyror2','coh1002e','coh1002e','fixed',
                    'COH1002E','RAIZING','CXD8530CQ','67737600','CXD8561Q','53693175','4194304','2097152','false')))
        (root/'board.ini').write_text(''.join(f'{k}={v}\n' for k,v in config.items()))
        code=[]
        def emit(*words):code.extend(words)
        def lui(reg,imm):emit(0x3c000000|(reg<<16)|imm)
        def addiu(reg,imm):emit(0x24000000|(reg<<16)|(imm&65535))
        def store(reg,base,offset,width=4):emit(({1:0xa0000000,2:0xa4000000,4:0xac000000}[width])|(base<<21)|(reg<<16)|(offset&65535))
        def load(reg,base,offset):emit(0x8c000000|(base<<21)|(reg<<16)|(offset&65535),0)
        lui(10,0x8000);lui(8,0x1fa1);load(9,8,0x200);store(9,10,0x100)
        # Upper ZNSECSEL bits still reach CAT/MCU; only low two select ROM.
        addiu(9,0xfd);store(9,8,0x300,1)
        lui(11,0xbf00);load(9,11,0);store(9,10,0x104)
        lui(11,0x9f00);load(9,11,0);store(9,10,0x108)
        lui(11,0xbf00);emit(0x356b0100,0x0160f809,0);store(5,10,0x120)
        addiu(9,0xfe);store(9,8,0x300,4)
        lui(11,0x1f40);load(9,11,0);store(9,10,0x10c)
        lui(11,0xbf00);emit(0x356b0100,0x0160f809,0);store(5,10,0x124)
        lui(11,0xbf80);load(9,11,-4);store(9,10,0x110)
        # Tecmo's selector is not the Raizing selector, nor does it change banks.
        lui(8,0x1fb0);addiu(9,0);store(9,8,6,1)
        lui(11,0xbf00);load(9,11,0);store(9,10,0x114)
        # Correct source sound latch/IRQ2 low byte lanes.
        addiu(9,0x42);store(9,8,0,4);store(9,8,4,4)
        lui(8,0x1fa1);addiu(9,0xfc);store(9,8,0x300,2)
        lui(11,0xbf00);addiu(9,0x1234);store(9,11,0)
        load(9,11,0);store(9,10,0x118)
        lui(11,0x1f80);load(9,11,0x1054);store(9,10,0x11c)
        emit(0x1000ffff,0)
        bios=bytearray(0x80000);struct.pack_into('<'+'I'*len(code),bios,0,*code);(root/'main.bin').write_bytes(bios)
        banked=bytearray(0x1800000)
        for offset,word in [(0,0x12345678),(0x800000,0x87654321),(0x1400000,0xaabbccdd),(0x17ffffc,0x13579bdf)]:struct.pack_into('<I',banked,offset,word)
        for offset,value in [(0x800100,0x44),(0x1000100,0x77)]:
            struct.pack_into('<3I',banked,offset,0x24050000|value,0x03e00008,0)
        (root/'banked.bin').write_bytes(banked)
        (root/'eeprom_default.bin').write_bytes(bytes(2048));(root/'ymf_samples.bin').write_bytes(bytes(0x400000))
        sound=bytearray(0x100000);struct.pack_into('>2I',sound,0,0x100000,0x100)
        struct.pack_into('>I',sound,26*4,0x200)
        struct.pack_into('>3H',sound,0x100,0x4e72,0x2000,0x60fa)
        struct.pack_into('>HIH',sound,0x200,0x1039,0x180009,0x4e73)
        (root/'audiocpu.bin').write_bytes(sound)
        for name in raizing.FILES[-2:]:(root/name).write_bytes(bytes(range(8)))
        rejected=run(a.harness,root,False)
        assert rejected.returncode!=0 and 'source ROM CRC mismatch' in rejected.stdout,rejected.stdout
        result=run(a.harness,root,True,root/'ram.bin')
        assert result.returncode==0,result.stdout
        tokens=['SystemZN selftest: PASS','RaizingZN SPU DMA fixture: PASS','bank=0 bank_writes=3 faults=0 media=SYNTHETIC_FIXTURE',
                'commands=1 IRQ2_requests=1 IRQ2_ack=1 pending=0 IRQ2=0']
        assert all(token in result.stdout for token in tokens),result.stdout
        actual=struct.unpack_from('<8I',(root/'ram.bin').read_bytes(),0x100)
        expected=(0x69,0x87654321,0x87654321,0xaabbccdd,0x13579bdf,0,0x12345678,5)
        assert actual==expected,(actual,expected,result.stdout)
        assert struct.unpack_from('<2I',(root/'ram.bin').read_bytes(),0x120)==(0x44,0x77),result.stdout
        (root/'board.ini').write_text((root/'board.ini').read_text().replace('mame_set=bldyror2\n','mame_set=beastrzrb\n'))
        rejected=run(a.harness,root,True)
        assert rejected.returncode!=0 and 'invalid or unsupported Sony ZN profile' in rejected.stdout,rejected.stdout
        print(result.stdout,end='');print('Raizing native MIPS: PASS bank8MiB=1 ZNSECSEL=low2 executed_banks=1 aliases=1 Tecmo_selector_ignored=1 last_bank=1 ROM_readonly=1 sound_latch_IRQ2=1 normal_loader_rejects_synthetic=1')

if __name__=='__main__':main()
