#!/usr/bin/env python3
"""Execute NBA original-map banks/SRAM/status/commands through the native MIPS core."""
import argparse
from pathlib import Path
import struct
import subprocess
import tempfile
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
import prepare_nbajam_zn_board as nba

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--harness',required=True,type=Path);a=p.parse_args()
    with tempfile.TemporaryDirectory(prefix='pcsx-nbajam-') as td:
        r=Path(td);values=('pcsx-aow-nbajam-zn-v1','nbajamex','coh1000a','coh1000a','fixed','COH1000A','NBAJAM','CXD8530CQ','67737600','CXD8561Q','53693175','4194304','2097152','false')
        (r/'board.ini').write_text(''.join(f'{k}={v}\n' for k,v in zip(nba.KEYS,values)))
        code=[]
        def emit(*v):code.extend(v)
        def lui(reg,v):emit(0x3c000000|reg<<16|v)
        def imm(reg,v):emit(0x24000000|reg<<16|(v&65535))
        def store(reg,base,off,width=4):emit({1:0xa0000000,2:0xa4000000,4:0xac000000}[width]|base<<21|reg<<16|(off&65535))
        def load(reg,base,off,width=4):emit({1:0x90000000,2:0x94000000,4:0x8c000000}[width]|base<<21|reg<<16|(off&65535),0)
        def save(base,off,dest,width=4):load(9,base,off,width);store(9,10,dest)
        lui(10,0x8000);lui(8,0x1f20);lui(11,0x1f00);lui(12,0x1fc0)
        save(8,0,0x100);save(11,0,0x104);lui(9,0x1234);emit(0x35295678);store(9,8,0)
        save(8,0,0x108);lui(13,0x1fc0);imm(9,1);store(9,13,0xff00,2)
        save(11,0,0x10c);save(8,0,0x110);imm(9,0xabcd);store(9,8,0,2)
        imm(9,0x11);store(9,13,0xff02,2);save(8,0,0x114)
        imm(9,0);store(9,13,0xff00,2);save(8,0,0x118)
        save(13,0xff08,0x11c);save(13,0xff80,0x120)
        imm(9,0x5678);store(9,13,0xff80,2);imm(14,256)
        emit(0x25ceffff,0x15c0fffe,0);save(13,0xff80,0x124)
        # Source read constants stay FFFF even after a real DSP reply.
        lui(11,0x1fa0);save(11,0x300,0x128,1);emit(0x1000ffff,0)
        bios=bytearray(0x80000);struct.pack_into('<'+'I'*len(code),bios,0,*code);(r/'main.bin').write_bytes(bios)
        (r/'banked.bin').write_bytes(b''.join(bytes([i])*0x200000 for i in range(16)))
        sound=bytearray(0x800000)
        dsp={0:0x3c0803,1:0x18001f,12:0x010030,13:0x018030,14:0x0a001f}
        for i,op in dsp.items():sound[3*i:3*i+3]=op.to_bytes(3,'big')
        (r/'rax.bin').write_bytes(sound)
        for n in ('cat702_key0.bin','cat702_key1.bin'):(r/n).write_bytes(bytes(range(8)))
        def run(fixture,reset=False):
            cmd=[str(a.harness),'--nbajam-zn-fixture' if fixture else '--nbajam-zn',str(r),'--frames','3','--dump-ram',str(r/'ram.bin'),'--output',str(r/'frame.ppm')]
            if reset:cmd+=['--reset-at-cycle','500000']
            return subprocess.run(cmd,capture_output=True,text=True,timeout=30)
        rejected=run(False);assert rejected.returncode!=0 and 'source ROM CRC mismatch' in rejected.stderr,rejected.stderr
        result=run(True);output=result.stdout+result.stderr;assert result.returncode==0,output
        data=(r/'ram.bin').read_bytes();assert len(data)==0x400000
        vals=struct.unpack_from('<11I',data,0x100)
        expected=(0xffffffff,0,0x12345678,0x02020202,0x01010101,0x02020202,0x1234abcd,0xffff0400,0xffffffff,0xffffffff,0xff)
        assert vals==expected,(vals,output)
        for token in ('SystemZN selftest: PASS','NBAJamZN SPU DMA fixture: PASS','faults=0 media=SYNTHETIC_FIXTURE','commands=1 reads=1 replies=1 output=5678'):
            assert token in output,(token,output)
        reset_code=(0x3c088000,0x35080800,0x8d090000,0,0x25290001,0xad090000,0x1000ffff,0)
        bios=bytearray(0x80000);struct.pack_into('<8I',bios,0,*reset_code);(r/'main.bin').write_bytes(bios)
        reset=run(True,True);reset_log=reset.stdout+reset.stderr;assert reset.returncode==0,reset_log
        assert struct.unpack_from('<I',(r/'ram.bin').read_bytes(),0x800)[0]==2,reset_log
        assert 'harness soft reset:' in reset_log and 'faults=0 media=SYNTHETIC_FIXTURE' in reset_log,reset_log
        print('NBA native soft reset: PASS safe_guest_restart=1 RAM4MiB_preserved=1 boot_counter=2')
        print(output,end='');print('NBA native MIPS: PASS source_banks=1 SRAM_write_mirror_ROM_read=1 SRAM_read_restored=1 sound_constants=1 actual_DSP_IRQ_reply=1 RAM4MiB=1 native_input4players=1 original_SPU_DMA=1 normal_rejects_synthetic=1')
if __name__=='__main__':main()
