// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <common.h>
namespace skyline::kernel {
    namespace type { struct KSession {}; }
    namespace ipc {
        struct IpcRequest {
            std::vector<u8> arguments;
            size_t cmdArgSz{}, position{};
            std::vector<KHandle> copyHandles, moveHandles;
            std::vector<span<u8>> inputBuf, outputBuf;
            template<class T> T &Pop() { auto &out=*reinterpret_cast<T*>(arguments.data()+position); position+=sizeof(T); return out; }
            template<class T> void SetArguments(const T &in) { arguments.resize(sizeof(T));std::memcpy(arguments.data(),&in,sizeof(T));cmdArgSz=sizeof(T);position=0; }
        };
        struct IpcResponse {
            std::vector<u8> payload;
            template<class T> void Push(const T &in) { const auto at=payload.size();payload.resize(at+sizeof(T));std::memcpy(payload.data()+at,&in,sizeof(T)); }
        };
    }
}
namespace skyline::service {
    using namespace kernel;
    class ServiceManager;
    class BaseService {
      public:
        const DeviceState &state; ServiceManager &manager;
        BaseService(const DeviceState &s,ServiceManager &m):state(s),manager(m) {}
        virtual ~BaseService()=default;
    };
    class ServiceManager {
      public:
        std::shared_ptr<BaseService> registered;
        void RegisterService(std::shared_ptr<BaseService> service,type::KSession &,ipc::IpcResponse &) {registered=std::move(service);}
    };
}
#define SERVICE_DECL(...)
#define SRVREG(cls,...) std::make_shared<cls>(state,manager,##__VA_ARGS__)
