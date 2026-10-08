// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <kernel/memory.h>
#include <kernel/types/KObject.h>
namespace skyline::kernel::type {
    class KProcess : public KObject {
      public:
        MemoryManager memory;
        span<u8> mainThreadStack;
        struct { struct { size_t systemResourceSize{}; } meta; } npdm;
        std::map<KHandle,std::shared_ptr<KObject>> handles;
        template<class T> std::shared_ptr<T> GetHandle(KHandle handle) {
            const auto it=handles.find(handle); if(it==handles.end()) throw std::runtime_error("bad handle");
            return std::static_pointer_cast<T>(it->second);
        }
        explicit KProcess(const DeviceState &state):KObject(state,KType::KProcess),memory(state) {}
    };
}
