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
            std::vector<u8> cmdStorage;
            std::vector<span<u8>> inputBuf;
            std::vector<span<u8>> outputBuf;
        };

        class IpcResponse {
          public:
            std::vector<u8> data;

            template<typename T>
            void Push(const T &value) {
                const auto offset{data.size()};
                data.resize(offset + sizeof(T));
                std::memcpy(data.data() + offset, &value, sizeof(T));
            }

            template<typename T>
            T Get(size_t offset = 0) const {
                T value{};
                std::memcpy(&value, data.data() + offset, sizeof(T));
                return value;
            }
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
