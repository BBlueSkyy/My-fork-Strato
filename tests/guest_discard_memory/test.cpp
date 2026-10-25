#define _GNU_SOURCE 1
#include <sys/mman.h>
#include <unistd.h>
#include <cstdint>
#include <cstring>
#include <cassert>
#include <cstdio>
#include <span>
#include <cerrno>
using u8=std::uint8_t;
namespace constant { constexpr std::size_t PageSize=4096; }
namespace util {
template<class T> T *AlignUp(T *p,std::size_t size){return reinterpret_cast<T *>((reinterpret_cast<uintptr_t>(p)+size-1)&~(size-1));}
template<class T> T *AlignDown(T *p,std::size_t size){return reinterpret_cast<T *>(reinterpret_cast<uintptr_t>(p)&~(size-1));}
}
template<class T> struct span:std::span<T>{using std::span<T>::span; auto end()const{struct Iterator{T*p;T*base(){return p;}};return Iterator{this->data()+this->size()};}};
#define LOGW(...) ((void)0)
#define LOGE(...) ((void)0)
struct MemoryManager{
 uintptr_t guestOffset{};
 span<u8>GetHostSpan(span<u8>g){return {reinterpret_cast<u8*>(reinterpret_cast<uintptr_t>(g.data())+guestOffset),g.size()};}
 void FreeMemory(span<u8>);
 bool FreeHostMemory(span<u8>);
};
#include "discard.inc"
int main(){
 constexpr auto page=constant::PageSize;
 int fd=memfd_create("discard-memory",0); assert(fd>=0 && !ftruncate(fd,page*4));
 auto *p=static_cast<u8*>(mmap(nullptr,page*4,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0));assert(p!=MAP_FAILED);
 MemoryManager m; m.guestOffset=page; // Deliberately nonidentity guest translation.
 std::memset(p,0x73,page*4);
 assert(m.FreeHostMemory({p+1,page*3-2})); // Only middle page is wholly inside span.
 assert(p[page-1]==0x73 && p[page]==0 && p[page*2]==0x73 && p[page*3]==0x73);
 std::memset(p,0x64,page*4);
 m.FreeMemory({reinterpret_cast<u8*>(reinterpret_cast<uintptr_t>(p)),page});
 assert(p[0]==0x64 && p[page]==0 && p[page*2]==0x64); // Guest API translates once.
 assert(!m.FreeHostMemory({p+1,page-2}));
 auto *privateMap=static_cast<u8*>(mmap(nullptr,page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));assert(privateMap!=MAP_FAILED);
 privateMap[0]=0x51;assert(!m.FreeHostMemory({privateMap,page}));assert(privateMap[0]==0x51);
 assert(!munmap(privateMap,page));assert(!munmap(p,page*4));assert(!close(fd));
 std::puts("PASS production discard: host mirror vs guest translation, interior pages, failed/no-op discard");
}
