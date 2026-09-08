#!/usr/bin/env python3
"""Reproduce dangling NSO symbol views, then test production owned storage.

Extracts the actual bookkeeping struct and its initializer. Temporary .rodata
is mmap-backed and unmapped after loading, just as an NSO's temporary buffers
are destroyed. The old span-based version must fail; the fixed version must
preserve both symbols and names. Requires g++ on Linux.
"""
import subprocess
import tempfile
from pathlib import Path

root = Path(__file__).resolve().parents[2]
loader = root / 'app/src/main/cpp/skyline/loader'
header, source = (loader/'loader.h').read_text(), (loader/'loader.cpp').read_text()
descriptor = 'struct ExecutableSymbolicInfo {' + header.split('struct ExecutableSymbolicInfo {', 1)[1].split('\n        };', 1)[0] + '\n};'
initializer = 'ExecutableSymbolicInfo symbolicInfo{' + source.split('ExecutableSymbolicInfo symbolicInfo{', 1)[1].split('\n            };', 1)[0] + '\n};'
symbol_bounds = 'const auto symbolsFit{' + source.split('const auto symbolsFit{', 1)[1].split('\n        if (!symbolsFit', 1)[0]
nro_checks = ''
for path in [loader/'nro.cpp', root/'app/src/main/cpp/skyline/services/ro/IRoInterface.cpp']:
    conversion = 'if (header.dynsym.offset >' + path.read_text().split('if (header.dynsym.offset >', 1)[1].split('\n        }', 1)[0] + '\n}'
    nro_checks += r'''
    {
        struct Segment { size_t offset, size; };
        struct { Segment ro, dynsym, dynstr; } header{{0x1000, 0x1000}, {0x1200, 24}, {0x1300, 12}};
        struct { Segment dynsym, dynstr; } executable{};
    ''' + conversion + r'''
        assert(executable.dynsym.offset == 0x200 && executable.dynsym.size == 24);
        assert(executable.dynstr.offset == 0x300 && executable.dynstr.size == 12);
    }
    '''
fixture = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>
using u8 = uint8_t;
template<typename T> using span = std::span<T>;
''' + descriptor + r'''
int main() {
    rlimit limit{}; setrlimit(RLIMIT_CORE, &limit);
''' + nro_checks + r'''
    {
        struct Executable { struct RelativeSegment { size_t offset, size; }; };
        struct { struct { std::vector<u8> contents; } ro; } executable;
        executable.ro.contents.resize(0x1000);
''' + symbol_bounds + r'''
        assert(symbolsFit({0x200, 24}));
        assert(symbolsFit({0xFF0, 0x10}));
        assert(!symbolsFit({0x1200, 24}));
        assert(!symbolsFit({0xFF0, 0x11}));
        assert(!symbolsFit({SIZE_MAX-3, 8}));
    }
    const size_t page = sysconf(_SC_PAGESIZE);
    auto ro = static_cast<u8 *>(mmap(nullptr, page, PROT_READ|PROT_WRITE,
                                   MAP_PRIVATE|MAP_ANONYMOUS, -1, 0));
    assert(ro != MAP_FAILED);
    std::memset(ro, 0x43, 24);
    std::memcpy(ro+32, "\0TestSymbol\0", 12);
    span<u8> dynsym{ro, 24};
    span<char> dynstr{reinterpret_cast<char *>(ro+32), 12};
    u8 *base = ro, *executableBase = ro;
    struct { size_t size{}; } patch;
    size_t size=page;
    std::string name="sdk.nso";
''' + initializer + r'''
    assert(munmap(ro, page) == 0);
    assert(symbolicInfo.symbols.size() == 24);
    assert(std::all_of(symbolicInfo.symbols.begin(), symbolicInfo.symbols.end(), [](u8 b){ return b == 0x43; }));
    assert(std::string(symbolicInfo.symbolStrings.data()+1) == "TestSymbol");
    std::vector<ExecutableSymbolicInfo> modules;
    modules.push_back(std::move(symbolicInfo));
    for (int i=0; i<64; ++i) modules.emplace_back();
    assert(modules[0].symbols[0] == 0x43);
    assert(std::string(modules[0].symbolStrings.data()+1) == "TestSymbol");
}
'''
old = fixture.replace('std::vector<u8> symbols;', 'span<u8> symbols;')
old = old.replace('std::vector<char> symbolStrings;', 'span<char> symbolStrings;')
old = old.replace('.symbols = {dynsym.begin(), dynsym.end()},', '.symbols = dynsym,')
old = old.replace('.symbolStrings = {dynstr.begin(), dynstr.end()},', '.symbolStrings = dynstr,')
with tempfile.TemporaryDirectory() as directory:
    path=Path(directory)
    for label, code in [('old', old), ('fixed', fixture)]:
        (path/'test.cpp').write_text(code)
        subprocess.run(['g++', '-std=c++20', '-Wall', '-Wextra', '-Werror', str(path/'test.cpp'), '-o', str(path/'test')], check=True)
        result=subprocess.run([str(path/'test')], capture_output=True)
        if label == 'old':
            assert result.returncode == -11, (result.returncode, result.stderr)
        else:
            assert result.returncode == 0, (result.returncode, result.stderr)
print('PASS: old symbol views reproduce SIGSEGV; production copies survive rodata teardown and module relocation; NRO offsets and copy bounds checked')
