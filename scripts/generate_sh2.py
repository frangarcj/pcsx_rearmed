#!/usr/bin/env python3
from pathlib import Path
import re
import argparse, subprocess
parser = argparse.ArgumentParser(description="Extract standalone SH-2 handlers from the pinned MAME ISA.")
parser.add_argument("--mame-source", type=Path, required=True)
args = parser.parse_args()
commit = subprocess.check_output(["git", "-C", str(args.mame_source), "rev-parse", "HEAD"], text=True).strip()
if commit != "deb2f54fe7300e7a192690318beb860159bfd52a":
    raise SystemExit("MAME checkout does not match the pinned revision")
source = args.mame_source / "src/devices/cpu/sh"
root = Path(__file__).resolve().parents[1]
s=(source/'sh.cpp').read_text();body=s[s.index('void sh_common_execution::ADD('):s.index('// DRC / UML related')]
t=(source/'sh2.cpp').read_text();extra=t[t.index('inline void sh2_device::LDCMSR('):t.index('void sh2_device::execute_run()')]
body=body.replace('sh_common_execution::','StandaloneSh2::');extra=extra.replace('inline void sh2_device::','void StandaloneSh2::').replace('void sh2_device::','void StandaloneSh2::')
body += '\n'+extra
body=body.replace('debugger_exception_hook(imm);','').replace('debugger_exception_hook(4);','')
body=body.replace('void StandaloneSh2::ILLEGAL()\n{','void StandaloneSh2::ILLEGAL()\n{\n    s.illegal_pc = s.pc - 2; s.illegal_opcode = read_word(s.illegal_pc);')
body='\n'.join(line.rstrip() for line in body.splitlines()).rstrip()+'\n'
decls=re.findall(r'void StandaloneSh2::(\w+)\((.*?)\)',body)
(root / 'libpcsxcore/arcade/sh2_ops.inc').write_text('/* Generated from MAME sh.cpp/sh2.cpp, deb2f54fe7300e7a192690318beb860159bfd52a.\n * Copyright David Haywood and the SH-2 contributors. BSD-3-Clause; sh2_LICENSE.txt. */\n'+body)
print('Extracted',len(decls),'SH-2 execution methods')
