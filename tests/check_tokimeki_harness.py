#!/usr/bin/env python3
"""Native MIPS source pulse packets and capture of actual GPU-rendered video."""
import argparse,struct,subprocess,tempfile
from pathlib import Path

def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--harness',type=Path,required=True);a=p.parse_args()
 with tempfile.TemporaryDirectory(prefix='pcsx-tok-') as td:
  r=Path(td);(r/'disc.bin').write_bytes(bytes(8*2048));(r/'disc.cue').write_text('FILE "disc.bin" BINARY\n  TRACK 01 MODE1/2048\n    INDEX 01 00:00:00\n')
  subprocess.run(['chdman','createcd','-i',str(r/'disc.cue'),'-o',str(r/'disc.chd'),'-c','none'],check=True,capture_output=True)
  run=subprocess.run([str(a.harness),'--system-gv-tokimeki-fixture',str(r/'disc.chd'),'--frames','40','--max-blocks','10000000','--printer-dir',str(r),'--dump-ram',str(r/'ram.bin'),'--output',str(r/'out.ppm')],capture_output=True,text=True,timeout=40)
  out=run.stdout+run.stderr;assert run.returncode==0,out
  b=(r/'ram.bin').read_bytes();bits=struct.unpack_from('<16I',b,0x100);assert bits==tuple(0x400 if 0xf073>>(15-i)&1 else 0 for i in range(16)),(bits,out)
  assert struct.unpack_from('<2I',b,0x140)==(0x40,0x544f4b50),out
  page=(r/'print-0001.ppm').read_bytes();parts=page.split(b'\n',3);assert parts[:3]==[b'P6',b'800 600',b'255'];rgb=parts[3];assert len(rgb)==800*600*3
  assert rgb[:3]==b'\xff\xff\xff';center=(300*800+400)*3;assert rgb[center:center+3]==b'\xff\0\0',(rgb[center:center+3],out)
  assert 'captures=1 prints=1' in out and 'rejected=0' in out and 'faults=0' in out,out
  print(out,end='');print('Tokimeki native MIPS identity GPIO, CPU-timed validated printer packets, real red GPU frame, next-vblank 800x600 page and busy response PASS')
if __name__=='__main__':main()
