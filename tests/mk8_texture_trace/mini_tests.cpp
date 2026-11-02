#include "gpu/texture/mini_capture.h"
#include <cassert>
#include <sstream>
using namespace skyline::gpu::texture::diagnostic;
int main() {
    std::weak_ptr<int> allocation;
    {
        MiniCapture owned;
        auto resource = std::make_shared<int>(1); allocation=resource;
        Snapshot metadata{};
        owned.Commit(1,1,metadata,[resource] { return std::vector<uint8_t>{1}; });
        resource.reset(); assert(!allocation.expired());
    }
    assert(allocation.expired()); // owner releases retained resources before its allocator/device
    MiniCapture capture; Snapshot s{};
    assert(!capture.Reserve(1,0)); assert(!capture.Arm(2));
    assert(capture.Arm(1));
    for(uint64_t i=1;i<=8;++i) {
        assert(capture.Reserve(i,100)==1);
        assert(!capture.Reserve(i,100));
        s.id=i; capture.Commit(1,100,s,[]{return std::vector<uint8_t>{0,10,255};});
    }
    assert(!capture.Reserve(9,100)); assert(!capture.Arm(1));
    std::ostringstream incomplete; assert(!capture.Export(incomplete)); assert(incomplete.str().empty());
    assert(capture.Arm(3)); assert(capture.Reserve(1,200)==3);
    assert(!capture.Reserve(2,201));
    capture.Commit(3,200,s,[]{return std::vector<uint8_t>{1,2,3};});
    std::ostringstream complete; assert(capture.Export(complete));
    assert(complete.str().starts_with("MK8MINI1\n"));
    assert(complete.str().find(std::string("\0\n\xff",3))!=std::string::npos);
    capture.Reset(); assert(!capture.Armed()); assert(capture.Arm(1));
}
