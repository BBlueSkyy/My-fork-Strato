#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Run production NCE patch emission and guest.S under an ARM64 interpreter.
Requires clang with AArch64 assembler support, pyelftools and unicorn.
The host handler is substituted at its ABI boundary; the actual context save,
stack/TLS switching, restore, branch offsets and displaced instructions execute.
"""
import os
import pathlib
import struct
import subprocess
import tempfile
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn import arm64_const as reg

ROOT = pathlib.Path(__file__).resolve().parents[2]
CPP = ROOT / 'app/src/main/cpp/skyline'

def function(source, signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

with tempfile.TemporaryDirectory(prefix='strato-nce-nvn-') as work:
    work = pathlib.Path(work)
    subprocess.run([os.environ.get('CLANG', 'clang'), '--target=aarch64-none-elf', '-c',
                    str(CPP / 'nce/guest.S'), '-o', str(work / 'guest.o')], check=True)
    with (work / 'guest.o').open('rb') as stream:
        elf = ELFFile(stream)
        assembly = elf.get_section_by_name('.text').data()
        symbols = {s.name: s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
    assert symbols['LoadCtx'] == 38 * 4 and len(assembly) - symbols['LoadCtx'] == 36 * 4
    source = (CPP / 'nce.cpp').read_text()
    shared = source[source.index('    constexpr size_t TrampolineSize'):source.index('    NCE::PatchData NCE::GetPatchData')]
    preamble = '''
#include <fstream>
#include <cassert>
#include <iostream>
#include <common.h>
#include <nce/instructions.h>
#include <nce/crysis_nvn_probe.h>
namespace skyline::util { constexpr u64 ClockFrequency{19200000}; }
namespace skyline::nce {
constexpr u16 CrysisMemoryCallbackProbeSvc{0x7FF0};
class NCE { public:
 struct PatchData {size_t size; std::vector<size_t> offsets;};
 static void SvcHandler(u16,void*) {}
 static PatchData GetPatchData(const std::vector<u8>&);
 static void PatchCode(std::vector<u8>&,u32*,size_t,const std::vector<size_t>&,size_t=0);
};
namespace guest {
constexpr size_t SaveCtxSize=38, LoadCtxSize=36;
'''
    arrays = ''
    for name, data in [('SaveCtx', assembly[:symbols['LoadCtx']]), ('LoadCtx', assembly[symbols['LoadCtx']:])]:
        arrays += 'alignas(4) u8 ' + name + '[]={' + ','.join(map(str, data)) + '};\n'
    harness = preamble + arrays + '}\n' + shared
    for signature in ['NCE::PatchData NCE::GetPatchData', 'void NCE::PatchCode']:
        harness += function(source, signature) + '\n'
    harness += '''
}
int main(int argc,char**argv) {
 using namespace skyline; using namespace skyline::nce;
 const u32 svc=std::stoul(argv[2]);
 std::vector<u8> text(8);const u32 instruction=0xD4000001U|(svc<<5);
 std::memcpy(text.data(),&instruction,4);
 auto info=NCE::GetPatchData(text);
 std::vector<u32> patch(info.size/4+16,0xDEADBEEF);
 NCE::PatchCode(text,patch.data(),info.size,info.offsets);
 for(size_t i=info.size/4;i<patch.size();++i)assert(patch[i]==0xDEADBEEF);
 std::ofstream output(argv[1],std::ios::binary);
 output.write(reinterpret_cast<char*>(patch.data()),info.size);
 output.write(reinterpret_cast<char*>(text.data()),text.size());
 std::cout<<info.size<<'\\n';
}
'''
    (work / 'emit.cpp').write_text(harness)
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++20', '-O1', '-DFMT_HEADER_ONLY',
                    '-I'+str(ROOT / 'tests/nso/host'), '-I'+str(CPP),
                    '-I'+str(ROOT / 'app/libraries/fmt/include'), str(work / 'emit.cpp'),
                    '-o', str(work / 'emit')], check=True)
    xregs = [getattr(reg, 'UC_ARM64_REG_X'+str(i)) for i in range(31)]
    qregs = [getattr(reg, 'UC_ARM64_REG_Q'+str(i)) for i in range(32)]
    # Expected guest-visible effect of each displaced instruction, independently
    # derived from the verified update SDK disassembly (ordinary SVC has none).
    specs = [(0x7FF0, 16, [30]), (0x7FF1, 80, [26]),
             (0x7FF2, 32, [21, 20]), (0x7FF3, 16, [19, 30]), (0x7FF4, 64, []), (1, 0, [])]
    for svc, stack_delta, stores in specs:
        binary = work / 'patch.bin'
        patch_size = int(subprocess.check_output([str(work / 'emit'), str(binary), str(svc)], text=True))
        for flags in [0, 0x60000000, 0x80000000, 0xF0000000]:
            cpu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
            base, tls, guest_sp, host_sp = 0x100000, 0x200000, 0x310000, 0x410000
            for address in [base, tls, guest_sp-0x10000, host_sp-0x10000]:
                cpu.mem_map(address, 0x20000)
            cpu.mem_write(base, binary.read_bytes())
            cpu.mem_write(tls+0x2A0, struct.pack('<QQ', 0x500000, host_sp))
            initial_x = [0x1111111100000000 + i*0x101 for i in range(31)]
            initial_q = [(0x9876543210FEDCBA << 64) + i for i in range(32)]
            for r, value in zip(xregs, initial_x): cpu.reg_write(r, value)
            for r, value in zip(qregs, initial_q): cpu.reg_write(r, value)
            cpu.reg_write(reg.UC_ARM64_REG_SP, guest_sp)
            cpu.reg_write(reg.UC_ARM64_REG_TPIDR_EL0, tls)
            cpu.reg_write(reg.UC_ARM64_REG_NZCV, flags)
            cpu.reg_write(reg.UC_ARM64_REG_FPCR, 0x400000)
            cpu.reg_write(reg.UC_ARM64_REG_FPSR, 0x8000000)
            calls = []
            def at_instruction(uc, address, size, _):
                word = struct.unpack('<I', bytes(uc.mem_read(address, 4)))[0]
                if word == 0xD63F0040:  # Production trampoline's BLR X2 into host.
                    assert uc.reg_read(reg.UC_ARM64_REG_X0) == svc
                    assert uc.reg_read(reg.UC_ARM64_REG_X1) == tls
                    assert uc.reg_read(reg.UC_ARM64_REG_TPIDR_EL0) == 0x500000
                    assert uc.reg_read(reg.UC_ARM64_REG_SP) == host_sp-16
                    saved_sp = struct.unpack('<Q', bytes(uc.mem_read(host_sp-8, 8)))[0]
                    assert saved_sp == guest_sp-(32 if svc >= 0x7FF0 else 16)
                    assert struct.unpack('<Q', bytes(uc.mem_read(saved_sp, 8)))[0] == initial_x[30]
                    calls.append(address)
                    for r in xregs[:19]: uc.reg_write(r, 0xBAD)
                    for r in qregs: uc.reg_write(r, 0xBAD)
                    uc.reg_write(reg.UC_ARM64_REG_NZCV, flags ^ 0xF0000000)
                    uc.reg_write(reg.UC_ARM64_REG_FPCR, 0)
                    uc.reg_write(reg.UC_ARM64_REG_FPSR, 0)
                    uc.reg_write(reg.UC_ARM64_REG_PC, address+4)
            cpu.hook_add(UC_HOOK_CODE, at_instruction)
            cpu.emu_start(base+patch_size, base+patch_size+4, count=500)
            assert len(calls) == 1
            assert [cpu.reg_read(r) for r in xregs] == initial_x
            expected_q = list(initial_q)
            if svc == 1:
                # Characterize the pre-existing ordinary-SVC Q31 overlap. This
                # investigation changes only synthetic diagnostic stubs.
                expected_q[31] = (0x0040000008000000 << 64) | 31
            assert [cpu.reg_read(r) for r in qregs] == expected_q
            assert cpu.reg_read(reg.UC_ARM64_REG_SP) == guest_sp-stack_delta
            assert cpu.reg_read(reg.UC_ARM64_REG_TPIDR_EL0) == tls
            assert cpu.reg_read(reg.UC_ARM64_REG_NZCV) == flags
            assert cpu.reg_read(reg.UC_ARM64_REG_FPCR) == 0x400000
            assert cpu.reg_read(reg.UC_ARM64_REG_FPSR) == 0x8000000
            for i, index in enumerate(stores):
                assert struct.unpack('<Q', bytes(cpu.mem_read(guest_sp-stack_delta+8*i, 8)))[0] == initial_x[index]
        print(f'PASS SVC 0x{svc:X}: production patch emitter, context/TLS/flags, stack and displaced prologue')
