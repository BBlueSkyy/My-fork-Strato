#include "gpu/ssbo_range_ab.h"
#include <cassert>
#include <thread>
#include <vector>
#include <iostream>
using namespace skyline::gpu::ssbo_range_ab;
int main(){
 auto digest=ModuleDigest;
 assert(ModuleCertified(ShaderHash,true,true,digest));
 assert(!ModuleCertified(ShaderHash,false,true,digest));
 assert(!ModuleCertified(ShaderHash,true,false,digest));
 assert(!ModuleCertified(0,true,true,digest));
 digest[31]^=1;assert(!ModuleCertified(ShaderHash,true,true,digest));
 Table t;Key k{.address=0x1000,.slot=0};auto id=t.Prepare(k,false);t.Resolve(id,4608,4096,8470528,false);
 std::vector<std::thread> workers;
 for(int n=0;n<4;++n)workers.emplace_back([&]{for(int i=0;i<1000;++i){auto x=t.Prepare(k,false);t.Resolve(x,4608,4096,8470528,false);}});
 for(auto& w:workers)w.join();
 assert(t.rows[id].prepared==4001&&t.rows[id].resolved==4001&&t.rows[id].rangeMin==4608&&t.rows[id].backingMax==8470528);
 for(unsigned n=0;n<128;++n){k.address=0x100000+4096*n;t.Prepare(k,false);}
 assert(t.dropped>=1);
 for(const auto& row:t.rows)if(row.state.load(std::memory_order_acquire)==2)assert(row.key.address!=0);
 std::cout<<"PASS: certificate fail-closed, concurrent row attribution, range/backing separation and overflow\n";
}
