// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <common.h>

#define SERVICE_DECL(...)
#define SFUNC(...)
#define SRVREG(className, ...) std::make_shared<className>(state, manager, ##__VA_ARGS__)

namespace skyline {
    struct DeviceState {};

    namespace kernel::type {
        class KSession {};
    }

    namespace ipc {
        class IpcRequest {
          public:
            u64 pid{};
            u8 *cmdArg{};
            u64 cmdArgSz{};
            std::vector<span<u8>> inputBuf;
            std::vector<span<u8>> outputBuf;
        };

        class IpcResponse {
          public:
            template<typename T>
            void Push(const T &) {}
        };
    }

    namespace service {
        using namespace kernel;

        class ServiceManager {
          public:
            template<typename Service>
            void RegisterService(std::shared_ptr<Service>, kernel::type::KSession &, ipc::IpcResponse &) {}
        };

        class BaseService {
          protected:
            const DeviceState &state;
            ServiceManager &manager;

          public:
            BaseService(const DeviceState &state, ServiceManager &manager) : state(state), manager(manager) {}
            virtual ~BaseService() = default;
        };
    }
}
