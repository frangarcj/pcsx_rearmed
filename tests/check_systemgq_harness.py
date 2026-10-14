#!/usr/bin/env python3
"""Run source GQ buses and CHD SCSI READ/WRITE through native MIPS fixtures."""
import argparse,json,struct,subprocess,tempfile,sys,wave
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
import prepare_konamigq_board as prep

def profile_ini(p):
 values={'format':'pcsx-aow-konamigq-v1','mame_set':'cryptklr','mame_parent':'none','mame_machine':p['machine'],'mame_input':p['input'],'mame_state':p['state']}
 for k in ('cpu_device','cpu_clock_hz','main_ram_size','gpu_device','gpu_model','gpu_clock_hz','vram_size','sound_cpu_device','sound_cpu_clock_hz','effects_device','effects_clock_hz','scsi_device','scsi_clock_hz','scsi_initiator_id','scsi_target_id','scsi_target_device','eeprom_device','duart_device','pcm_device','pcm_device_count','pcm_clock_hz','pcm_ram_base','pcm_ram_size'):values[k]=p[k]
 values.update(eeprom_default='default_eeprom.bin',eeprom_default_only='true',game_execution_verified='false')
 for row in p['rom_regions']:values['region.'+row['tag']]=prep.REGION_FILES[row['tag']];values['region.'+row['tag']+'.size']=row['size']
 values.update({'disk.harddisk':'420uaa04.chd','disk.harddisk.sha1':p['disk_media'][0]['sha1'],'disk.harddisk.readonly':'false'})
 return ''.join(f'{k}={v}\n' for k,v in values.items())

def guest(read_lba=3,write=True):
 c=[]
 def emit(*v):c.extend(v)
 def reg(n,v):emit(0x3c000000|n<<16|((v>>16)&65535),0x34000000|n<<21|n<<16|(v&65535))
 def imm(n,v):emit(0x24000000|n<<16|(v&65535))
 def store(n,b,o,w=4):emit({1:0xa0000000,2:0xa4000000,4:0xac000000}[w]|b<<21|n<<16|(o&65535))
 def load(n,b,o,w=4):emit({1:0x90000000,2:0x94000000,4:0x8c000000}[w]|b<<21|n<<16|(o&65535),0)
 def save(b,o,d,w=4):load(9,b,o,w);store(9,10,d)
 def ncr(i,v):imm(9,v);store(9,8,2*i,1)
 def wait_irq():load(9,8,8,1);emit(0x31290080,0x1120fffc,0)
 def settle():imm(12,200);emit(0x258cffff,0x1580fffe,0)
 def disk_cmd(write=False,lba=3):
  ncr(3,1) # flush FIFO
  for v in ((0x0a if write else 0x08),0,0,lba,1,0):ncr(2,v)
  ncr(8,7);ncr(4,0);ncr(3,0x41);wait_irq();load(9,8,10,1);settle()
  ncr(11,8);ncr(0,0);ncr(1,2);ncr(14,0)
  reg(9,0x80000200);store(9,11,0x10d0);reg(9,0x00010080);store(9,11,0x10d4)
  reg(9,0x01000201 if write else 0x01000200);store(9,11,0x10d8);ncr(3,0x90)
  wait_irq();load(9,8,10,1);ncr(3,0x11);wait_irq();load(9,8,10,1)
  load(9,8,4,1);load(9,8,4,1);ncr(3,0x12);wait_irq();load(9,8,10,1);settle()
 reg(10,0x80000000);reg(11,0xbf800000);reg(8,0x1f000000)
 for a,d in [(0x1f200000,0x100),(0x1f208000,0x104),(0x1f230000,0x108),(0x1f230004,0x10c),(0x1f238000,0x110)]:reg(13,a);save(13,0,d)
 reg(13,0x1f300000);reg(9,0x12345678);store(9,13,0);save(13,0,0x114)
 reg(9,0x00800000);store(9,11,0x10f0)
 disk_cmd(False,read_lba);save(11,0x1070,0x118);save(11,0x10d8,0x11c)
 if write:disk_cmd(True,4)
 # Source I_STAT acknowledgement retains only enabled pending bits.
 imm(9,0);store(9,11,0x1074);reg(9,0xffffffff);store(9,11,0x1070);save(11,0x1070,0x124)
 # Release the real MC68000 to program the source PCM chips through its bus.
 reg(13,0x1f180000);imm(9,0x40);store(9,13,0,2)
 reg(9,0x4751444d);store(9,10,0x120);emit(0x1000ffff,0)
 bios=bytearray(524288)
 for i,v in enumerate(c):struct.pack_into('<I',bios,i*4,v)
 return bios

def sound_guest(chips):
 """Synthetic MC68000 program, accepted solely by the explicit fixture loader."""
 b=bytearray(524288);struct.pack_into('>II',b,0,0x0010fff0,0x100)
 program=bytearray.fromhex('46fc2700') # mask interrupts for this finite PCM fixture
 registers={0:0,1:0,2:1,3:0,5:0x18,8:0,9:0,10:0,0x200:0,0x201:0,0x22f:0x21,0x214:1}
 for register,value in registers.items():
  for chip in range(2):
   data=0 if register==0x214 and chip not in chips else value
   program+=struct.pack('>HHI',0x13fc,data,0x200000+register*2+chip)
 program+=bytes.fromhex('60fe');b[0x100:0x100+len(program)]=program
 return b

def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--harness',type=Path,required=True);a=p.parse_args()
 with tempfile.TemporaryDirectory(prefix='pcsx-gq-') as td:
  r=Path(td);profile=prep.load_registry(prep.boards.DEFAULT_OUTPUT)['profiles'][0]
  (r/'board.ini').write_text(profile_ini(profile));(r/'bios.bin').write_bytes(guest())
  (r/'soundcpu.bin').write_bytes(sound_guest([0]))
  (r/'pcmrom.bin').write_bytes(bytes([0x20])*524288);(r/'default_eeprom.bin').write_bytes(prep._default_eeprom(profile))
  raw=bytes((lba*31+i*7+3)&255 for lba in range(16) for i in range(512));(r/'disk.raw').write_bytes(raw)
  subprocess.run(['chdman','createhd','-i',str(r/'disk.raw'),'-o',str(r/'420uaa04.chd'),'-chs','1,1,16','-c','zlib'],check=True,capture_output=True,text=True)
  (r/'inputs.csv').write_text('frame,control,pressed\n0,p1gunx,443\n0,p1guny,239\n0,p3button1,1\n')
  def run(option='--system-gq-fixture',test=False):
   cmd=[str(a.harness),option,str(r),'--frames','5','--dump-ram',str(r/'ram.bin'),'--output',str(r/'out.ppm'),'--nvram',str(r/'nv.bin'),'--audio',str(r/'audio.wav')]
   if option.endswith('fixture'):cmd += ['--inputs',str(r/'inputs.csv')]
   return subprocess.run(cmd,capture_output=True,text=True,timeout=45)
  bad=run('--system-gq');assert bad.returncode and 'source SHA1 mismatch' in bad.stderr,bad.stderr
  first=run();out=first.stdout+first.stderr;assert first.returncode==0,out
  b=(r/'ram.bin').read_bytes();v=struct.unpack_from('<9I',b,0x100)
  assert v[:6]==(443,239,0x00ff00ff,0x00ff00f7,0x000100fd,0x00340078),(v,out)
  assert v[6]&0x400 and not(v[7]&0x11000000) and v[8]==0x4751444d,(v,out)
  assert struct.unpack_from('<I',b,0x124)[0]==0,out
  assert b[0x200:0x400]==raw[3*512:4*512],out
  assert 'SystemGQ selftest: PASS' in out and 'errors=0' in out and 'faults=0/00000000' in out,out
  assert (r/'420uaa04.overlay').exists() and len((r/'nv.bin').read_bytes())==128,out
  def audio():
   with wave.open(str(r/'audio.wav'),'rb') as w:
    assert (w.getnchannels(),w.getsampwidth(),w.getframerate())==(2,2,48000)
    samples=struct.unpack('<'+'h'*w.getnframes()*2,w.readframes(w.getnframes()))
    assert any(samples) and 'queued=0/0' in out
    return samples
  mono=audio()
  # Restart mounts the explicit mutable overlay while the source CHD stays intact.
  (r/'bios.bin').write_bytes(guest(4,False));second=run();out2=second.stdout+second.stderr;assert second.returncode==0,out2
  assert (r/'ram.bin').read_bytes()[0x200:0x400]==raw[3*512:4*512],out2
  # Independently execute each source byte lane. With a constant sample and
  # equal pan/volume, settled dual-chip output must be their exact gain-1 sum.
  (r/'bios.bin').write_bytes(guest())
  (r/'soundcpu.bin').write_bytes(sound_guest([1]));only_second=run();assert only_second.returncode==0,only_second.stderr
  chip1=audio();assert mono[-1024:]==chip1[-1024:],(mono[-16:],chip1[-16:],only_second.stdout,only_second.stderr)
  (r/'soundcpu.bin').write_bytes(sound_guest([0,1]));dual=run();assert dual.returncode==0,dual.stderr
  both=audio();assert both[-1024:]==tuple(x+y for x,y in zip(mono[-1024:],chip1[-1024:]))
  # Duplicate/unknown/wrong clock fields must fail even the fixture loader.
  ini=(r/'board.ini').read_text()
  for text in [ini+'cpu_clock_hz=67737600\n',ini+'unknown=1\n',ini.replace('gpu_model=1','gpu_model=2')]:
   (r/'board.ini').write_text(text);bad=run();assert bad.returncode and 'unsupported source board.ini' in bad.stderr,bad.stderr
  print(out,end='');print(out2,end='');print('GQ native MIPS buses, gun bit8, source lanes, IRQ10/ack, CHD READ/WRITE DMA5, persisted overlay/EEPROM, real MC68000 dual K054539 stereo 48kHz gain-1 output and authentic SHA/profile rejection PASS')
if __name__=='__main__':main()
