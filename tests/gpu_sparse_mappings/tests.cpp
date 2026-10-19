// SPDX-License-Identifier: MPL-2.0
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>
namespace fmt { template<class T> auto ptr(T *p) { return p; } }
namespace skyline {
using u8=uint8_t;
template<class T> struct span:std::span<T> {
    using std::span<T>::span;
    span(std::span<T> s):std::span<T>(s){}
    bool contains(span other) const {
        auto start=reinterpret_cast<uintptr_t>(this->data()), value=reinterpret_cast<uintptr_t>(other.data());
        return value>=start && value-start<=this->size() && other.size()<=this->size()-(value-start);
    }
};
template<class... T> struct exception:std::runtime_error { exception(const char *m,T&&...):std::runtime_error(m){} };
template<class... T> exception(const char*,T&&...)->exception<T...>;
namespace constant { constexpr size_t PageSize=4096; }
namespace util { bool IsPageAligned(size_t v) { return !(v%constant::PageSize); } }
namespace kernel {
struct MemoryManager {
    span<u8> base;
    span<u8> CreateMirrorsWithSparse(const std::vector<span<u8>>&);
};
#include "mirror.inc"
}
}
int main() {
    using namespace skyline;
    constexpr size_t P=constant::PageSize;
    int fd=memfd_create("strato-sparse-mirror-test",MFD_CLOEXEC); assert(fd>=0 && !ftruncate(fd,P*4));
    auto physical=static_cast<u8*>(mmap(nullptr,P*4,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0)); assert(physical!=MAP_FAILED);
    std::fill_n(physical,P*4,0x12); std::fill_n(physical+P*3,P,0x34);
    kernel::MemoryManager memory{{physical,P*4}};
    auto view=memory.CreateMirrorsWithSparse({{physical+P*3,P},{static_cast<u8*>(nullptr),P},{physical,P}});
    assert(view.size()==P*3 && view[0]==0x34 && view[P]==0 && view[P*2]==0x12);
    physical[P*3+9]=0x56; assert(view[9]==0x56); // Resident pages are aliases, never snapshots.
    view[P*2+17]=0x78; assert(physical[17]==0x78);
    view[P+7]=0xFF; assert(physical[P+7]==0x12); // Missing pages have no guest write destination.
    auto other=memory.CreateMirrorsWithSparse({{static_cast<u8*>(nullptr),P}}); assert(other[7]==0);
    bool rejected{};
    try { memory.CreateMirrorsWithSparse({{physical+1,P}}); } catch(const std::runtime_error&) { rejected=true; }
    assert(rejected);
    rejected=false;
    try { memory.CreateMirrorsWithSparse({{reinterpret_cast<u8*>(0xCAFEBABE),P}}); } catch(const std::runtime_error&) { rejected=true; }
    assert(rejected);
    assert(!munmap(view.data(),view.size()) && !munmap(other.data(),other.size()) && !munmap(physical,P*4));
    close(fd);
    std::cout<<"PASS: disjoint resident aliases, sparse isolation and invalid mirror rejection\n";
}
