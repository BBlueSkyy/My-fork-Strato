// SPDX-License-Identifier: MPL-2.0
#include <cstdlib>
#include <new>
namespace resource_bench {
    inline thread_local bool countAllocations{};
    inline thread_local unsigned long long allocations{};
    inline void *Allocate(std::size_t size, std::size_t alignment=0) {
        if(countAllocations)++allocations;
        void *p{};
        if(alignment){if(alignment<sizeof(void *))alignment=sizeof(void *);if(posix_memalign(&p,alignment,size?size:1))p=nullptr;}
        else p=std::malloc(size?size:1);
        if(!p)throw std::bad_alloc{};
        return p;
    }
}
void *operator new(std::size_t size){return resource_bench::Allocate(size);}
void *operator new[](std::size_t size){return resource_bench::Allocate(size);}
void operator delete(void *p)noexcept{std::free(p);}
void operator delete[](void *p)noexcept{std::free(p);}
void operator delete(void *p,std::size_t)noexcept{std::free(p);}
void operator delete[](void *p,std::size_t)noexcept{std::free(p);}
void *operator new(std::size_t size,std::align_val_t alignment){return resource_bench::Allocate(size,static_cast<std::size_t>(alignment));}
void *operator new[](std::size_t size,std::align_val_t alignment){return resource_bench::Allocate(size,static_cast<std::size_t>(alignment));}
void operator delete(void *p,std::align_val_t)noexcept{std::free(p);}
void operator delete[](void *p,std::align_val_t)noexcept{std::free(p);}
void operator delete(void *p,std::size_t,std::align_val_t)noexcept{std::free(p);}
void operator delete[](void *p,std::size_t,std::align_val_t)noexcept{std::free(p);}
