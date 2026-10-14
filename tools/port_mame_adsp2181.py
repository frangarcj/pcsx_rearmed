#!/usr/bin/env python3
"""Extract the pinned complete ADSP2181 interpreter/ops without MAME devices."""
import argparse
from pathlib import Path
import re
import subprocess
ROOT=Path(__file__).resolve().parents[1]
PIN='deb2f54fe7300e7a192690318beb860159bfd52a'
def function(text,signature):
    begin=text.index(signature);brace=text.index('{',begin);depth=1;end=brace+1
    while depth:
        if text[end]=='{':depth+=1
        elif text[end]=='}':depth-=1
        end+=1
    return text[begin:end]
def generate(source):
    base=source/'src/devices/cpu/adsp2100'
    if subprocess.run(['git','-C',str(source),'rev-parse','HEAD'],check=True,capture_output=True,text=True).stdout.strip()!=PIN:raise SystemExit('ADSP source differs from pinned commit')
    if subprocess.run(['git','-C',str(source),'diff','--quiet','HEAD','--',str(base.relative_to(source))]).returncode:raise SystemExit('ADSP source has local changes')
    cpp=(base/'adsp2100.cpp').read_text();header=(base/'adsp2100.h').read_text();ops=(base/'2100ops.hxx').read_text()
    license='/* Generated from MAME '+PIN+'; BSD-3-Clause, Aaron Giles. See adsp2181_LICENSE.txt. */\n'
    registers=header[header.index('enum\n{\n\tADSP2100_PC'):header.index('//**************************************************************************\n//  TYPE DEFINITIONS')].strip()
    registers=registers.replace('ADSP2100_','ADSP2181_REG_').replace('STATE_GENPC','0x100')
    constants=header[header.index('// ADSP-2100 IRQs'):header.index('// ADSP-2181 IRQs')]
    declarations=header[header.index('\t// helpers\n'):header.index('\t// internal state\n')]
    declarations=declarations.replace('virtual bool generate_irq(int which, int indx = 0) = 0;','bool generate_irq(int which, int indx = 0);').replace('virtual void check_irqs() = 0;','void check_irqs();')
    fields=header[header.index('\t// internal state\n'):header.index('\t// address spaces\n')]
    fields=re.sub(r'\tconst address_space_config[^\n]+\n','',fields)
    tables=header[header.index('\t// tables\n'):header.index('\tdevcb_read32')]
    flags=header[header.index('\t// flag definitions\n'):header.index('// ======================> adsp2100_device')]
    flags=flags[:flags.rfind('};')]
    types='enum { CHIP_TYPE_ADSP2100, CHIP_TYPE_ADSP2101, CHIP_TYPE_ADSP2104, CHIP_TYPE_ADSP2105, CHIP_TYPE_ADSP2115, CHIP_TYPE_ADSP2181 };\n'
    coredecl=license+constants+'\nstruct Adsp2181 {\n'+types+declarations+fields+tables+flags+'''
    Adsp2181Config config{};
    uint64_t cycles=0, instructions=0, interrupts=0, diagnostic_count=0;
    uint32_t diagnostics=0, budget=0;
    unsigned callback_depth=0;
    bool running=false, halted=false;
    explicit Adsp2181(const Adsp2181Config &);
    void device_reset();void execute_run();void execute_set_input(int,int);
    void idma_addr_w(uint16_t);uint16_t idma_addr_r();
    void idma_data_w(uint16_t);uint16_t idma_data_r();
    uint64_t now() const;
    uint32_t m_sport_rx_cb(unsigned);
    void m_sport_tx_cb(unsigned,uint32_t,uint32_t);
    void m_timer_fired_cb(int);void m_dmovlay_cb(uint32_t);
    void logerror(const char *,...);
};
'''
    init=function(cpp,'adsp21xx_device::adsp21xx_device(')
    init=init[init.index('{')+1:-1]
    constructor='Adsp2181::Adsp2181(const Adsp2181Config &cfg) {\n std::memset(this,0,sizeof(*this));config=cfg;m_chip_type=CHIP_TYPE_ADSP2181;m_mstat_mask=0x7f;m_imask_mask=0x3ff;\n'+init+'\n running=true;device_reset();running=false;\n}\n'
    funcs=[function(cpp,'void adsp21xx_device::create_tables()'),function(cpp,'void adsp21xx_device::device_reset()'),function(cpp,'void adsp21xx_device::execute_set_input('),function(cpp,'bool adsp2181_device::generate_irq('),function(cpp,'void adsp2181_device::check_irqs()')]
    funcs += [function(cpp,sign) for sign in ('void adsp2181_device::idma_addr_w(', 'uint16_t adsp2181_device::idma_addr_r(', 'void adsp2181_device::idma_data_w(', 'uint16_t adsp2181_device::idma_data_r(')]
    execute=function(cpp,'void adsp21xx_device::execute_run()')
    execute=execute.replace('input_line_state(INPUT_LINE_HALT)','halted').replace('\t\tdebugger_wait_hook();\n','').replace('\tconst bool check_debugger = debugger_enabled();\n','').replace('\t\tif (check_debugger)\n\t\t\tdebugger_instruction_hook(m_pc);','\t\tinstructions++;')
    funcs[3]=funcs[3].replace('\t// clear the latch','\tinterrupts++;\n\t// clear the latch',1)
    body=license+constructor+'\n#include "adsp2181_ops.inc"\n\n'+'\n\n'.join(funcs)+'\n\n'+execute+'\n'
    body=body.replace('(res << 1)','(uint32_t(res) << 1)') # DIVQ may shift a negative remainder
    for key in ('adsp21xx_device::','adsp2181_device::'):
        body=body.replace(key,'Adsp2181::');ops=ops.replace(key,'Adsp2181::')
    # Define the original hardware wrap/shift semantics for negative operands.
    # Signed-right-shift remains arithmetic under C++20; all left shifts wrap.
    # Upstream stores its intermediate product in int32_t. Preserve those
    # source bit patterns without signed multiplication/shift overflow.
    ops=re.sub(r'\(xop \* yop\) << shift',r'int32_t(uint32_t(uint64_t(int64_t(xop) * yop) << shift))',ops)
    ops=re.sub(r'\(xop \* xop\) << shift',r'int32_t(uint32_t(uint64_t(int64_t(xop) * xop) << shift))',ops)
    ops=re.sub(r'(SHIFT_GETXREG_(?:SIGNED|UNSIGNED)\(xop\)) << 16',r'int32_t(uint32_t(\1) << 16)',ops)
    ops=ops.replace('(xop << sc)','int32_t(uint32_t(xop) << sc)').replace('(xop << -sc)','int32_t(uint32_t(xop) << -sc)')
    ops=ops.replace('(val << 8)','(uint32_t(val) << 8)')
    # NORM(HI) fills from carry; after inserting that bit, arithmetic shifts
    # saturate at the word width (ADSP-218x instruction reference, pp. 4-101/102).
    ops=ops.replace('xop >> (sc - 1)','adsp_asr32(xop,sc - 1)')
    getcases=[];setcases=[]
    for name,field in re.findall(r'state_add\(ADSP2100_(\w+),\s*"[^"]+",\s*(m_[\w.]+)\)',cpp):
        if name=='PC':continue
        reg='ADSP2181_REG_'+name;getcases.append(f'case {reg}:return {field};')
        mask='0xffffffffu'
        if name in ('ASTAT','SSTAT'):mask='0xff'
        if name=='MSTAT':mask='0x7f'
        if name=='IMASK':mask='0x3ff'
        if name=='ICNTL':mask='0x1f'
        if name=='CNTR':mask='0x3fff'
        if name in ('FLAGIN','FLAGOUT','FL0','FL1','FL2'):mask='1'
        if name=='MR2' or name=='MR2_SEC' or name=='SE' or name=='SE_SEC':value='int8_t(value)'
        elif name=='SB' or name=='SB_SEC':value='util::sext(value,5)'
        else:value=f'value&{mask}'
        action=''
        if name=='MSTAT':action='update_mstat();'
        if name in ('IMASK','ICNTL'):action='check_irqs();'
        if name=='DMOVLAY':action='update_dmovlay();'
        if name in ('PCSP','CNTRSP','STATSP','LOOPSP'):
            depth={'PCSP':16,'CNTRSP':4,'STATSP':4,'LOOPSP':4}[name]
            action=f'if(value>{depth})return -1;'+f'{field}={value};return 0;'
            setcases.append(f'case {reg}:{action}');continue
        setcases.append(f'case {reg}:{field}={value};{action}return 0;')
    maps=license+'uint32_t adsp2181_get_register(const Adsp2181 *s,unsigned r) {\nif(!s)return 0;\n'+''.join(f'if(r>=ADSP2181_REG_{reg}0 && r<=ADSP2181_REG_{reg}7)return s->{field}[r-ADSP2181_REG_{reg}0];\n' for reg,field in [('I','m_i'),('M','m_m'),('L','m_l')])+'switch(r){case ADSP2181_REG_PC:return s->m_pc;\n'+'\n'.join(c.replace('return m_','return s->m_') for c in getcases)+'default:return 0;}\n}\n'
    maps+='int adsp2181_set_register(Adsp2181 *s,unsigned r,uint32_t value){\nif(!s)return -1;\n'+''.join(f'if(r>=ADSP2181_REG_{reg}0 && r<=ADSP2181_REG_{reg}7){{unsigned n=r-ADSP2181_REG_{reg}0;s->{field}[n]={expr};{action}return 0;}}\n' for reg,field,expr,action in [('I','m_i','value&0x3fff','s->update_i(n);'),('M','m_m','util::sext(value,14)',''),('L','m_l','value&0x3fff','s->update_l(n);')])+'switch(r){case ADSP2181_REG_PC:s->m_pc=value&0x3fff;s->m_ppc=s->m_pc;return 0;\n'+'\n'.join(re.sub(r'\b(m_\w+)\b',r's->\1',c).replace('update_mstat();','s->update_mstat();').replace('check_irqs();','s->check_irqs();').replace('update_dmovlay();','s->update_dmovlay();') for c in setcases)+'default:return -1;}}\n'
    return {'adsp2181_internal.h':coredecl,'adsp2181_core.inc':body,'adsp2181_ops.inc':license+ops,'adsp2181_registers.inc':license+registers+'\n','adsp2181_register_api.inc':maps}
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--source',type=Path,default=ROOT.parent/'mamedev');p.add_argument('--check',action='store_true');a=p.parse_args()
    for name,body in generate(a.source).items():
        path=ROOT/'libpcsxcore/arcade'/name
        if a.check:
            if path.read_text()!=body:raise SystemExit('stale '+name)
        else:path.write_text(body)
