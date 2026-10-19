#!/usr/bin/env python3
"""dis.py <addr_hex> [count_bytes] [-- file]: disassemble stock app (base 0x16000); annotate literal pool loads."""
import os
import capstone,struct,sys
root = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
image = os.environ.get('STOCK_IMAGE', os.path.join(root, 'evidence', 'stock', 'stock-7.9.07.bin'))
d=open(image,'rb').read()
md=capstone.Cs(capstone.CS_ARCH_ARM,capstone.CS_MODE_THUMB|capstone.CS_MODE_MCLASS); md.detail=True
a=int(sys.argv[1],16); n=int(sys.argv[2],0) if len(sys.argv)>2 else 64
for i in md.disasm(d[a-0x16000:a-0x16000+n],a):
    note=''
    if i.mnemonic.startswith('ldr') and ', [pc,' in i.op_str:
        imm=int(i.op_str.split('#')[1].rstrip(']'),0)
        la=((i.address+4)&~3)+imm
        note='   ; =%#x'%struct.unpack_from('<I',d,la-0x16000)[0]
    print('%#x %s %s%s'%(i.address,i.mnemonic,i.op_str,note))
