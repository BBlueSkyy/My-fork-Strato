// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <common.h>
namespace skyline::kernel::type { class KSession {}; }
namespace skyline::kernel::ipc {
    enum class CommandType : u16 { Request = 4, RequestWithContext = 6 };
    enum class BufferCFlag : u8 { None = 0, SingleDescriptor = 2 };
    struct CommandHeader {
        CommandType type : 16;
        u8 xNo : 4; u8 aNo : 4; u8 bNo : 4; u8 wNo : 4;
        u32 rawSize : 10; BufferCFlag cFlag : 4; u32 : 17; bool handleDesc : 1;
    };
    struct HandleDescriptor { bool sendPid : 1; u32 copyCount : 4; u32 moveCount : 4; u32 : 23; };
    struct DomainHeaderRequest { u8 command; u8 inputCount; u16 payloadSz; u32 objectId; u32 : 32; u32 token; };
    struct PayloadHeader { u32 magic; u32 version; u32 value; u32 token; };
    struct BufferDescriptorX { u8 bytes[8]; };
    struct BufferDescriptorABW { u8 bytes[12]; };
    static_assert(sizeof(CommandHeader) == 8 && sizeof(HandleDescriptor) == 4 && sizeof(DomainHeaderRequest) == 16);
    struct IpcRequest {
        std::shared_ptr<std::array<u8, 0x100>> wire{std::make_shared<std::array<u8, 0x100>>()};
        CommandHeader *header{reinterpret_cast<CommandHeader *>(wire->data())};
        HandleDescriptor *handleDesc{};
        bool isDomain{};
        bool isTipc{};
        DomainHeaderRequest *domain{};
        PayloadHeader *payload{reinterpret_cast<PayloadHeader *>(wire->data() + 16)};
        u8 *cmdArg{};
        u64 cmdArgSz{};
        std::vector<span<u8>> inputBuf;
        std::vector<span<u8>> outputBuf;
        IpcRequest() { header->type = CommandType::Request; header->rawSize = 8; cmdArg = wire->data() + 32; cmdArgSz = 32; }
    };
    struct IpcResponse {
        std::vector<u8> payload;
        template<class T> void Push(const T &value) {
            const size_t offset{payload.size()};
            payload.resize(offset + sizeof(T));
            std::memcpy(payload.data() + offset, &value, sizeof(T));
        }
    };
}
namespace skyline::service {
    using namespace kernel;
    class ServiceManager {};
    class BaseService {
      protected:
        const DeviceState &state;
        ServiceManager &manager;
      public:
        BaseService(const DeviceState &state, ServiceManager &manager) : state{state}, manager{manager} {}
        virtual ~BaseService() = default;
    };
}
// Exercise the production command registration as well as each IPC handler.
#define SFUNC(id, Class, Function) std::pair{skyline::u32{id}, &Class::Function}
#define SERVICE_DECL(...) \
public: \
    skyline::Result HandleRequest(skyline::u32 id, skyline::kernel::type::KSession &s, skyline::kernel::ipc::IpcRequest &q, skyline::kernel::ipc::IpcResponse &r) { \
        const std::array functions{__VA_ARGS__}; \
        for (const auto &entry : functions) if (entry.first == id) return (this->*entry.second)(s, q, r); \
        return skyline::Result{10, 221}; \
    }
