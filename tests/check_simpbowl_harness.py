#!/usr/bin/env python3
"""Run actual MIPS bus accesses and protected flash persistence without game media."""
import argparse,hashlib,struct,subprocess,tempfile
from pathlib import Path

def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--harness',type=Path,required=True);a=p.parse_args()
 with tempfile.TemporaryDirectory(prefix='pcsx-simp-') as td:
  r=Path(td);raw=bytes((i*7+3)&255 for i in range(8*2048));(r/'disc.bin').write_bytes(raw)
  (r/'disc.cue').write_text('FILE "disc.bin" BINARY\n  TRACK 01 MODE1/2048\n    INDEX 01 00:00:00\n')
  subprocess.run(['chdman','createcd','-i',str(r/'disc.cue'),'-o',str(r/'disc.chd'),'-c','none'],check=True,capture_output=True)
  original=hashlib.sha256((r/'disc.chd').read_bytes()).digest()
  (r/'inputs.csv').write_text('frame,control,pressed\n0,p1trackx,443\n0,p1tracky,239\n')
  def run():
   return subprocess.run([str(a.harness),'--system-gv-simpbowl-fixture',str(r/'disc.chd'),'--frames','16','--inputs',str(r/'inputs.csv'),'--dump-ram',str(r/'ram.bin'),'--output',str(r/'out.ppm')],capture_output=True,text=True,timeout=30)
  first=run();out=first.stdout+first.stderr;assert first.returncode==0,out
  b=(r/'ram.bin').read_bytes();words=struct.unpack_from('<10I',b,0x100)
  assert words==(0xffff,0x3412,0x0404,0xadad,0x0100bb00,0x0000ef00,0,0,0x53494d50,0x7856),(words,out)
  state=r/'simpbowl.fixture.flash.nvram';saved=state.read_bytes()
  assert len(saved)==64+0x800000 and saved[:16]==b'PCSX-SIMP-FLASH1'
  assert hashlib.sha1(saved[64:]).digest()==saved[36:56]
  assert [saved[64+i*0x200000+0x1234] for i in range(4)]==[0x12,0x34,0x56,0x78]
  second=run();out2=second.stdout+second.stderr;assert second.returncode==0,out2
  assert struct.unpack_from('<I',(r/'ram.bin').read_bytes(),0x100)[0]==0x3412 and 'warm=1' in out2,out2
  assert state.read_bytes()==saved and hashlib.sha256((r/'disc.chd').read_bytes()).digest()==original
  for mutated in [saved[:64],saved[:16]+bytes([saved[16]^1])+saved[17:],saved[:-1]+bytes([saved[-1]^1])]:
   state.write_bytes(mutated);bad=run();assert bad.returncode and 'invalid or wrong-profile flash.nvram' in bad.stderr,bad.stderr
   assert state.read_bytes()==mutated
  print(out,end='');print(out2,end='');print('Simpsons native MIPS four flash lanes/IDs/program, frame-sampled 12-bit trackball/reset, process-restart persistence and corrupt/profile state rejection PASS')
if __name__=='__main__':main()
