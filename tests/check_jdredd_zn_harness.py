#!/usr/bin/env python3
"""Execute Judge HDD/IRQ10 and source GPU gun latches through native MIPS."""
import argparse
from pathlib import Path
import struct,subprocess,tempfile,sys
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
import prepare_jdredd_zn_board as judge

def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--harness',required=True,type=Path);a=p.parse_args()
 with tempfile.TemporaryDirectory(prefix='pcsx-judge-') as td:
  r=Path(td);values=('pcsx-aow-jdredd-zn-v1','jdredd','coh1000a','coh1000a','fixed','COH1000A','JDREDD','CXD8530CQ','67737600','CXD8561Q','53693175','4194304','2097152','false')
  (r/'board.ini').write_text(''.join(f'{k}={v}\n' for k,v in zip(judge.KEYS,values)))
  code=[]
  def emit(*v):code.extend(v)
  def lui(n,v):emit(0x3c000000|n<<16|v)
  def imm(n,v):emit(0x24000000|n<<16|(v&65535))
  def store(n,b,o,w=4):emit({1:0xa0000000,2:0xa4000000,4:0xac000000}[w]|b<<21|n<<16|(o&65535))
  def load(n,b,o,w=4):emit({1:0x90000000,2:0x94000000,4:0x8c000000}[w]|b<<21|n<<16|(o&65535),0)
  def save(b,o,d,w=4):load(9,b,o,w);store(9,10,d)
  def reg(n,v):imm(9,v);store(9,8,0xff90+2*n,1)
  def wait(bit):load(9,8,0xff9e,1);emit(0x31290000|bit,0x1120fffc if bit==8 else 0x1520fffc,0)
  lui(10,0x8000);lui(8,0x1fc0);lui(11,0x1f80);wait(0x80)
  reg(6,0xe0);reg(7,0xec);wait(8)
  save(11,0x1070,0x100);save(8,0xff8c,0x104,2);save(8,0xff90,0x108,4);save(8,0xff90,0x10c,2)
  imm(12,254);load(9,8,0xff90,2);emit(0x258cffff,0x1580fffc,0)
  for n,v in ((2,1),(3,3),(4,0),(5,0),(6,0xe0),(7,0x20)):reg(n,v)
  wait(8);lui(12,0x8000);emit(0x358c0200);imm(13,256)
  load(9,8,0xff90,2);store(9,12,0,2);emit(0x258c0002,0x25adffff,0x15a0fffa,0)
  # GP1(10h,08h) reports source raw X/Y; GP1(0dh) clears even repeated writes.
  lui(9,0x1000);emit(0x35290008);store(9,11,0x1814);save(11,0x1810,0x110)
  lui(9,0x0d00);store(9,11,0x1814);lui(9,0x1000);emit(0x35290008);store(9,11,0x1814);save(11,0x1810,0x114)
  lui(9,0x1000);emit(0x35290008)
  # Wait for a VBlank latch. Source starts with P2 mux on the first edge.
  store(9,11,0x1814);load(12,11,0x1810);emit(0x1180fffc,0);store(12,10,0x118)
  # Repeating GP1 0Dh after a new sample must not hit the register cache.
  lui(9,0x0d00);store(9,11,0x1814);lui(9,0x1000);emit(0x35290008);store(9,11,0x1814);save(11,0x1810,0x12c)
  lui(9,0x1000);emit(0x35290008);store(9,11,0x1814);load(12,11,0x1810);emit(0x1180fffc,0)
  imm(9,0);store(9,11,0x1814);lui(9,0x1000);emit(0x35290008);store(9,11,0x1814);save(11,0x1810,0x130)
  lui(12,0x1fa0);save(12,0x200,0x11c,1);lui(12,0xbf00);save(12,0,0x120)
  imm(9,0x1234);store(9,12,0);save(12,0,0x124)
  # Source I_STAT write retains only acknowledged, enabled IRQ inputs.
  imm(9,1);store(9,11,0x1074);imm(9,0xffff);store(9,11,0x1070);save(11,0x1070,0x134)
  imm(9,0x401);store(9,11,0x1074);save(11,0x1070,0x138);emit(0x1000ffff,0)
  bios=bytearray(0x80000);struct.pack_into('<'+'I'*len(code),bios,0,*code);(r/'main.bin').write_bytes(bios)
  rom=bytearray(0x200000);struct.pack_into('<I',rom,0,0x89abcdef);(r/'fixed.bin').write_bytes(rom)
  for name in ('cat702_key0.bin','cat702_key1.bin'):(r/name).write_bytes(bytes(range(8)))
  disk=bytes((i*17+3)&255 for i in range(8192));(r/'disk.raw').write_bytes(disk)
  subprocess.run(['chdman','createhd','-i',str(r/'disk.raw'),'-o',str(r/'jdreddc.chd'),'-chs','1,1,16','-c','zlib'],check=True,capture_output=True,text=True)
  def run(fixture):return subprocess.run([str(a.harness),'--jdredd-zn-fixture' if fixture else '--jdredd-zn',str(r),'--frames','3','--dump-ram',str(r/'ram.bin'),'--output',str(r/'out.ppm')],capture_output=True,text=True,timeout=30)
  bad=run(False);assert bad.returncode!=0 and 'source ROM CRC mismatch' in bad.stderr,bad.stderr
  result=run(True);output=result.stdout+result.stderr;assert result.returncode==0,output
  data=(r/'ram.bin').read_bytes();v=struct.unpack_from('<10I',data,0x100)
  assert v[0]&0x400 and v[1]&8 and v[2:]==(0x045a,1,0,0,0x01220822,0xff,0x89abcdef,0x89abcdef),(v,output)
  assert data[0x200:0x400]==disk[3*512:4*512],output
  assert struct.unpack_from('<2I',data,0x12c)==(0,0x01220822),output
  assert struct.unpack_from('<2I',data,0x134)==(1,1),output
  for token in ('SystemZN selftest: PASS','JudgeZN SPU DMA fixture: PASS','faults=0 media=SYNTHETIC_FIXTURE','errors=0','mux=1'):
   assert token in output,(token,output)
  print(output,end='');print('Judge native MIPS: PASS direct_ATA16=1 data32_one_word_plus_ERROR=1 IRQ10=1 HDD_sector_payload512=1 GPU_query_raw_XY=1 repeated_GP1_reset=1 GPU_reset_preserves_latch=1 VBlank_mux=1 active_low_mux_bit=1 source_I_STAT_ack_mask=1 ROM_readonly=1 original_SPU_DMA=1 normal_rejects_synthetic=1')
if __name__=='__main__':main()
