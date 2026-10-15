#!/usr/bin/env python3
"""Extract the reviewed RTC65271 register/calendar methods from pinned MAME."""
import argparse
import hashlib
from pathlib import Path
import re

PIN = 'deb2f54fe7300e7a192690318beb860159bfd52a'
HASHES = {'rtc65271.cpp': '58ae9dbaa21bf1494933907a6534cf66e3b8a68a409aa66fff89bdd4c3f7cb28',
          'rtc65271.h': '60a7e23138687cfee5a06c4006485bc99efc0172eaddd24b0e1f2cf2d5f5c91e'}


def function(text, name):
    match = re.search(r'(?:void|int|uint8_t) rtc65271_device::'+name+r'\([^)]*\)|'
                      r'TIMER_CALLBACK_MEMBER\(rtc65271_device::'+name+r'\)', text)
    if not match:
        raise ValueError('missing source method '+name)
    start = text.index('{', match.end()); depth = 1; end = start+1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}');end += 1
    return text[match.start():end]


def generate(source):
    for name, expected in HASHES.items():
        path = source / 'src/devices/machine' / name
        if hashlib.sha256(path.read_bytes()).hexdigest() != expected:
            raise ValueError('reviewed RTC source SHA256 mismatch: '+name)
    text = (source / 'src/devices/machine/rtc65271.cpp').read_text()
    constants = text[text.index('enum\n{'):text.index('/*\n    Public functions')]
    names = ['rtc_clock_updated','read','write','field_interrupts','intrq_r',
             'rtc_SQW_cb','rtc_begin_update_cb','rtc_end_update_cb']
    methods = '\n\n'.join(function(text, n) for n in names)
    methods = re.sub(r'TIMER_CALLBACK_MEMBER\(rtc65271_device::(\w+)\)',
                     r'void rtc65271_device::\1(int32_t)', methods)
    # Defined host failure at the two source table accesses for an invalid
    # guest-programmed month; do not clamp or invent a replacement date.
    for index in ['m_regs[reg_month] - 1', 'BCD_to_binary(m_regs[reg_month]) - 1']:
        old = 'days_in_month_table['+index+']'
        assert methods.count(old) == 1
        marker = 'days_in_month = ' if index.startswith('m_regs') else 'days_in_month = binary_to_BCD('
        target = marker+old
        replacement = ('const int month_index = '+index+';\n'
                       '\t\t\t\t\t\tif (month_index < 0 || month_index >= 12) { faults |= 4; return; }\n'
                       '\t\t\t\t\t\t'+marker+'days_in_month_table[month_index]')
        assert target in methods
        methods = methods.replace(target, replacement)
    header = ('// Generated from MAME '+PIN+'. BSD-3-Clause.\n'
              '// Raphael Nabet, R. Belmont; see rtc65271_LICENSE.txt.\n'
              '// Framework timer declarations adapted; two month-table bounds guards.\n')
    return header+constants+'\n'+methods+'\n'


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('source', type=Path);p.add_argument('--check', action='store_true')
    args = p.parse_args();out = Path(__file__).with_name('rtc65271_mame.inc')
    data = generate(args.source)
    if args.check:
        if out.read_text() != data:
            p.exit(1, 'RTC65271 generated source differs\n')
        print('RTC65271 source extraction matches reviewed MAME pin')
    else:
        out.write_text(data)
