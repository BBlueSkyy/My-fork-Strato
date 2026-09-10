#!/usr/bin/env python3
"""Execute the production SVC trampoline in Unicorn and compare it to baseline.

Requires g++, an AArch64 GNU assembler, unicorn and pyelftools.
Usage: python tests/kernel/abort_diagnostics.py --assembler aarch64-linux-gnu-as
No game execution is claimed by this test.
"""
import argparse
import re
import struct
import subprocess
import tempfile
from pathlib import Path

from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn import arm64_const as r

parser = argparse.ArgumentParser()
parser.add_argument('--assembler', default='aarch64-linux-gnu-as')
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
skyline = root / 'app/src/main/cpp/skyline'
source = (skyline / 'nce.cpp').read_text()
writer = source.split('    u32 *WriteTrampoline(', 1)[1].split('    constexpr size_t RescaleClockSize', 1)[0]
writer = '    u32 *WriteTrampoline(' + writer[:writer.rfind('    }') + 5]
svc_patch = source.split('                /* Per-SVC Trampoline */', 1)[1].split('            } else if (mrs.Verify())', 1)[0]
trampoline_size = int(re.search(r'TrampolineSize\{(\d+)\}', source).group(1))

with tempfile.TemporaryDirectory() as directory:
    tmp = Path(directory)
    (tmp / 'common.h').write_text('''#pragma once
#include <cstddef>
#include <array>
#include <common/base.h>
namespace skyline::util {
template<typename T, size_t N> constexpr T MakeMagic(const char (&s)[N]) {
    T v{}; for (size_t i=0; i<N-1; ++i) v |= T(s[i]) << (i*8); return v;
}}
''')
    subprocess.run([args.assembler, str(skyline / 'nce/guest.S'), '-o', str(tmp / 'guest.o')], check=True)
    with (tmp / 'guest.o').open('rb') as file:
        elf = ELFFile(file)
        assembly = elf.get_section_by_name('.text').data()
        symbols = {s.name: s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
    assert symbols['LoadCtx'] == 38 * 4
    assert len(assembly) == (38 + 36) * 4
    (tmp / 'guest.bin').write_bytes(assembly)
    binaries = []
    for baseline in (True, False):
        implementation = writer
        size = trampoline_size
        if baseline:
            implementation = re.sub(r'        // BEGIN diagnostic call-site capture.*?// END diagnostic call-site capture[^\n]*\n', '', implementation, flags=re.S)
            size -= 11
        driver = '''#include <cstdio>
#include <cstring>
#include <fstream>
#include <nce/guest.h>
#include <nce/instructions.h>
namespace skyline::nce {
''' + implementation + f'''
constexpr size_t TrampolineSize{{{size}}};
void Emit(const char *assemblyPath) {{
    std::array<u32, 0x9000/4> code{{}};
    std::array<u32, 74> assembly{{}};
    std::ifstream input(assemblyPath, std::ios::binary);
    input.read(reinterpret_cast<char *>(assembly.data()), sizeof(assembly));
    u32 *start=code.data(), *patch=start, *end=start+0x8000/4;
    std::memcpy(patch, assembly.data(), guest::SaveCtxSize*4);
    patch += guest::SaveCtxSize;
    auto before=patch;
    patch=WriteTrampoline(patch, 0x600000);
    if (size_t(patch-before) != TrampolineSize) std::abort();
    std::memcpy(patch, assembly.data()+guest::SaveCtxSize, guest::LoadCtxSize*4);
    patch += guest::LoadCtxSize;
    u32 *instruction=end;
    size_t offset=0, textOffset=0;
    instructions::Svc svc{{}}; svc.value=0x15;
    auto endOffset=[&] {{ return size_t(end-patch) + textOffset/4; }};
    auto startOffset=[&] {{ return size_t(start-patch); }};
''' + svc_patch + '''
    std::fwrite(code.data(), 1, sizeof(code), stdout);
}
}
int main(int, char **argv) { skyline::nce::Emit(argv[1]); }
'''
        (tmp / 'emitter.cpp').write_text(driver)
        subprocess.run(['g++', '-std=c++20', '-I'+str(tmp), '-I'+str(skyline), str(tmp/'emitter.cpp'), '-o', str(tmp/'emitter')], check=True)
        binaries.append(subprocess.check_output([str(tmp/'emitter'), str(tmp/'guest.bin')]))

    results = []
    for baseline, binary in zip((True, False), binaries):
        m = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        text, tls, stack, host_stack, host_tls, handler = [i*0x100000 for i in range(1, 7)]
        for address in (text, tls, stack, host_stack, host_tls, handler):
            m.mem_map(address, 0x10000)
        m.mem_write(text, binary)
        gp = [0x1234000000000000+i for i in range(31)]
        vectors = [(0xdeadbeef00000000+i) << 64 | (0xabcdef0000+i) for i in range(32)]
        regs = [getattr(r, 'UC_ARM64_REG_X'+str(i)) for i in range(31)]
        for reg, value in zip(regs, gp):
            m.reg_write(reg, value)
        for i, value in enumerate(vectors):
            m.reg_write(getattr(r, 'UC_ARM64_REG_Q'+str(i)), value)
        sp = stack+0x8000
        for reg, value in ((r.UC_ARM64_REG_SP, sp), (r.UC_ARM64_REG_TPIDR_EL0, tls),
                           (r.UC_ARM64_REG_NZCV, 0xa0000000), (r.UC_ARM64_REG_FPCR, 0x400000),
                           (r.UC_ARM64_REG_FPSR, 0x8000011)):
            m.reg_write(reg, value)
        m.mem_write(tls+0x2a0, struct.pack('<QQ', host_tls, host_stack+0x8000))
        visited = []

        def host_callback(machine, address, size, data):
            if address != handler:
                return
            visited.append(address)
            assert machine.reg_read(regs[0]) == 0x15
            assert machine.reg_read(regs[1]) == tls
            assert machine.reg_read(r.UC_ARM64_REG_TPIDR_EL0) == host_tls
            assert host_stack < machine.reg_read(r.UC_ARM64_REG_SP) < host_stack+0x10000
            if not baseline:
                snapshot = bytes(machine.mem_read(tls+0x2e0, 0x70))
                assert struct.unpack_from('<12Q', snapshot) == tuple(gp[19:])
                captured_sp, trampoline_lr = struct.unpack_from('<QQ', snapshot, 0x60)
                assert captured_sp == sp
                branch_address = trampoline_lr+8
                branch = struct.unpack('<I', bytes(machine.mem_read(branch_address, 4)))[0]
                assert branch >> 26 == 5
                words = (branch & 0x3ffffff) - (0x4000000 if branch & 0x2000000 else 0)
                assert branch_address+words*4-4 == text+0x8000
            # Simulate a C++ SVC handler clobbering caller-saved registers while
            # returning a successful Result and handle through ThreadContext.
            machine.mem_write(tls, struct.pack('<QQ', 0, 0xd01f))
            return_address = machine.reg_read(regs[30])
            for reg in regs[:19]:
                machine.reg_write(reg, 0xbad)
            machine.reg_write(r.UC_ARM64_REG_PC, return_address)

        m.hook_add(UC_HOOK_CODE, host_callback)
        m.emu_start(text+0x8000, text+0x8004, count=1000)
        assert visited == [handler]
        final_gp = [m.reg_read(reg) for reg in regs]
        assert final_gp == [0, 0xd01f] + gp[2:]
        assert m.reg_read(r.UC_ARM64_REG_SP) == sp
        assert m.reg_read(r.UC_ARM64_REG_TPIDR_EL0) == tls
        results.append((final_gp, [m.reg_read(getattr(r, 'UC_ARM64_REG_Q'+str(i))) for i in range(32)],
                        [m.reg_read(reg) for reg in (r.UC_ARM64_REG_NZCV, r.UC_ARM64_REG_FPCR, r.UC_ARM64_REG_FPSR)]))
    # Existing FP/SIMD behavior is preserved, not repaired by this diagnostic.
    assert results[0] == results[1]
print('PASS: production SVC trampoline captures guest PC/LR/SP/X19-X30 and preserves baseline register/TLS behavior')
