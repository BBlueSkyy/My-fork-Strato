// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <services/am/storage/VectorIStorage.h>
#include <services/am/storage/TransferMemoryIStorage.h>
#include <services/am/applet/ILibraryAppletAccessor.h>
#include <kernel/types/KTransferMemory.h>
#include <kernel/types/KProcess.h>
#include "ILibraryAppletCreator.h"

namespace skyline::service::am {
    namespace {
        constexpr Result ObjectInvalid{128, 500};
    }

    ILibraryAppletCreator::ILibraryAppletCreator(const DeviceState &state, ServiceManager &manager,
                                                 std::shared_ptr<AppletState> appletState)
        : BaseService(state, manager), appletState(std::move(appletState)) {}

    Result ILibraryAppletCreator::CreateLibraryApplet(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        const auto appletId{request.Pop<skyline::applet::AppletId>()};
        const auto appletMode{request.Pop<applet::LibraryAppletMode>()};
        LOGD("CreateLibraryApplet request: id=0x{:X}, mode=0x{:X}",
             static_cast<u32>(appletId), static_cast<u32>(appletMode));
        auto accessor{SRVREG(ILibraryAppletAccessor, appletId, appletMode)};
        manager.RegisterService(accessor, session, response);
        appletState->libraryAppletLaunchableEvent->Signal();
        return {};
    }

    Result ILibraryAppletCreator::CreateLibraryAppletEx(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        const auto appletId{request.Pop<skyline::applet::AppletId>()};
        const auto appletMode{request.Pop<applet::LibraryAppletMode>()};
        [[maybe_unused]] const u64 threadId{request.Pop<u64>()};
        LOGD("CreateLibraryAppletEx request: id=0x{:X}, mode=0x{:X}, thread=0x{:X}",
             static_cast<u32>(appletId), static_cast<u32>(appletMode), threadId);
        auto accessor{SRVREG(ILibraryAppletAccessor, appletId, appletMode)};
        manager.RegisterService(accessor, session, response);
        appletState->libraryAppletLaunchableEvent->Signal();
        return {};
    }

    Result ILibraryAppletCreator::CreateStorage(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        const i64 size{request.Pop<i64>()};
        if (size <= 0)
            return ObjectInvalid;
        manager.RegisterService(SRVREG(VectorIStorage, static_cast<size_t>(size)), session, response);
        return {};
    }

    Result ILibraryAppletCreator::CreateTransferMemoryStorage(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        const bool writable{request.Pop<u64>() != 0};
        const i64 size{request.Pop<i64>()};
        if (size <= 0 || request.copyHandles.empty())
            return ObjectInvalid;

        auto transferMemory{state.process->GetHandle<kernel::type::KTransferMemory>(request.copyHandles.at(0))};
        if (!transferMemory || static_cast<u64>(size) > transferMemory->host.size())
            return ObjectInvalid;

        manager.RegisterService(SRVREG(TransferMemoryIStorage, transferMemory, writable), session, response);
        return {};
    }

    Result ILibraryAppletCreator::CreateHandleStorage(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        const i64 size{request.Pop<i64>()};
        if (size <= 0 || request.copyHandles.empty())
            return ObjectInvalid;

        auto transferMemory{state.process->GetHandle<kernel::type::KTransferMemory>(request.copyHandles.at(0))};
        if (!transferMemory || static_cast<u64>(size) > transferMemory->host.size())
            return ObjectInvalid;

        manager.RegisterService(SRVREG(TransferMemoryIStorage, transferMemory, true), session, response);
        return {};
    }
}
