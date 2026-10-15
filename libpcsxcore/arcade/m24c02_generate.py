#!/usr/bin/env python3
"""Extract the reviewed M24C02 pin state machine from pinned MAME i2cmem."""
import argparse
import hashlib
from pathlib import Path
import re
PIN='deb2f54fe7300e7a192690318beb860159bfd52a'
HASHES={'i2cmem.cpp':'cdd4a0ba6954417abab767e994c6341f52285a17e3f9af47b3816009de2ac005',
        'i2cmem.h':'bd6bf70290db8eeb7352868356e4ca27ec302d5a5fca49bd8a4f85099ff9eede'}
def generate(root):
    for name,digest in HASHES.items():
        if hashlib.sha256((root/'src/devices/machine'/name).read_bytes()).hexdigest()!=digest:
            raise ValueError('reviewed I2C source SHA256 mismatch: '+name)
    source=(root/'src/devices/machine/i2cmem.cpp').read_text()
    constants=source[source.index('constexpr int STATE_IDLE'):source.index('//**************************************************************************\n//  DEBUGGING')]
    methods=[]
    for name in ['write_e0','write_e1','write_e2','write_sda','write_scl','write_wc','read_sda','address_mask','select_device','data_offset']:
        match=re.search(r'(?:void|int) i2cmem_device::'+name+r'\([^)]*\)',source)
        start=source.index('{',match.end());end=start+1;depth=1
        while depth:
            depth+=(source[end]=='{')-(source[end]=='}');end+=1
        methods.append(source[match.start():end].replace('i2cmem_device::','M24c02::'))
    return '// Generated from MAME '+PIN+'. BSD-3-Clause; smf.\n// Framework logging elided by host macros; pin and page state machine unchanged.\n'+constants+'\n'+'\n\n'.join(methods)+'\n'
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('source',type=Path);p.add_argument('--check',action='store_true');a=p.parse_args()
    data=generate(a.source);out=Path(__file__).with_name('m24c02_mame.inc')
    if a.check:
        if out.read_text()!=data:p.exit(1,'M24C02 generated source differs\n')
        print('M24C02 source extraction matches reviewed MAME pin')
    else:out.write_text(data)
