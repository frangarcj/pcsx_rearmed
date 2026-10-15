#!/usr/bin/env python3
"""Generated native MIPS digital-board checks; never original game evidence."""
import argparse
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import zlib
import hashlib
import re
import shutil
import wave

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from prepare_system573_digital_board import configuration


def encrypted_words(data):
    a,b,c=0xc3d2,0xab89,0xe7
    def permute(v,positions):
        out=0
        for bit in positions: out=(out<<1)|((v>>bit)&1)
        return out
    result=[]
    padded=data+bytes(3)
    for i in range(0,len(data)+2,2):
        key=permute(a^b,[15,13,14,12,11,10,9,7,8,6,5,4,3,1,2,0])
        value=(padded[i]<<8)|padded[i+1]
        value^=permute(c,[7,0,6,1,5,2,4,3,3,4,2,5,1,6,0,7])
        value^=key&0x5555
        for pair in range(8):
            shift=pair*2
            if key&(2<<shift):
                low=(value>>shift)&1; high=(value>>(shift+1))&1
                value=(value&~(3<<shift))|(low<<(shift+1))|(high<<shift)
        result.append(value)
        if ((a>>14)^(a>>15))&1: b=((b<<1)|(b>>15))&65535
        a=(a&0x8000)|((a<<1)&0x7ffe)|((a>>14)&1);c=(c+1)&255
    return result


def guest(mpeg=None):
    words = []
    def li(reg, value):
        words.extend([0x3c000000 | reg << 16 | (value >> 16),
                      0x34000000 | reg << 21 | reg << 16 | (value & 65535)])
    def store(value, address, width=4):
        li(8, address); li(9, value)
        words.append({1: 0xa0000000, 2: 0xa4000000, 4: 0xac000000}[width] | 8 << 21 | 9 << 16)
    def read(address, slot, width=4):
        li(8, address)
        words.extend([{1: 0x90000000, 2: 0x94000000, 4: 0x8c000000}[width] | 8 << 21 | 9 << 16,
                      0, 0xac000000 | 10 << 21 | 9 << 16 | (0x100 + slot * 4)])
    li(10, 0x80000000)
    read(0xbf640080, 0, 2); read(0xbf640081, 1, 1); read(0xbf6400f6, 2, 2)
    read(0xbf640080, 10); read(0xbf640083, 11, 1) # Source-zero unmapped neighbouring lanes.
    store(0, 0xbf6400b0, 2); store(0xabcd, 0xbf6400b2, 2)
    store(0x12, 0xbf6400b3, 1); store(0x5566, 0xbf6400b4, 2)
    store(0, 0xbf6400b6, 2); store(0x1200, 0xbf6400b8, 2); read(0xbf6400b4, 3, 2)
    store(0x12340000, 0xbf6400a0); read(0xbf6400a0, 4)
    read(0xbf400008, 5)
    # Let the actual source ATA power-on diagnostic timer expire; while BSY,
    # reads of signature registers intentionally return the busy status.
    li(11, 150000); words.extend([0x256bffff, 0x1560fffe, 0])
    read(0xbf480008, 6)
    store(0x10, 0xbf6400ef, 1); read(0xbf6400ef, 7, 1)
    store(0, 0xbf6400ef, 1); read(0xbf6400ef, 8, 1)
    store(0, 0xbf500000, 2); store(8, 0xbf6a0000, 1)
    for bit in range(32):
        store(10, 0xbf6a0000, 1); store(8, 0xbf6a0000, 1)
        store(9, 0xbf6a0000, 1) # ZI ignores D0 even when opposite to control SDA.
        read(0xbf400004, 16 + bit)
    store(0xf000, 0xbf6400e2, 2); store(0, 0xbf6400e2, 2)
    store(0xaabbccdd, 0xbf620000)
    for value, address in [(0xaa, 0xbf000aaa), (0x55, 0xbf000554), (0xa0, 0xbf000aaa), (0, 0xbf000200)]:
        store(value, address, 1)
    store(0x5046344b, 0x80000124)
    if mpeg:
        data=encrypted_words(mpeg)
        store(0,0xbf6400b0,2);store(0x1000,0xbf6400b2,2)
        for word in data: store(word,0xbf6400b4,2)
        end=0x1000+len(data)*2
        for address,value in [(0xa0,0),(0xa2,0x1000),(0xa4,end>>16),(0xa6,end&65535),
                              (0xa8,0xc3d2),(0xea,0xab89),(0xec,0xe7),(0xae,0xe000)]:
            store(value,0xbf640000+address,2)
    words.extend([0x1000ffff, 0])
    return struct.pack('<' + 'I' * len(words), *words)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--harness', required=True, type=Path)
    p.add_argument('--reject-cd', action='store_true')
    p.add_argument('--original-media', type=Path, help='optional audited original seven-chip directory')
    p.add_argument('--wrong-cd', type=Path, help='optional actual other-game CHD for identity rejection')
    p.add_argument('--mpeg', type=Path, help='explicit synthetic MPEG fixture, encrypted and loaded by MIPS')
    p.add_argument('--evidence-dir', type=Path)
    p.add_argument('--live', action='store_true', help='bounded SDL test of the explicitly synthetic digital fixture')
    args = p.parse_args(); binary = args.harness.resolve()
    if args.live and not args.mpeg: p.error('--live requires an explicit synthetic --mpeg fixture')
    with tempfile.TemporaryDirectory(prefix='pcsx-system573-digital-fixture-') as temp:
        root = Path(temp)
        ini = ''.join(f'{k}={v}\n' for k,v in configuration().items())
        (root / 'board.ini').write_text(ini)
        program = guest(args.mpeg.read_bytes() if args.mpeg else None)
        assert len(program)<=524288, 'synthetic MPEG fixture does not fit generated BIOS'
        (root / 'main.bin').write_bytes(program + bytes(524288 - len(program)))
        (root / 'h8_response.bin').write_bytes(bytes(range(64)))
        cassette = bytes.fromhex('5a530001ed68504bc644483e') + bytes(128)
        assert len(cassette) == 140
        (root / 'cassette.bin').write_bytes(cassette)
        (root / 'serial.bin').write_bytes(bytes.fromhex('9b22768afe451101'))
        (root / 'digital_id.bin').write_bytes(bytes.fromhex('317ce409a50b6a01'))
        (root / 'msu_flash.bin').write_bytes(bytes(524288))
        (root / 'msu_timekeeper.bin').write_bytes(bytes(8192))
        controls = [f'p1button{i}' for i in range(1,7)] + ['p1left', 'p1right', 'p1start', 'boot_cd']
        (root / 'inputs.csv').write_text('frame,control,pressed\n' + ''.join(f'{frame},{name},{pressed}\n'
            for frame,pressed in [(0,1),(1,0)] for name in controls))
        state = root / 'state.nv'
        def run(selector='--system573-digital-fixture', extra=()):
            audio=['--mixed-audio',str(root/'speaker.wav')] if args.mpeg else []
            if args.live: audio.append('--interactive')
            return subprocess.run([str(binary), selector, str(root), '--frames', '120' if args.mpeg else '2', '--board-test',
                '--inputs', str(root / 'inputs.csv'), '--nvram', str(state), '--dump-ram', str(root / 'ram.bin'),
                '--output', str(root / 'out.ppm'), *audio,*extra], capture_output=True, text=True, timeout=30)
        result = run(); output = result.stdout + result.stderr
        if args.reject_cd:
            assert result.returncode and 'requires HAVE_CHD' in output, output
            print('System573 digital profile fails closed without HAVE_CHD PASS'); return
        assert not result.returncode, output
        data = (root / 'ram.bin').read_bytes()
        assert struct.unpack_from('<9I', data, 0x100) == (0x1234,0x12,0xb000,0x5566,0x12340000,0x20fc,0x00eb0014,0,0x10), (struct.unpack_from('<9I', data, 0x100), output)
        assert struct.unpack_from('<I', data, 0x124)[0] == 0x5046344b
        assert struct.unpack_from('<2I', data, 0x128) == (0x1234,0)
        bits = [(struct.unpack_from('<I', data, 0x140 + i*4)[0] >> 18) & 1 for i in range(32)]
        value = 0
        for bit in bits: value = (value << 1) | bit
        assert value == 0x5a530001, hex(value)
        for token in ['digital drum/select/input selftest PASS', 'media=SYNTHETIC_FIXTURE',
                      'CDROM=EMPTY_FIXTURE_DRIVE', 'consumed=20/20', 'faults=0', 'drums=1111000000']:
            assert token in output, (token, output)
        cold_audio=None
        if args.mpeg:
            assert int(re.search(r'System573 digital:.*? frames=(\d+)',output)[1])>3,output
            assert 'clock_faults=0' in output,output
            pending=int(re.search(r'pending_SPU=(\d+) faults=0',output)[1])
            assert pending<=8,output # Real resampler lookahead/even-pair SPU tail, never fabricated future PCM.
            cold_audio=(root/'speaker.wav').read_bytes()
            with wave.open(str(root/'speaker.wav'),'rb') as wav:
                assert wav.getframerate()==44100 and wav.getnchannels()==2 and wav.getsampwidth()==2
                values=struct.unpack('<'+'h'*(wav.getnframes()*2),wav.readframes(wav.getnframes()))
                assert max(map(abs,values))>2000
                if args.live:
                    live=re.search(r'Interactive System573 speaker: frames=(\d+) nonzero=(\d+) peak=(\d+) error=(\d+)',output)
                    assert live and int(live[1])==wav.getnframes() and int(live[2])>0 and int(live[3])>2000 and int(live[4])==0,output
            if args.evidence_dir:
                args.evidence_dir.mkdir(parents=True,exist_ok=True)
                shutil.copyfile(root/'speaker.wav',args.evidence_dir/'speaker-cold.wav')
                (args.evidence_dir/'cold.log').write_text(output)
        snapshot = state.read_bytes()
        assert len(snapshot) == 32 + 0x1000000 + 8192 + 140 + 65536
        assert snapshot[:16] == b'S573NV3\0PF4KAA\0\0'
        assert struct.unpack_from('<II', snapshot, 16) == (len(snapshot), zlib.crc32(snapshot[32:]))
        assert snapshot[32+0x100] == 0 and snapshot[32+0x200000+0x100] == 255
        assert snapshot[32+0x1000000:32+0x1000000+2] == b'\xdd\xbb'
        assert snapshot[32+0x1002000:32+0x1002000+140] == cassette
        # Native persistence accepts legitimate mutable flash, EEPROM and drive-buffer state.
        updated = bytearray(snapshot)
        for offset in [32+0x2222, 32+0x1002000+30, 32+0x1002000+140+0xeeee]: updated[offset] ^= 1
        struct.pack_into('<I', updated, 20, zlib.crc32(updated[32:]))
        state.write_bytes(updated); warm = run()
        assert not warm.returncode and 'NVRAM=RESTORED' in warm.stderr, warm.stderr
        restored = state.read_bytes()
        for offset in [32+0x2222, 32+0x1002000+30, 32+0x1002000+140+0xeeee]: assert restored[offset] == updated[offset]
        if args.mpeg:
            warm_audio=(root/'speaker.wav').read_bytes();assert warm_audio==cold_audio
            print('Synthetic native digital speaker cold/warm WAV SHA256 '+hashlib.sha256(cold_audio).hexdigest()+' PASS')
            if args.evidence_dir:
                shutil.copyfile(root/'speaker.wav',args.evidence_dir/'speaker-warm.wav')
                (args.evidence_dir/'warm.log').write_text(warm.stdout+warm.stderr)
        if args.live:
            print(output,end='');print('Synthetic native digital live SDL speaker test PASS');return
        original = run('--system573')
        assert original.returncode and 'original main.bin size/CRC mismatch' in original.stderr
        assert run('--system573-fixture').returncode # The base fixture selector cannot open a digital profile.
        for bad in [snapshot[:-1], b'X'+snapshot[1:], snapshot[:24]+b'X'+snapshot[25:], snapshot[:100]+b'X'+snapshot[101:]]:
            state.write_bytes(bad); assert run().returncode
        state.write_bytes(snapshot)
        for bad in [ini+'cassette=ZI\n', ini.replace('cassette=ZI','cassette=X'), ini.replace('mame_parent=pcnfrk4m','mame_parent=drmn4m')]:
            (root / 'board.ini').write_text(bad); rejected = run()
            assert rejected.returncode and 'invalid or unsupported original profile' in rejected.stderr
        if args.original_media:
            source = root / 'original'; source.mkdir(); (source / 'board.ini').write_text(ini)
            filenames = ['main.bin','h8_response.bin','cassette.bin','serial.bin','digital_id.bin','msu_flash.bin','msu_timekeeper.bin']
            for name in filenames: (source / name).symlink_to((args.original_media / name).resolve())
            def normal():
                r = subprocess.run([str(binary),'--system573',str(source),'--frames','2','--output',str(root/'original.ppm')],
                                   capture_output=True,text=True,timeout=30)
                assert r.returncode
                return r.stdout+r.stderr
            assert 'System573 CD:' in normal() # All original chips pass before the missing-CD error.
            for name in filenames:
                target = source / name; data = bytearray(target.read_bytes()); target.unlink(); data[-1] ^= 1; target.write_bytes(data)
                assert f'original {name} size/CRC mismatch' in normal()
                target.unlink(); target.symlink_to((args.original_media / name).resolve())
            if args.wrong_cd:
                (source / 'a25kaa02.chd').symlink_to(args.wrong_cd.resolve())
                assert 'original CD SHA mismatch' in normal()
            print('System573 digital original seven-chip CRC checks and missing/wrong-CD rejection PASS')
        print(output, end='')
        print('System573 digital native MIPS registers/lanes/RAM, ZI reset, drum inputs, source lamps, empty CR589, complete NV3 persistence and strict original-entry rejection PASS')


if __name__ == '__main__': main()
