// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <common.h>
namespace skyline::kernel::type {
    class KProcess {
      public:
        u64 id{0x1234};
        struct {
            std::array<u8,0x100> backing{};
            span<u8> GetHostSpan(span<u8> guest) {
                auto offset=reinterpret_cast<uintptr_t>(guest.data())-0x10000;
                if(offset>backing.size() || guest.size()>backing.size()-offset) throw std::runtime_error("invalid buffer");
                return {backing.data()+offset,guest.size()};
            }
        } memory;
        template<class T> std::shared_ptr<T> GetHandle(KHandle) { throw std::runtime_error("not used"); }
    };
}
