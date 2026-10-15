#!/usr/bin/env python3
"""Execute the production NCE context routines, including relocated copies.

Requires unicorn and keystone-engine. No Android runtime or game files needed.
"""
from pathlib import Path
import re

from keystone import Ks, KS_ARCH_ARM64, KS_MODE_LITTLE_ENDIAN
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
from unicorn import arm64_const as reg

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "app/src/main/cpp/skyline/nce/guest.S"
HEADER = ROOT / "app/src/main/cpp/skyline/nce/guest.h"


def routines():
    source = re.sub(r"/\*.*?\*/", "", SOURCE.read_text(), flags=re.S)
    source = re.sub(r"//[^\n]*", "", source)
    assembler = Ks(KS_ARCH_ARM64, KS_MODE_LITTLE_ENDIAN)
    result = {}
    for name in ("SaveCtx", "LoadCtx"):
        body = source.split(name + ":", 1)[1].split(".global", 1)[0]
        code, _ = assembler.asm(body)
        result[name] = bytes(code)
        expected = int(re.search(name + r"Size\{(\d+)\}", HEADER.read_text())[1])
        assert len(code) == expected * 4, (name, len(code), expected)
    return result


def roundtrip(code, code_base, fpsr, fpcr):
    cpu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    context, stack = 0x200000, 0x300000
    for address in (code_base, context, stack):
        cpu.mem_map(address, 0x2000)
    cpu.mem_write(code_base, code["SaveCtx"])
    cpu.mem_write(code_base + 0x1000, code["LoadCtx"])
    cpu.mem_write(context, b"\xA5" * 0x2000)
    cpu.reg_write(reg.UC_ARM64_REG_TPIDR_EL0, context)
    initial_sp = stack + 0x1000
    cpu.reg_write(reg.UC_ARM64_REG_SP, initial_sp)
    gp = [0x1234567800000000 + i for i in range(19)]
    vectors = [(0xFFEEDDCCBBAA9988 + i) << 64 | (0x1122334455667700 + i)
               for i in range(32)]
    for i, value in enumerate(gp):
        cpu.reg_write(getattr(reg, f"UC_ARM64_REG_X{i}"), value)
    for i, value in enumerate(vectors):
        cpu.reg_write(getattr(reg, f"UC_ARM64_REG_Q{i}"), value)
    cpu.reg_write(reg.UC_ARM64_REG_FPSR, fpsr)
    cpu.reg_write(reg.UC_ARM64_REG_FPCR, fpcr)
    cpu.reg_write(reg.UC_ARM64_REG_NZCV, 0xA0000000)
    stop = code_base + 0x800
    cpu.reg_write(reg.UC_ARM64_REG_LR, stop)
    cpu.emu_start(code_base, stop, count=100)
    assert cpu.reg_read(reg.UC_ARM64_REG_PC) == stop
    # Simulate arbitrary caller-saved register use by the host handler.
    for i in range(19):
        cpu.reg_write(getattr(reg, f"UC_ARM64_REG_X{i}"), 0)
    for i in range(32):
        cpu.reg_write(getattr(reg, f"UC_ARM64_REG_Q{i}"), 0)
    cpu.reg_write(reg.UC_ARM64_REG_FPSR, 0)
    cpu.reg_write(reg.UC_ARM64_REG_FPCR, 0)
    cpu.reg_write(reg.UC_ARM64_REG_NZCV, 0)
    cpu.reg_write(reg.UC_ARM64_REG_LR, stop)
    cpu.emu_start(code_base + 0x1000, stop, count=100)
    assert cpu.reg_read(reg.UC_ARM64_REG_PC) == stop
    for i, value in enumerate(vectors):
        actual = cpu.reg_read(getattr(reg, f"UC_ARM64_REG_Q{i}"))
        assert actual == value, f"Q{i} corrupted: expected {value:032X}, got {actual:032X}"
    for i, value in enumerate(gp):
        assert cpu.reg_read(getattr(reg, f"UC_ARM64_REG_X{i}")) == value
    assert cpu.reg_read(reg.UC_ARM64_REG_SP) == initial_sp
    assert cpu.reg_read(reg.UC_ARM64_REG_FPSR) == fpsr
    assert cpu.reg_read(reg.UC_ARM64_REG_FPCR) == fpcr
    assert cpu.reg_read(reg.UC_ARM64_REG_NZCV) == 0xA0000000
    assert cpu.reg_read(reg.UC_ARM64_REG_TPIDR_EL0) == context
    # Host/TLS metadata must not be overwritten by either context routine.
    assert bytes(cpu.mem_read(context + 0x2A0, 0x20)) == b"\xA5" * 0x20


if __name__ == "__main__":
    code = routines()
    for base in (0x100000, 0x400000):
        for fpsr, fpcr in ((0, 0), (0x08000001, 0x00400000)):
            roundtrip(code, base, fpsr, fpcr)
    # Check the sizing formula against the instruction writes in the actual
    # entry/exit-hook emitter. Each LR preservation block emits six words.
    nce = (ROOT / "app/src/main/cpp/skyline/nce.cpp").read_text()
    store = nce.split("/* TLS LR Store */", 1)[1].split("/* Entry Hook */", 1)[0]
    load = nce.split("/* TLS LR Load */", 1)[1].split("*hook++ = 0xD65F03C0", 1)[0]
    lr_words = store.count("*hook++") + load.count("*hook++")
    sizing = nce.split("size_t NCE::GetHookSectionSize", 1)[1].split("void NCE::WriteHookSection", 1)[0]
    expression = re.search(r"EntryExitHook>\(entry.hook\)\)\s*size \+= ([^;]+)", sizing)[1]
    # Only arithmetic constants and this known identifier occur in the formula.
    numeric = expression.replace("EmitTrampolineSize", "10")
    assert re.fullmatch(r"[0-9 +]+", numeric)
    sized_words = sum(int(value.strip()) for value in numeric.split("+"))
    assert sized_words == lr_words + 2 * 10 + 2, (sized_words, lr_words)
    print("NCE context tests passed (4 ARM64 roundtrips; Q0-Q31, X0-X18, FP state, SP, TLS)")
