#include <cassert>
#include <iostream>
#ifdef BASELINE
#include <cstdint>
constexpr uint64_t SelectedRange(uint64_t modern,uint64_t,unsigned,bool){return modern;}
#else
#include "gpu/ssbo_range_ab.h"
using skyline::gpu::ssbo_range_ab::SelectedRange;
#endif
int main(){
 assert(SelectedRange(8388608,4608,0,true)==4608);
 assert(SelectedRange(8388608,4864,0,true)==4864);
 assert(SelectedRange(8388608,4608,0,false)==8388608);
 assert(SelectedRange(8388608,4608,1,true)==8388608);
 assert(SelectedRange(2048,4608,0,true)==2048);
 std::cout<<"PASS: certified extent only, padding, refusal, modern undersized range unchanged\n";
}
