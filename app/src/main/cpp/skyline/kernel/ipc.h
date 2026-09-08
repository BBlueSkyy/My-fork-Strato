// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <common.h>
#include "types/KSession.h"
#include "types/KProcess.h"

namespace skyline {
    namespace constant {
        constexpr u8 IpcPaddingSum{0x10}; // The sum of the padding surrounding the data payload
        constexpr u16 TlsIpcSize{0x100}; // The size of the IPC command buffer in a TLS slot
    }

    namespace kernel::ipc {
        /**
         * @url https://switchbrew.org/wiki/IPC_Marshalling#Type
         */
        enum class CommandType : u16 {
            Invalid = 0,
            LegacyRequest = 1,
            Close = 2,
            LegacyControl = 3,
            Request = 4,
            Control = 5,
            RequestWithContext = 6,
            ControlWithContext = 7,
            TipcCloseSession = 0xF,
        };

        enum class BufferCFlag : u8 {
            None = 0,
            InlineDescriptor = 1,
            SingleDescriptor = 2,
        };

        struct CommandHeader {
            CommandType type : 16;
            u8 xNo : 4;
            u8 aNo : 4;
            u8 bNo : 4;
            u8 wNo : 4;
            u32 rawSize : 10;
            BufferCFlag cFlag : 4;
            u32               : 17;
            bool handleDesc : 1;
        };
        static_assert(sizeof(CommandHeader) == 8);

        struct HandleDescriptor {
            bool sendPid : 1;
            u32 copyCount : 4;
            u32 moveCount : 4;
            u32           : 23;
        };
        static_assert(sizeof(HandleDescriptor) == 4);

        enum class DomainCommand : u8 {
            SendMessage = 1,
            CloseVHandle = 2,
        };

        struct DomainHeaderRequest {
            DomainCommand command;
            u8 inputCount;
            u16 payloadSz;
            u32 objectId;
            u32 : 32;
            u32 token;
        };
        static_assert(sizeof(DomainHeaderRequest) == 16);

        struct DomainHeaderResponse {
            u32 outputCount;
            u32 : 32;
            u64 : 64;
        };
        static_assert(sizeof(DomainHeaderResponse) == 16);

        struct PayloadHeader {
            u32 magic;
            u32 version;
            u32 value;
            u32 token;
        };
        static_assert(sizeof(PayloadHeader) == 16);

        enum class ControlCommand : u32 {
            ConvertCurrentObjectToDomain = 0,
            CopyFromCurrentDomain = 1,
            CloneCurrentObject = 2,
            QueryPointerBufferSize = 3,
            CloneCurrentObjectEx = 4,
        };

        struct BufferDescriptorX {
            u16 counter0_5 : 6;
            u16 address36_38 : 3;
            u16 counter9_11 : 3;
            u16 address32_35 : 4;
            u16 size : 16;
            u32 address0_31 : 32;

            u8 *Pointer() {
                return reinterpret_cast<u8 *>(static_cast<u64>(address0_31) | static_cast<u64>(address32_35) << 32 | static_cast<u64>(address36_38) << 36);
            }

            u16 Counter() {
                return static_cast<u16>(counter0_5) | static_cast<u16>(static_cast<u16>(counter9_11) << 9);
            }
        };
        static_assert(sizeof(BufferDescriptorX) == 8);

        struct BufferDescriptorABW {
            u32 size0_31 : 32;
            u32 address0_31 : 32;
            u8 flags : 2;
            u8 address36_38 : 3;
            u32             : 19;
            u8 size32_35 : 4;
            u8 address32_35 : 4;

            u8 *Pointer() {
                return reinterpret_cast<u8 *>(static_cast<u64>(address0_31) | static_cast<u64>(address32_35) << 32 | static_cast<u64>(address36_38) << 36);
            }

            u64 Size() {
                return static_cast<u64>(size0_31) | static_cast<u64>(size32_35) << 32;
            }
        };
        static_assert(sizeof(BufferDescriptorABW) == 12);

        struct BufferDescriptorC {
            u64 address : 48;
            u32 size : 16;

            u8 *Pointer() {
                return reinterpret_cast<u8 *>(address);
            }
        };
        static_assert(sizeof(BufferDescriptorC) == 8);

        enum class IpcBufferType {
            X,
            A,
            B,
            W,
            C,
        };

        class IpcRequest {
          private:
            u8 *payloadOffset;

          public:
            CommandHeader *header{};
            HandleDescriptor *handleDesc{};
            u64 pid{}; //!< PID supplied by the client when HandleDescriptor::sendPid is set
            bool isDomain{};
            bool isTipc;
            DomainHeaderRequest *domain{};
            PayloadHeader *payload{};
            u8 *cmdArg{};
            u64 cmdArgSz{};
            boost::container::small_vector<KHandle, 2> copyHandles;
            boost::container::small_vector<KHandle, 2> moveHandles;
            boost::container::small_vector<KHandle, 2> domainObjects;
            boost::container::small_vector<span<u8>, 3> inputBuf;
            boost::container::small_vector<span<u8>, 3> outputBuf;

            IpcRequest(bool isDomain, const DeviceState &state);

            template<typename ValueType>
            ValueType &Pop() {
                ValueType &value{*reinterpret_cast<ValueType *>(payloadOffset)};
                payloadOffset += sizeof(ValueType);
                return value;
            }

            std::string_view PopString(size_t size = 0, bool nullTerminated = true) {
                size = size ? size : cmdArgSz - reinterpret_cast<u64>(payloadOffset);
                auto view{span(payloadOffset, size).as_string(nullTerminated)};
                if (nullTerminated)
                    payloadOffset += size;
                else
                    payloadOffset += view.length();
                return view;
            }

            template<typename ServiceType>
            std::shared_ptr<ServiceType> PopService(u32 id, type::KSession &session) {
                std::shared_ptr<service::BaseService> serviceObject;
                if (session.isDomain)
                    serviceObject = session.domains.at(domainObjects.at(id));
                else
                    serviceObject = session.state.process->GetHandle<kernel::type::KSession>(moveHandles.at(id))->serviceObject;

                return std::static_pointer_cast<ServiceType>(serviceObject);
            }

            template<typename ValueType>
            void Skip() {
                payloadOffset += sizeof(ValueType);
            }
        };

        class IpcResponse {
          private:
            const DeviceState &state;
            std::vector<u8> payload;

          public:
            Result errorCode{};
            boost::container::small_vector<KHandle, 2> copyHandles;
            boost::container::small_vector<KHandle, 2> moveHandles;
            boost::container::small_vector<KHandle, 2> domainObjects;

            IpcResponse(const DeviceState &state);

            template<typename ValueType>
            void Push(const ValueType &value) {
                auto size{payload.size()};
                payload.resize(size + sizeof(ValueType));
                std::memcpy(payload.data() + size, reinterpret_cast<const u8 *>(&value), sizeof(ValueType));
            }

            void Push(std::string_view string) {
                auto size{payload.size()};
                payload.resize(size + string.size());
                std::memcpy(payload.data() + size, string.data(), string.size());
            }

            void WriteResponse(bool isDomain, bool isTipc = false);
        };
    }
}
