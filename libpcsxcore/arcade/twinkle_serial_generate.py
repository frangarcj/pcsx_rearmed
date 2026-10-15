#!/usr/bin/env python3
"""Extract reviewed MAME serial/UART/Super-I/O/JVC logic with host plumbing."""
import argparse
import hashlib
from pathlib import Path
import re
PIN='deb2f54fe7300e7a192690318beb860159bfd52a'
HASHES={
'src/emu/diserial.cpp':'7f5307c6e2025ef02b4a8ba40b85fb446b9aada8fa74e55ab7ded8ac52a6f4b4',
'src/emu/diserial.h':'a787f56d47cc003b6f13918e81ba7115908de28549d3d69a8fc6da951637c53f',
'src/devices/machine/ins8250.cpp':'460bd261fee6e75a54fbda21bddab4d3ab7258254f9570d3eeb665a73a793836',
'src/devices/machine/ins8250.h':'eca045c860b6b658974e7fe66920f80ea28fdce7e4e5ccba010d05dffee4548e',
'src/devices/machine/fdc37c665gt.h':'8492f4df518b5c24f8e2a28437d0551f8085be881902de4d3001321dce211a21',
'src/devices/bus/rs232/xvd701.h':'5b739c2940251f16b220be53db85cfab7d96bcf499ee91fcc032645553045e99',
'src/devices/machine/fdc37c665gt.cpp':'79605b64a4b99ee73918cf7fb31d12b2e06fccc50cdfc4a3c72e1e03339a6c8e',
'src/devices/bus/rs232/xvd701.cpp':'17706f7793e9dce75814d348f97dd6ae12a2d6826a9c7fc7ce29f5767d029809'}
def generate(root):
    files={}
    for path,digest in HASHES.items():
        data=(root/path).read_bytes()
        if hashlib.sha256(data).hexdigest()!=digest:raise ValueError('unreviewed source: '+path)
        files[Path(path).name]=data.decode()
    header=files['diserial.h'];header=header[header.index('class device_serial_interface :'):header.index('template <u32 FIFO_LENGTH>')]
    header=header.replace('const machine_config &mconfig, device_t &device','device_t &device')
    uart=files['ins8250.h'];base=uart[uart.index('class ins8250_uart_device :'):uart.index('class ins8250_device :')]
    base=re.sub(r'ins8250_uart_device\(const machine_config .*?dev_type device_type\);','ins8250_uart_device(Scheduler &,u32);',base,flags=re.S)
    base=base.replace('devcb_write_line','Line')
    fifo=uart[uart.index('class ns16550_device :'):uart.index('class pc16552_device :')]
    fifo=re.sub(r'ns16550_device\(const machine_config .*?clock = 0\);','ns16550_device(Scheduler &,u32);',fifo)
    fifo=fifo.replace('void set_timer() {','void set_timer() { if (!m_regs.dl) throw Fault("UART FIFO timeout with zero divisor");')
    # Framework used to construct the source classes is supplied by the host.
    serial=files['diserial.cpp'];serial=serial[serial.index('device_serial_interface::device_serial_interface'):]
    serial=serial.replace('const machine_config &mconfig, device_t &device','device_t &device').replace('device().save_item','save_item')
    serial=re.sub(r'device\(\)\.machine\(\)\.scheduler\(\)\.timer_alloc\(timer_expired_delegate\(FUNC\((.*?)\), this\)\)',r'device().timer_alloc(FUNC(\1), this)',serial)
    needle='int bit = (m_tra_register_data >> (m_tra_bit_count - 1 - m_tra_bit_count_transmitted)) & 1;'
    assert needle in serial
    serial=serial.replace(needle,'if (!m_tra_bit_count || m_tra_bit_count_transmitted >= m_tra_bit_count) throw Fault("unconfigured/exhausted serial frame");\n\t'+needle)
    source=files['ins8250.cpp'];source=source[source.index("/* int's pending */"):]
    source=source.replace('void ins8250_uart_device::device_start()','void ins8250_uart_device::device_start()')
    fdc=files['fdc37c665gt.cpp'];start=fdc[fdc.index('void fdc37c665gt_device::device_start()'):fdc.index('void fdc37c665gt_device::device_add_mconfig')]
    fdc=start+fdc[fdc.index('uint8_t fdc37c665gt_device::read'):]
    fdc=fdc.replace('void fdc37c665gt_device::write_configuration_register(int index, int data)\n{','void fdc37c665gt_device::write_configuration_register(int index, int data)\n{\n\tif (index < 0 || index >= 16) throw Fault("FDC configuration index outside source array");')
    dvd=files['xvd701.cpp'];dvd=dvd[dvd.index('void jvc_xvd701_device::device_start()'):dvd.index('DEFINE_DEVICE_TYPE(')]
    return {'twinkle_serial_declarations.inc':header+'\n'+base+'\n'+fifo,
            'twinkle_serial_mame.inc':serial+'\n'+source+'\n'+fdc+'\n'+dvd}
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('source',type=Path);p.add_argument('--check',action='store_true');a=p.parse_args()
    for name,data in generate(a.source).items():
        data='// Generated from MAME '+PIN+'. BSD-3-Clause.\n// See twinkle_serial_LICENSE.txt; framework adapter/defined bounds guards only.\n'+data.rstrip()+'\n'
        out=Path(__file__).with_name(name)
        if a.check:
            if out.read_text()!=data:p.exit(1,name+' differs\n')
        else:out.write_text(data)
    if a.check:print('Twinkle serial extraction matches reviewed MAME sources')
