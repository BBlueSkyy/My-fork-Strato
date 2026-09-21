#!/usr/bin/env python3
"""Run the actual assembled SaveCtx/LoadCtx in Unicorn AArch64.
Usage: python tests/kernel/nce_context.py /path/to/guest.S.o
Requires unicorn and pyelftools. This is not a game/device test.
"""
import struct
import sys
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
from unicorn import arm64_const as r

with open(sys.argv[1], 'rb') as source:
    elf = ELFFile(source)
    code = elf.get_section_by_name('.text').data()
    symbols = {s.name: s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
assert symbols['LoadCtx'] - symbols['SaveCtx'] == 51 * 4
assert len(code) - symbols['LoadCtx'] == 36 * 4
machine = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
text, context, stack, stop = 0x100000, 0x200000, 0x300000, 0x108000
for base in (text, context, stack):
    machine.mem_map(base, 0x10000)
machine.mem_write(text, code)
sp = stack + 0x8000 - 16
regs = [getattr(r, 'UC_ARM64_REG_X' + str(i)) for i in range(31)]
gp = [0x1234567800000000 + i for i in range(31)]
vectors = [((0xfedcba9876540000 + i) << 64) | (0xabcdef0000000000 + i) for i in range(32)]
for reg, value in zip(regs, gp):
    machine.reg_write(reg, value)
for i, value in enumerate(vectors):
    machine.reg_write(getattr(r, 'UC_ARM64_REG_Q' + str(i)), value)
for reg, value in ((r.UC_ARM64_REG_SP, sp), (r.UC_ARM64_REG_TPIDR_EL0, context),
                   (r.UC_ARM64_REG_NZCV, 0xa0000000), (r.UC_ARM64_REG_FPCR, 0x400000),
                   (r.UC_ARM64_REG_FPSR, 0x8000011), (regs[30], stop)):
    machine.reg_write(reg, value)
machine.mem_write(sp, struct.pack('<Q', gp[30]))
machine.emu_start(text + symbols['SaveCtx'], stop, count=1000)
snapshot = bytes(machine.mem_read(context, 0x350))
assert struct.unpack_from('<19Q', snapshot) == tuple(gp[:19])
assert struct.unpack_from('<12Q', snapshot, 0x2e0) == tuple(gp[19:])
assert struct.unpack_from('<QQ', snapshot, 0x340) == (sp + 16, gp[30])
assert snapshot[0xa0:0x2a0] == b''.join(v.to_bytes(16, 'little') for v in vectors)
assert struct.unpack_from('<II', snapshot, 0x2d8) == (0x8000011, 0x400000)
assert struct.unpack_from('<I', snapshot, 0x2c0) == (0xa0000000,)
assert [machine.reg_read(reg) for reg in regs[:30]] == gp[:30]
for reg in regs[:19]:
    machine.reg_write(reg, 0)
for i in range(32):
    machine.reg_write(getattr(r, 'UC_ARM64_REG_Q' + str(i)), 0)
for reg in (r.UC_ARM64_REG_NZCV, r.UC_ARM64_REG_FPCR, r.UC_ARM64_REG_FPSR):
    machine.reg_write(reg, 0)
machine.reg_write(regs[30], stop)
machine.emu_start(text + symbols['LoadCtx'], stop, count=1000)
assert [machine.reg_read(reg) for reg in regs[:30]] == gp[:30]
assert [machine.reg_read(getattr(r, 'UC_ARM64_REG_Q' + str(i))) for i in range(32)] == vectors
assert machine.reg_read(r.UC_ARM64_REG_SP) == sp
assert machine.reg_read(r.UC_ARM64_REG_NZCV) == 0xa0000000
assert machine.reg_read(r.UC_ARM64_REG_FPCR) == 0x400000
assert machine.reg_read(r.UC_ARM64_REG_FPSR) == 0x8000011
print('AArch64 SaveCtx/LoadCtx register and ABI checks passed')
