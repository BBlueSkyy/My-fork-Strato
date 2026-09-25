// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Strato Contributors

#include <nce.h>
#include <kernel/types/KProcess.h>
#include <services/am/storage/IStorage.h>
#include "AppletDataBroker.h"
#include "NativeAppletContext.h"
#include "ILibraryAppletSelfAccessor.h"

namespace skyline::service::am {
    namespace {
        constexpr Result NotAvailable{128, 2};
    }

    ILibraryAppletSelfAccessor::ILibraryAppletSelfAccessor(
        const DeviceState &state, ServiceManager &manager, std::shared_ptr<AppletState> appletState)
        : BaseService(state, manager), appletState(std::move(appletState)),
          context(this->appletState->nativeAppletContext) {
        if (!context || !context->broker)
            throw exception("LibraryAppletSelfAccessor opened without a native applet context");
    }

    Result ILibraryAppletSelfAccessor::PopInData(
        type::KSession &session, ipc::IpcRequest &, ipc::IpcResponse &response) {
        auto storage{context->broker->PopNormalIn()};
        if (!storage)
            return NotAvailable;
        manager.RegisterService(storage, session, response);
        return {};
    }

    Result ILibraryAppletSelfAccessor::PushOutData(
        type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &) {
        context->broker->PushNormalOut(request.PopService<IStorage>(0, session));
        return {};
    }

    Result ILibraryAppletSelfAccessor::PopInteractiveInData(
        type::KSession &session, ipc::IpcRequest &, ipc::IpcResponse &response) {
        auto storage{context->broker->PopInteractiveIn()};
        if (!storage)
            return NotAvailable;
        manager.RegisterService(storage, session, response);
        return {};
    }

    Result ILibraryAppletSelfAccessor::PushInteractiveOutData(
        type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &) {
        context->broker->PushInteractiveOut(request.PopService<IStorage>(0, session));
        return {};
    }

    Result ILibraryAppletSelfAccessor::GetPopInDataEvent(
        type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        auto event{context->broker->NormalInEvent()};
        response.copyHandles.push_back(state.GetCurrentProcessPtr()->InsertItem(event));
        return {};
    }

    Result ILibraryAppletSelfAccessor::GetPopInteractiveInDataEvent(
        type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        auto event{context->broker->InteractiveInEvent()};
        response.copyHandles.push_back(state.GetCurrentProcessPtr()->InsertItem(event));
        return {};
    }

    Result ILibraryAppletSelfAccessor::ExitProcessAndReturn(
        type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
        context->exited.store(true, std::memory_order_release);
        context->broker->stateChangedEvent->Signal();
        throw nce::NCE::ExitException(true);
    }

    Result ILibraryAppletSelfAccessor::GetLibraryAppletInfo(
        type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        response.Push<u32>(context->appletId);
        response.Push<u32>(context->appletMode);
        return {};
    }

    Result ILibraryAppletSelfAccessor::GetMainAppletIdentityInfo(
        type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        response.Push(AppletIdentityInfo{context->callerAppletId, 0, context->callerApplicationId});
        return {};
    }

    Result ILibraryAppletSelfAccessor::CanUseApplicationCore(
        type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        response.Push<bool>(false);
        return {};
    }

    Result ILibraryAppletSelfAccessor::GetCallerAppletIdentityInfo(
        type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        response.Push(AppletIdentityInfo{context->callerAppletId, 0, context->callerApplicationId});
        return {};
    }

    Result ILibraryAppletSelfAccessor::GetDesirableKeyboardLayout(
        type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        response.Push<u32>(context->desirableKeyboardLayout);
        return {};
    }

    Result ILibraryAppletSelfAccessor::UnpopInData(
        type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &) {
        context->broker->PushFrontNormalIn(request.PopService<IStorage>(0, session));
        return {};
    }
}
