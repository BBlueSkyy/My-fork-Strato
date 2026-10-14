// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "../../fssrv/host/common.h"
#include <map>
namespace boost::container { template<class T, size_t N> using small_vector = std::vector<T>; }
namespace skyline {
    namespace kernel::type { class KProcess; struct KThread { u8 *tlsRegion{}; }; }
    namespace service { class BaseService; }
    struct DeviceState {
        std::shared_ptr<kernel::type::KProcess> process;
        std::shared_ptr<kernel::type::KThread> thread;
    };
    namespace util {
        template<class T> constexpr T MakeMagic(const char *s) {
            T result{}; for(size_t i=0;i<sizeof(T) && s[i];i++) result |= static_cast<T>(s[i]) << (i*8); return result;
        }
        template<class T> constexpr T AlignUp(T value,size_t alignment) { return (value+alignment-1)&~(alignment-1); }
        template<class T> constexpr T DivideCeil(T value,size_t divisor) { return (value+divisor-1)/divisor; }
    }
}
#define LOGV(...) ((void)0)
#define LOGD(...) ((void)0)
