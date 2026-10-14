#!/usr/bin/env python3
"""Execute FX1Z ROM/FRAM/Zoom bus and combined NVRAM through native PSX MMIO."""
import argparse
import pathlib
import struct
import subprocess
import sys
import tempfile
sys.path.insert(0,str(pathlib.Path(__file__).resolve().parents[1]/'scripts'))
import prepare_taito_fx1_board as prepare
import taito_fx1_boards as profiles


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--harness',required=True,type=pathlib.Path);a=p.parse_args()
    by_set={p['set']:p for p in profiles.build_registry()['profiles']}
    with tempfile.TemporaryDirectory(prefix='pcsx-fx1z-') as temp:
        for name in ['raystorm','gdarius2']:
            root=pathlib.Path(temp)/name;profile=by_set[name]
            sizes={r['tag']:r['size'] for r in profile['rom_regions']}
            regions={tag:bytearray(sizes[tag]) for tag in profiles.FX1Z_REQUIRED_REGIONS}
            code=[]
            def emit(*words):code.extend(words)
            def lui(r,v):emit(0x3c000000|(r<<16)|v)
            def imm(r,v):emit(0x24000000|(r<<16)|(v&0xffff))
            def store(r,b,o,w=4):emit({1:0xa0000000,2:0xa4000000,4:0xac000000}[w]|(b<<21)|(r<<16)|(o&0xffff))
            def load(r,b,o,w=4):emit({1:0x90000000,2:0x94000000,4:0x8c000000}[w]|(b<<21)|(r<<16)|(o&0xffff),0)
            lui(10,0x8000);lui(8,0x1fb0);load(9,8,0);store(9,10,0x100)
            lui(9,0xaa44);emit(0x35290033);store(9,8,0)
            imm(9,0x99);store(9,8,1,1);load(9,8,0);store(9,10,0x104)
            imm(9,0x55);store(9,8,0x3fe,1);load(9,8,0x3fc);store(9,10,0x108)
            lui(8,0x1fbe);lui(9,0x0044);emit(0x35290033);store(9,8,0)
            load(9,8,0);store(9,10,0x10c);load(9,8,2,2);store(9,10,0x110)
            lui(8,0x1fba);imm(9,1);store(9,8,0) # upper open-bus word ignored
            lui(8,0x1fbc);load(9,8,0);store(9,10,0x114)
            lui(8,0x1fb4);imm(9,0x21);store(9,8,0,1);imm(9,1);store(9,8,0,1)
            lui(11,0xbf00);load(9,11,0);store(9,10,0x118)
            emit(0x356b0100,0x0160f809,0);store(5,10,0x11c)
            lui(8,0x1fa1);load(9,8,0x200);store(9,10,0x120)
            emit(0x1000ffff,0)
            struct.pack_into('<'+'I'*len(code),regions['maincpu:rom'],0,*code)
            struct.pack_into('<I',regions['bankedroms'],0x800000,0x87654321)
            struct.pack_into('<3I',regions['bankedroms'],0x800100,0x24050077,0x03e00008,0)
            regions['taito_zoom:mn10200'][:2]=b'\xea\xfe'
            prepare.package_regions(name,profile,{k:bytes(v) for k,v in regions.items()},root,{})
            base=[str(a.harness),'--taito-fx1z-fixture',str(root),'--frames','3',
                  '--output',str(root/'frame.ppm'),'--dump-ram',str(root/'ram.bin'),'--nvram',str(root/'nvram.bin')]
            normal=subprocess.run([str(a.harness),'--taito-fx1z',str(root)],capture_output=True,text=True,timeout=20)
            assert normal.returncode!=0 and 'source ROM CRC mismatch' in normal.stderr,normal.stderr
            for warm in [False,True]:
                run=subprocess.run(base,capture_output=True,text=True,timeout=45)
                assert run.returncode==0,run.stdout+run.stderr
                assert 'faults=0/0 media=SYNTHETIC_FIXTURE' in run.stderr,run.stderr
                assert 'TaitoFX1Z SPU DMA fixture: PASS' in run.stderr,run.stderr
                ram=(root/'ram.bin').read_bytes()
                vals=struct.unpack_from('<9I',ram,0x100)
                expected=(0xff44ff33 if warm else 0xffffffff,0xff44ff33,0xff55ffff,0xff44ff33,0xff44,0xffff0000,0x87654321,0x77,0x61 if name=='raystorm' else 0x69)
                assert vals==expected,(name,warm,vals,expected)
                nvram=(root/'nvram.bin').read_bytes();assert len(nvram)==2592
                assert nvram[2080:2082]==b'\x33\x44' and nvram[-1]==0x55
            print(f'FX1Z native {name}: PASS FRAM_lanes=1 shared_lanes=1 IRQ_read_source=1 ROM_bank_execute=1 VRAM={profile["vram_size"]} NV2592_roundtrip=1 SPU_DMA=1 normal_loader_rejects_synthetic=1')

if __name__=='__main__':main()
