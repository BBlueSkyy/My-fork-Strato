#!/usr/bin/env python3
"""Compile all production abort diagnostics with Linux memory/logger adapters.

Exercises relocated AArch64 PLT/static references, bounded ELF lookup, history
retention across exception unwinding, LR call-site resolution and observational
behavior. Uses synthetic code, not game binaries. Requires g++ and repo fmt.
"""
import re
import subprocess
import tempfile
from pathlib import Path

root = Path(__file__).resolve().parents[2]
skyline = root / 'app/src/main/cpp/skyline'
header = (skyline/'loader/loader.h').read_text()
loader = (skyline/'loader/loader.cpp').read_text()
descriptor = 'struct ExecutableSymbolicInfo {' + header.split('struct ExecutableSymbolicInfo {', 1)[1].split('\n        };', 1)[0] + '\n};'
symbol_info = 'struct SymbolInfo {' + header.split('struct SymbolInfo {', 1)[1].split('\n        };', 1)[0] + '\n};'
resolve = 'template<ElfSymbol ElfSym>\n' + loader.split('    template<ElfSymbol ElfSym>\n', 1)[1].split('    inline std::string GetFunctionStackTrace', 1)[0]
source = re.sub(r'^#include .*\n', '', (skyline/'nce/diagnostics.cpp').read_text(), flags=re.M)

fixture = r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <linux/elf.h>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <sys/mman.h>
#include <sys/uio.h>
#include <unistd.h>
#define FMT_HEADER_ONLY
#include <fmt/format.h>
#include <nce/diagnostic_instructions.h>
namespace skyline {
using u8=uint8_t; using u16=uint16_t; using u32=uint32_t;
using u64=uint64_t; using i64=int64_t;
template<typename T> struct span : std::span<T> {
    using std::span<T>::span;
    template<typename U> span<U> cast() const {
        assert(this->size_bytes() % sizeof(U) == 0);
        return {reinterpret_cast<U *>(this->data()), this->size_bytes()/sizeof(U)};
    }
};
template<typename T> span(std::vector<T>&)->span<T>;
namespace signal { struct SignalException : std::runtime_error { using std::runtime_error::runtime_error; }; }
namespace util { constexpr u64 ClockFrequency=19200000; u64 GetTimeNs() { return 123456789; } }
struct AsyncLogger {
    enum class LogLevel { Info };
    static inline std::vector<std::string> messages;
    static bool CheckLogLevel(LogLevel) { return true; }
    static void LogSync(LogLevel, std::string s, const char*) { messages.push_back(std::move(s)); }
};
namespace loader {
template<typename T> concept ElfSymbol = std::same_as<T, Elf64_Sym>;
class Loader {
public:
''' + descriptor + symbol_info + r'''
    std::vector<ExecutableSymbolicInfo> executables;
    bool throwSymbols{};
    std::vector<void *> lastFrames;
    template<ElfSymbol ElfSym> SymbolInfo ResolveSymbol(void *ptr);
    SymbolInfo ResolveSymbol64(void *ptr) { return ResolveSymbol<Elf64_Sym>(ptr); }
    std::vector<SymbolInfo> FindFunctionSymbols64(std::string_view nameFragment, size_t limit=16);
    std::string GetStackTrace(const std::vector<void *> &frames) {
        lastFrames=frames;
        if (throwSymbols) throw signal::SignalException("injected symbol lookup failure");
        return "synthetic stack";
    }
};
''' + resolve + r'''
}
struct Descriptor {
    size_t size;
    struct { bool r,w,x; u32 raw; } permission;
    struct { u32 value{},type{}; } state;
    struct { u32 value{}; } attributes;
};
struct Memory {
    u8 *base; size_t size;
    bool AddressSpaceContains(span<u8> s) const {
        return uintptr_t(s.data()) >= uintptr_t(base) && s.size() <= size &&
            uintptr_t(s.data()) - uintptr_t(base) <= size - s.size();
    }
    auto GetChunk(u8 *p) const -> std::optional<std::pair<u8 *,Descriptor>> {
        if (!AddressSpaceContains({p,1})) return {};
        if (p < base+0x1000) return std::pair{base,Descriptor{0x1000,{true,false,true,5},{},{}}};
        return std::pair{base+0x1000,Descriptor{size-0x1000,{true,true,false,3},{},{}}};
    }
    auto GetHostSpan(span<u8> s) const { return s; }
};
struct Process { Memory memory; };
struct Thread { u64 id{1}; };
struct DeviceState { Process *process; Thread *thread; loader::Loader *loader; };
namespace kernel::type { struct KSharedMemory { span<u8> guest,host; }; }
namespace nce {
struct ThreadContext {
    union Gp { std::array<u64,19> regs{}; struct { u64 x0,x1; }; } gpr;
    u32 nzcv{};
    const DeviceState *state{};
    struct { std::array<u64,12> x19ToX30{}; u64 sp{},trampolineLr{}; } callSite;
};
}
}
''' + source + r'''
int main() {
    using namespace skyline;
    using namespace skyline::nce::diagnostics;
    using namespace skyline::nce::diagnostics::instructions;
    assert(DirectBranch(0x94000004,0x1000) == 0x1010);
    assert(DirectBranch(0x17FFFFFF,0x1000) == 0xFFC);
    assert(!DirectBranch(0x54000080,0x1000)); // B.eq
    assert(!DirectBranch(0xD61F0220,0x1000)); // BR x17
    assert(DataReference(0xB0000010,0xF9400A11,0x12345080)->address == 0x12346010);
    assert(DataReference(0xF0FFFFF0,0xF9400A11,0x12345080)->address == 0x12344010);
    assert(!DataReference(0xB0000010,0xF9400A31,0x12345080)); // wrong base
    assert(!DataReference(0xB0000010,0xF9000A11,0x12345080)); // store
    assert(!DataReference(0xB0000010,0xFD400A11,0x12345080)); // SIMD load
    auto p=static_cast<u8 *>(mmap(nullptr,0x3000,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
    assert(p != MAP_FAILED);
    const u64 address=reinterpret_cast<u64>(p);
    const std::array<u32,5> body{0x94000070,0xB0000009,0xF9401128,0x17FFFFFD,0xD65F03C0};
    const std::array<u32,4> plt{0xB0000010,0xF9400A11,0x91004210,0xD61F0220};
    std::memcpy(p+0x40,body.data(),sizeof(body));
    std::memcpy(p+0x200,plt.data(),sizeof(plt));
    const u32 ret=0xD65F03C0; std::memcpy(p+0x500,&ret,4);
    const u64 helper=address+0x500,object=address+0x1180;
    std::memcpy(p+0x1010,&helper,8); std::memcpy(p+0x1020,&object,8);
    std::memcpy(p+0x1180,"clock ready",12);
    assert(mprotect(p,0x1000,PROT_READ|PROT_EXEC) == 0);
    loader::Loader loader;
    constexpr char symbolNames[]="\0clock_gettime\0guest_clock_helper\0unterminated_clock_gettime";
    std::string names(symbolNames,sizeof(symbolNames)-1);
    std::vector<Elf64_Sym> symbols{
        {1,STT_FUNC,0,1,0x40,0x30}, {15,STT_FUNC,0,1,0x500,4},
        {1,STT_FUNC,0,SHN_UNDEF,0x800,4}, {1,STT_FUNC,0,1,UINT64_MAX-3,16},
        {1,STT_FUNC,0,1,0x1000,UINT64_MAX}, {999,STT_FUNC,0,1,0x800,4},
        {34,STT_FUNC,0,1,0x900,4}
    };
    loader::Loader::ExecutableSymbolicInfo module{p,p,p,p+0x3000,"sdk.nso","sdk.nso.patch","sdk.nso.hook",{}, {names.begin(),names.end()}};
    module.symbols.resize(symbols.size()*sizeof(Elf64_Sym));
    std::memcpy(module.symbols.data(),symbols.data(),module.symbols.size());
    loader.executables.push_back(std::move(module));
    assert(loader.FindFunctionSymbols64("clock_gettime").size() == 1);
    assert(loader.FindFunctionSymbols64("clock",1).size() == 1);
    assert(loader.FindFunctionSymbols64("").empty());
    assert(loader.FindFunctionSymbols64("clock",0).empty());
    Process process{{p,0x3000}}; Thread thread;
    DeviceState state{&process,&thread,&loader};
    nce::ThreadContext ctx{}; ctx.state=&state;
    std::array<u8,0x3000> before{}; std::memcpy(before.data(),p,before.size());
    errno=EAGAIN;
    Diagnostic("fixture functions",[&]{ DumpFunctions(state,{"clock_gettime"}); });
    auto contains=[](std::string_view needle) {
        return std::any_of(AsyncLogger::messages.begin(),AsyncLogger::messages.end(),[&](const auto &s){return s.find(needle)!=s.npos;});
    };
    assert(contains("name=guest_clock_helper module=sdk.nso"));
    assert(contains("Static reference pointee"));
    assert(contains("clock ready"));
    assert(contains("bodies=2"));
    assert(errno == EAGAIN && std::memcmp(p,before.data(),before.size()) == 0);

    trace.current={.sequence=1,.id=0x21};
    RecordIpc("ISystemClock::GetCurrentTime",0xCC74);
    EndSvc(state,ctx);
    for (size_t i=2;i<502;i++) { trace.current={.sequence=i,.id=0x6}; EndSvc(state,ctx); }
    assert(trace.calls[(trace.callCount-1)%trace.calls.size()].sequence == 501);
    assert(trace.nonQueryCount == 1 && trace.nonQueryCalls[0].sequence == 1);
    AsyncLogger::messages.clear(); DumpHistory();
    assert(contains("seq=1 svc=0x21") && contains("ISystemClock::GetCurrentTime result=0xCC74"));

    auto memory=std::make_shared<kernel::type::KSharedMemory>();
    memory->host={p+0x1000,0x1000}; memory->guest=memory->host;
    WatchTimeSharedMemory(memory);
    trace.pc=address+0x44; trace.sp=address+0x2000; trace.registers[30]=address+0x70;
    Diagnostic("fixture stack",[&]{ DumpContext(state,"fixture"); });
    assert(loader.lastFrames[0] == reinterpret_cast<void *>(trace.pc));
    assert(loader.lastFrames[1] == p+0x6C); // boundary LR belongs to previous function
    const u32 payload=0xCC74; std::memcpy(p+0x2100,&payload,4);
    std::memcpy(before.data(),p,before.size());
    const auto registers=trace.registers;
    loader.throwSymbols=true; AsyncLogger::messages.clear(); errno=EDOM;
    DumpBreak(state,0,address+0x2100,4);
    assert(contains("Break info u32=0xCC74"));
    assert(contains("injected symbol lookup failure"));
    assert(contains("Time shmem +0x1E0")); // survives symbol failure
    assert(errno == EDOM && trace.registers == registers);
    assert(std::memcmp(p,before.data(),before.size()) == 0);
    memory.reset(); assert(trace.timeSharedMemory.expired());
    assert(munmap(p,0x3000) == 0);
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path/'test.cpp').write_text(fixture)
    subprocess.run(['g++','-std=c++20','-O1','-Wall','-Wextra','-Werror',
                    '-Wno-missing-field-initializers','-I',str(skyline),
                    '-I',str(root/'app/libraries/fmt/include'),str(path/'test.cpp'),
                    '-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
print('PASS: production snapshots resolve synthetic PLT/data, bound symbol lookup, retain pre-unwind history, use LR-4, survive symbol failures and preserve state/errno')
