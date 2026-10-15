#!/usr/bin/env python3
"""Verify original KAA installation against independently programmed MAME NVRAM."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import zlib

CHIPS = ['31m','27m','31l','27l','31j','27j','31h','27h']
NV_SIZE = 16851116
INSTALL_INPUTS = 'frame,control,pressed\n0,boot_cd,1\n2400,test,1\n2460,test,0\n'


def nvram(path):
    data=path.read_bytes()
    assert len(data)==NV_SIZE and data[:16]==b'S573NV3\0PF4KAA\0\0',path
    assert struct.unpack_from('<II',data,16)==(NV_SIZE,zlib.crc32(data[32:])),path
    assert data[24:32]==bytes(8),path
    return data


def diagnostic(log,prefix):
    return next(line for line in log.splitlines() if line.startswith(prefix))


def verify(root,reference):
    assert (root/'cold-update.csv').read_text()==INSTALL_INPUTS
    result={};images={};rams={}
    for gpu in ['neon','gl']:
        log=(root/f'install-{gpu}.log').read_text()
        assert 'cpu=interpreter frames=14400' in log and 'Arcade inputs: consumed=3/3' in log
        assert 'execution stopped' not in log and 'execution limit reached' not in log
        board=diagnostic(log,'System573:')
        assert all(token in board for token in ['profile=pcnfrk4mk','faults=0',
            'media=SOURCE_CRC_VERIFIED','cassette_seed=SOURCE_DUMP',
            'CDROM=SOURCE_BAD_DUMP','NVRAM=SOURCE_SEED']),board
        assert 'seven original chip CRCs and original CD SHA matched' in log
        digital=diagnostic(log,'System573 digital:')
        assert all(token in digital for token in ['faults=0','clock_faults=0','nonzero=0']),digital
        cd=diagnostic(log,'System573 CD:')
        assert 'faults=0' in cd and 'packets=547 sectors=8426' in cd and 'DMA_transfers=8426 DMA_words=4314112' in cd,cd
        host=diagnostic(log,'host=');cycles=int(re.search(r'cycles=(\d+)',host)[1])
        assert cycles>2**32
        image=nvram(root/f'install-{gpu}.nv');images[gpu]=image
        chips={}
        for i,name in enumerate(CHIPS):
            actual=image[32+i*0x200000:32+(i+1)*0x200000]
            expected=(reference/f'29f016a.{name}').read_bytes()
            assert len(expected)==0x200000 and actual==expected,(gpu,name)
            # This original installation leaves both J chips erased; their
            # exact FF contents also match the independent MAME result.
            assert (actual==b'\xff'*len(actual))==(name in ['31j','27j']),(gpu,name)
            chips[name]=hashlib.sha256(actual).hexdigest()
        offset=32+0x1002000
        for name,size in [('cassette_game_eeprom',140),('ata_0_cr589',65536)]:
            actual=image[offset:offset+size];expected=(reference/name).read_bytes()
            assert len(expected)==size and actual==expected,(gpu,name)
            chips[name]=hashlib.sha256(actual).hexdigest();offset+=size
        drive=image[-65536:]
        assert drive[0x3ab:0x3ab+28]==b'MATSHITACD-ROM CR-589   gscb'
        ram=(root/f'install-{gpu}.ram').read_bytes();assert len(ram)==4194304;rams[gpu]=ram
        result[gpu]={'cycles':cycles,'programmed_sha256':chips,
                     'ram_sha256':hashlib.sha256(ram).hexdigest(),
                     'nvram_sha256':hashlib.sha256(image).hexdigest()}
    capture=diagnostic((root/'install-gl.log').read_text(),'capture=')
    assert any(source in capture for source in ['source=OpenGL-back-buffer-before-swap',
        'source=OpenGL-front-buffer-last-presentation']),capture
    # Calendar fields are seeded from host time on cold start; preserve any
    # differences in these whole-machine artifacts rather than masking bytes.
    result['whole_ram_equal']=rams['neon']==rams['gl']
    result['whole_nvram_equal']=images['neon']==images['gl']
    return result


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--evidence-dir',required=True,type=Path)
    p.add_argument('--reference-nvram',type=Path,help='independent MAME installation directory')
    args=p.parse_args();root=args.evidence_dir.resolve()
    reference=args.reference_nvram or root/'mame-install/nvram/pcnfrk4mk'
    print(json.dumps(verify(root,reference),indent=2))
    print('Original KAA native cold install, eight flashes, ZS01 and updated CR-589 match independent MAME PASS')


if __name__=='__main__': main()
