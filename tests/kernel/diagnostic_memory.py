#!/usr/bin/env python3
"""Exercise the production diagnostic reader against real Linux page protections.

The fixture replaces only VMM metadata, keeping the production Read() body. This
tests stale metadata, short reads, null/overflow addresses and unreadable pages.
Requires g++ and Linux process_vm_readv; no Android/game validation is implied.
"""
import subprocess
import tempfile
from pathlib import Path

root = Path(__file__).resolve().parents[2]
source = (root / 'app/src/main/cpp/skyline/nce/diagnostics.cpp').read_text()
reader = 'size_t Read(' + source.split('        size_t Read(', 1)[1].split('        std::string Bytes(', 1)[0]
fixture = r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <optional>
#include <span>
#include <sys/mman.h>
#include <sys/uio.h>
#include <unistd.h>
using u8 = uint8_t;
using u64 = uint64_t;
template<typename T> using span = std::span<T>;
struct Descriptor { struct { bool r; } permission; size_t size; };
struct Memory {
    u8 *base;
    size_t size;
    bool readable{true};
    bool AddressSpaceContains(span<u8> s) const {
        return uintptr_t(s.data()) >= uintptr_t(base) && s.size() <= size &&
               uintptr_t(s.data()) - uintptr_t(base) <= size - s.size();
    }
    auto GetChunk(u8 *p) const -> std::optional<std::pair<u8 *, Descriptor>> {
        if (!AddressSpaceContains({p, 1})) return {};
        return std::pair{base, Descriptor{{readable}, size}};
    }
    auto GetHostSpan(span<u8> s) const { return s; }
};
struct Process { Memory memory; };
struct DeviceState { Process *process; };
''' + reader + r'''
int main() {
    const size_t page = sysconf(_SC_PAGESIZE);
    auto p = static_cast<u8 *>(mmap(nullptr, page*2, PROT_READ|PROT_WRITE,
                                  MAP_PRIVATE|MAP_ANONYMOUS, -1, 0));
    assert(p != MAP_FAILED);
    Process process{{p, page*2}};
    DeviceState state{&process};
    std::memset(p, 0x42, page*2);
    std::array<u8, 128> out{};
    assert(Read(state, uintptr_t(p), out.data(), out.size()) == out.size());
    assert(std::all_of(out.begin(), out.end(), [](u8 b){ return b == 0x42; }));
    assert(Read(state, 0, out.data(), out.size()) == 0);
    assert(Read(state, UINT64_MAX-16, out.data(), out.size()) == 0);
    process.memory.readable = false;
    assert(Read(state, uintptr_t(p), out.data(), out.size()) == 0);
    process.memory.readable = true;
    // VMM still claims readability, simulating a concurrent protection change.
    assert(mprotect(p+page, page, PROT_NONE) == 0);
    // /proc/self/mem can read still-mapped PROT_NONE pages. Either reader must
    // remain bounded and must not turn a stale VMM permission into SIGSEGV.
    const auto protectedCount = Read(state, uintptr_t(p+page), out.data(), out.size());
    assert(protectedCount == 0 || protectedCount == out.size());
    const auto partialCount = Read(state, uintptr_t(p+page-64), out.data(), out.size());
    assert(partialCount == 64 || partialCount == out.size());
    assert(munmap(p+page, page) == 0);
    assert(Read(state, uintptr_t(p+page-64), out.data(), out.size()) == 64);
    assert(munmap(p, page) == 0);
    assert(Read(state, uintptr_t(p), out.data(), out.size()) == 0);
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path / 'test.cpp').write_text(fixture)
    subprocess.run(['g++', '-std=c++20', '-Wall', '-Wextra', '-Werror', str(path/'test.cpp'), '-o', str(path/'test')], check=True)
    subprocess.run([str(path/'test')], check=True)
print('PASS: diagnostic reader handles readable, protected, unmapped, partial, null and overflowing ranges without SIGSEGV')
