#!/usr/bin/env python3
"""Execute Primal IDE/latch/IRQ/DMA5 and upper RAM through the native MIPS core."""
import argparse
from pathlib import Path
import struct
import subprocess
import tempfile
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
import prepare_primal_zn_board as primal
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--harness',required=True,type=Path);a=p.parse_args()
    with tempfile.TemporaryDirectory(prefix='pcsx-primal-') as td:
        r=Path(td);config=dict(zip(primal.KEYS,('pcsx-aow-primal-zn-v1','primrag2','coh1000w','coh1000w','fixed','COH1000W','PRIMAL','CXD8530CQ','67737600','CXD8561Q','53693175','8388608','2097152','false')))
        (r/'board.ini').write_text(''.join(f'{k}={v}\n' for k,v in config.items()))
        code=[]
        def emit(*v):code.extend(v)
        def lui(reg,v):emit(0x3c000000|reg<<16|v)
        def imm(reg,v):emit(0x24000000|reg<<16|(v&65535))
        def store(reg,base,off,width=4):emit({1:0xa0000000,2:0xa4000000,4:0xac000000}[width]|base<<21|reg<<16|(off&65535))
        def load(reg,base,off,width=4):emit({1:0x90000000,2:0x94000000,4:0x8c000000}[width]|base<<21|reg<<16|(off&65535),0)
        def save(base,off,dest,width=4):load(9,base,off,width);store(9,10,dest)
        def reg(n,v):imm(9,v);store(9,8,0x41f0+n,1)
        def drq():load(9,8,0x41f7,1);emit(0x31290008,0x1120fffc,0)
        lui(10,0x8000);lui(8,0x1f7e);imm(9,1)
        # The control lives at +8000; MIPS signed immediates require base 1f7f.
        lui(11,0x1f7f);store(9,11,0x8000,2)
        load(9,8,0x41f7,1);emit(0x31290080,0x1520fffc,0) # source HDD power-on reset must finish
        reg(6,0xe0);reg(7,0xec);drq()
        lui(11,0x1f80);save(11,0x1070,0x100);lui(11,0x1f7f)
        save(11,0x41f0,0x104);save(11,0x41f2,0x108,2);save(8,0x41f0,0x10c,2)
        imm(12,253);load(9,8,0x41f0,2);emit(0x258cffff,0x1580fffc,0)
        for n,v in ((2,1),(3,3),(4,0),(5,0),(6,0xe0),(7,0x20)):reg(n,v)
        drq();lui(11,0x1f80);lui(9,0x0080);store(9,11,0x10f0)
        lui(9,0x00c0);store(9,11,0x10d0);lui(9,1);emit(0x35290080);store(9,11,0x10d4)
        lui(9,0x0100);emit(0x35290200);store(9,11,0x10d8)
        save(11,0x10d8,0x110);save(11,0x10d0,0x114);save(8,0x41f7,0x118,1)
        lui(11,0xbf00);imm(9,0x5566);store(9,11,8);save(11,8,0x11c)
        lui(11,0xbf20);save(11,-4,0x120)
        lui(11,0x1f00);imm(9,1);store(9,11,0,2);emit(0x1000fffe,0)
        bios=bytearray(0x80000);struct.pack_into('<'+'I'*len(code),bios,0,*code);(r/'main.bin').write_bytes(bios)
        rom=bytearray(0x200000);struct.pack_into('<I',rom,8,0x89abcdef);struct.pack_into('<I',rom,len(rom)-4,0x13579bdf);(r/'fixed.bin').write_bytes(rom)
        (r/'eeprom_default.bin').write_bytes(bytes(2048))
        for n in ('cat702_key0.bin','cat702_key1.bin'):(r/n).write_bytes(bytes(range(8)))
        disk=bytes((i*17+3)&255 for i in range(8192));(r/'disk.raw').write_bytes(disk)
        subprocess.run(['chdman','createhd','-i',str(r/'disk.raw'),'-o',str(r/'primrag2.chd'),'-chs','1,1,16','-c','zlib'],check=True,capture_output=True,text=True)
        def run(fixture,frames=3):
            return subprocess.run([str(a.harness),'--primal-zn-fixture' if fixture else '--primal-zn',str(r),'--frames',str(frames),'--dump-ram',str(r/'ram.bin'),'--output',str(r/'frame.ppm')],capture_output=True,text=True,timeout=45)
        rejected=run(False);assert rejected.returncode!=0 and 'source ROM CRC mismatch' in rejected.stderr,rejected.stderr
        result=run(True);output=result.stdout+result.stderr;assert result.returncode==0,output
        ram=(r/'ram.bin').read_bytes();assert len(ram)==0x800000,len(ram)
        values=struct.unpack_from('<9I',ram,0x100)
        assert values[0]&0x400 and values[1:]==(0x0001045a,1,0,0x200,0xc00000,0x50,0x89abcdef,0x13579bdf),(values,output)
        assert ram[0x400000:0x400200]==disk[3*512:4*512],output
        for token in ('SystemZN selftest: PASS','PrimalZN SPU DMA fixture: PASS','faults=0 media=SYNTHETIC_FIXTURE','DMA5_reads=1 words=128','watchdog_resets=0 pending=0 VIA_diagnostics=0'):
            assert token in output,(token,output)
        print(output,end='');print('Primal native MIPS: PASS IDE32_latch=1 IRQ10=1 DMA5_payload=512 initial_address_mask8MiB=1 MADR_retained=1 RAM8MiB=1 ROM_readonly=1 original_SPU_DMA=1 normal_rejects_synthetic=1')
        # A deliberately unserviced watchdog restarts the real guest at a safe
        # boundary. Its upper-RAM boot counter must survive the source reset.
        reset_program=(0x3c088040,0x35080100,0x8d090000,0,0x25290001,0xad090000,0x1000ffff,0)
        bios=bytearray(0x80000);struct.pack_into('<8I',bios,0,*reset_program);(r/'main.bin').write_bytes(bios)
        reset=run(True,50);reset_output=reset.stdout+reset.stderr;assert reset.returncode==0,reset_output
        assert struct.unpack_from('<I',(r/'ram.bin').read_bytes(),0x400100)[0]==2,reset_output
        assert 'watchdog_resets=1 pending=0' in reset_output and 'faults=0 media=SYNTHETIC_FIXTURE' in reset_output,reset_output
        print('Primal native watchdog: PASS timeout600ms=1 safe_guest_restart=1 source_RAM_preserved=1 upper_RAM_boot_counter=2')
if __name__=='__main__':main()
