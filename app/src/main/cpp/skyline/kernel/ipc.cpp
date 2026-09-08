// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include "ipc.h"
#include "types/KProcess.h"

namespace skyline::kernel::ipc {
    IpcRequest::IpcRequest(bool isDomain, const DeviceState &state) : isDomain(isDomain) {
        auto tls{state.ctx->tpidrroEl0};
        u8 *pointer{tls};

        header = reinterpret_cast<CommandHeader *>(pointer);
        isTipc = static_cast<u16>(header->type) > static_cast<u16>(CommandType::TipcCloseSession);
        pointer += sizeof(CommandHeader);

        if (header->handleDesc) {
            handleDesc = reinterpret_cast<HandleDescriptor *>(pointer);
            pointer += sizeof(HandleDescriptor);
            if (handleDesc->sendPid) {
                pid = *reinterpret_cast<u64 *>(pointer);
                pointer += sizeof(u64);
            }
            for (u32 index{}; handleDesc->copyCount > index; index++) {
                copyHandles.push_back(*reinterpret_cast<KHandle *>(pointer));
                pointer += sizeof(KHandle);
            }
            for (u32 index{}; handleDesc->moveCount > index; index++) {
                moveHandles.push_back(*reinterpret_cast<KHandle *>(pointer));
                pointer += sizeof(KHandle);
            }
        }

        for (u8 index{}; header->xNo > index; index++) {
            auto bufX{reinterpret_cast<BufferDescriptorX *>(pointer)};
            if (bufX->Pointer()) {
                inputBuf.emplace_back(bufX->Pointer(), static_cast<u16>(bufX->size));
                LOGV("Buf X #{}: {}, 0x{:X}, #{}", index, fmt::ptr(bufX->Pointer()), static_cast<u16>(bufX->size), static_cast<u16>(bufX->Counter()));
            }
            pointer += sizeof(BufferDescriptorX);
        }

        for (u8 index{}; header->aNo > index; index++) {
            auto bufA{reinterpret_cast<BufferDescriptorABW *>(pointer)};
            if (bufA->Pointer()) {
                inputBuf.emplace_back(bufA->Pointer(), bufA->Size());
                LOGV("Buf A #{}: {}, 0x{:X}", index, fmt::ptr(bufA->Pointer()), static_cast<u64>(bufA->Size()));
            }
            pointer += sizeof(BufferDescriptorABW);
        }

        for (u8 index{}; header->bNo > index; index++) {
            auto bufB{reinterpret_cast<BufferDescriptorABW *>(pointer)};
            if (bufB->Pointer()) {
                outputBuf.emplace_back(bufB->Pointer(), bufB->Size());
                LOGV("Buf B #{}: {}, 0x{:X}", index, fmt::ptr(bufB->Pointer()), static_cast<u64>(bufB->Size()));
            }
            pointer += sizeof(BufferDescriptorABW);
        }

        for (u8 index{}; header->wNo > index; index++) {
            auto bufW{reinterpret_cast<BufferDescriptorABW *>(pointer)};
            if (bufW->Pointer()) {
                outputBuf.emplace_back(bufW->Pointer(), bufW->Size());
                outputBuf.emplace_back(bufW->Pointer(), bufW->Size());
                LOGV("Buf W #{}: {}, 0x{:X}", index, fmt::ptr(bufW->Pointer()), static_cast<u16>(bufW->Size()));
            }
            pointer += sizeof(BufferDescriptorABW);
        }

        auto bufCPointer{pointer + header->rawSize * sizeof(u32)};

        if (isTipc) {
            cmdArg = pointer;
            cmdArgSz = header->rawSize * sizeof(u32);
        } else {
            size_t offset{static_cast<size_t>(pointer - tls)};
            auto padding{util::AlignUp(offset, constant::IpcPaddingSum) - offset};
            pointer += padding;

            if (isDomain && (header->type == CommandType::Request || header->type == CommandType::RequestWithContext)) {
                domain = reinterpret_cast<DomainHeaderRequest *>(pointer);
                pointer += sizeof(DomainHeaderRequest);

                payload = reinterpret_cast<PayloadHeader *>(pointer);
                pointer += sizeof(PayloadHeader);

                cmdArg = pointer;
                cmdArgSz = domain->payloadSz - sizeof(PayloadHeader);
                pointer += cmdArgSz;

                for (u8 index{}; domain->inputCount > index; index++) {
                    domainObjects.push_back(*reinterpret_cast<KHandle *>(pointer));
                    pointer += sizeof(KHandle);
                }
            } else {
                payload = reinterpret_cast<PayloadHeader *>(pointer);
                pointer += sizeof(PayloadHeader);

                cmdArg = pointer;
                cmdArgSz = header->rawSize * sizeof(u32);
            }
        }

        payloadOffset = cmdArg;

        if (!isTipc && payload->magic != util::MakeMagic<u32>("SFCI") && (header->type != CommandType::Control && header->type != CommandType::ControlWithContext && header->type != CommandType::Close) && (!domain || domain->command != DomainCommand::CloseVHandle))
            LOGD("Unexpected Magic in PayloadHeader: 0x{:X}", static_cast<u32>(payload->magic));

        if (header->cFlag == BufferCFlag::SingleDescriptor) {
            auto bufC{reinterpret_cast<BufferDescriptorC *>(bufCPointer)};
            if (bufC->address) {
                outputBuf.emplace_back(bufC->Pointer(), static_cast<u16>(bufC->size));
                LOGV("Buf C: {}, 0x{:X}", fmt::ptr(bufC->Pointer()), static_cast<u16>(bufC->size));
            }
        } else if (header->cFlag > BufferCFlag::SingleDescriptor) {
            for (u8 index{}; (static_cast<u8>(header->cFlag) - 2) > index; index++) {
                auto bufC{reinterpret_cast<BufferDescriptorC *>(bufCPointer)};
                if (bufC->address) {
                    outputBuf.emplace_back(bufC->Pointer(), static_cast<u16>(bufC->size));
                    LOGV("Buf C #{}: {}, 0x{:X}", index, fmt::ptr(bufC->Pointer()), static_cast<u16>(bufC->size));
                }
                bufCPointer += sizeof(BufferDescriptorC);
            }
        }

        if (header->type == CommandType::Request || header->type == CommandType::RequestWithContext) {
            LOGV("Header: Input No: {}, Output No: {}, Raw Size: {}", inputBuf.size(), outputBuf.size(), static_cast<u64>(cmdArgSz));
            if (header->handleDesc)
                LOGV("Handle Descriptor: Send PID: {}, PID: 0x{:X}, Copy Count: {}, Move Count: {}", static_cast<bool>(handleDesc->sendPid), pid, static_cast<u32>(handleDesc->copyCount), static_cast<u32>(handleDesc->moveCount));
            if (isDomain)
                LOGV("Domain Header: Command: {}, Input Object Count: {}, Object ID: 0x{:X}", domain->command, domain->inputCount, domain->objectId);

            if (isTipc)
                LOGV("TIPC Command ID: 0x{:X}", static_cast<u16>(header->type));
            else
                LOGV("Command ID: 0x{:X}", static_cast<u32>(payload->value));
        }
    }

    IpcResponse::IpcResponse(const DeviceState &state) : state(state) {}

    void IpcResponse::WriteResponse(bool isDomain, bool isTipc) {
        auto tls{state.ctx->tpidrroEl0};
        u8 *pointer{tls};

        memset(tls, 0, constant::TlsIpcSize);

        auto header{reinterpret_cast<CommandHeader *>(pointer)};
        size_t sizeBytes{isTipc ? (payload.size() + sizeof(Result)) : (sizeof(PayloadHeader) + constant::IpcPaddingSum + payload.size() + (domainObjects.size() * sizeof(KHandle)) + (isDomain ? sizeof(DomainHeaderRequest) : 0))};
        header->rawSize = static_cast<u32>(util::DivideCeil(sizeBytes, sizeof(u32)));
        header->handleDesc = (!copyHandles.empty() || !moveHandles.empty());
        pointer += sizeof(CommandHeader);

        if (header->handleDesc) {
            auto handleDesc{reinterpret_cast<HandleDescriptor *>(pointer)};
            handleDesc->copyCount = static_cast<u8>(copyHandles.size());
            handleDesc->moveCount = static_cast<u8>(moveHandles.size());
            pointer += sizeof(HandleDescriptor);

            for (auto copyHandle : copyHandles) {
                *reinterpret_cast<KHandle *>(pointer) = copyHandle;
                pointer += sizeof(KHandle);
            }

            for (auto moveHandle : moveHandles) {
                *reinterpret_cast<KHandle *>(pointer) = moveHandle;
                pointer += sizeof(KHandle);
            }
        }

        if (isTipc) {
            *reinterpret_cast<Result *>(pointer) = errorCode;
            pointer += sizeof(Result);
            std::memcpy(pointer, payload.data(), payload.size());
        } else {
            size_t offset{static_cast<size_t>(pointer - tls)};
            auto padding{util::AlignUp(offset, constant::IpcPaddingSum) - offset};
            pointer += padding;

            if (isDomain) {
                auto domain{reinterpret_cast<DomainHeaderResponse *>(pointer)};
                domain->outputCount = static_cast<u32>(domainObjects.size());
                pointer += sizeof(DomainHeaderResponse);
            }

            auto payloadHeader{reinterpret_cast<PayloadHeader *>(pointer)};
            payloadHeader->magic = util::MakeMagic<u32>("SFCO");
            payloadHeader->version = 1;
            payloadHeader->value = errorCode;
            pointer += sizeof(PayloadHeader);

            if (!payload.empty())
                std::memcpy(pointer, payload.data(), payload.size());
            pointer += payload.size();

            if (isDomain) {
                for (auto &domainObject : domainObjects) {
                    *reinterpret_cast<KHandle *>(pointer) = domainObject;
                    pointer += sizeof(KHandle);
                }
            }
        }

        LOGV("Output: Raw Size: {}, Result: 0x{:X}, Copy Handles: {}, Move Handles: {}", static_cast<u32>(header->rawSize), static_cast<u32>(errorCode), copyHandles.size(), moveHandles.size());
    }
}
