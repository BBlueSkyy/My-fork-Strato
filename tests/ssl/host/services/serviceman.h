// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <common.h>

#define SERVICE_DECL(...)

namespace skyline::kernel::type {
    class KSession {
      public:
        bool isDomain{};
    };
}

namespace skyline::kernel::ipc {
    struct IpcRequest {
        u8 *cmdArg{};
        u64 cmdArgSz{};
        std::vector<span<u8>> inputBuf;
        std::vector<span<u8>> outputBuf;
    };

    class IpcResponse {
      public:
        std::vector<u8> payload;

        template<typename T>
        void Push(const T &value) {
            const size_t offset{payload.size()};
            payload.resize(offset + sizeof(T));
            std::memcpy(payload.data() + offset, &value, sizeof(value));
        }
    };
}

namespace skyline::service {
    using namespace kernel;

    class BaseService;

    class ServiceManager {
      public:
        std::vector<std::shared_ptr<BaseService>> registered;
        bool failRegistration{};

        template<typename T>
        void RegisterService(std::shared_ptr<T> service, type::KSession &, ipc::IpcResponse &) {
            if (failRegistration)
                throw std::runtime_error("injected registration failure");
            registered.emplace_back(std::move(service));
        }
    };

    class BaseService {
      protected:
        const DeviceState &state;
        ServiceManager &manager;

      public:
        BaseService(const DeviceState &state, ServiceManager &manager) : state(state), manager(manager) {}
        virtual ~BaseService() = default;
        virtual void OnSessionClosed(const type::KSession &) {}
    };
}
