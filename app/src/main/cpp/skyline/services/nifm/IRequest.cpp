// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <kernel/types/KProcess.h>
#include "IRequest.h"
#include <common/settings.h>

namespace skyline::service::nifm {
    namespace result {
        constexpr Result PendingConnection{110, 111};
        constexpr Result AppletLaunchNotRequired{110, 180};
        constexpr Result NetworkCommunicationDisabled{110, 1111};
    }

    IRequest::IRequest(const DeviceState &state, ServiceManager &manager)
        : event0(std::make_shared<type::KEvent>(state, false)),
          event1(std::make_shared<type::KEvent>(state, false)),
          BaseService(state, manager) {}

    void IRequest::UpdateState(RequestState newState) {
        if (requestState == newState)
            return;

        requestState = newState;
        event0->Signal();
    }

    Result IRequest::GetRequestState(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        response.Push(requestState);
        return {};
    }

    Result IRequest::GetResult(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        const bool hasConnection{*state.settings->isInternetEnabled};

        switch (requestState) {
            case RequestState::Invalid:
                return result::NetworkCommunicationDisabled;
            case RequestState::Free:
                return hasConnection ? Result{} : result::NetworkCommunicationDisabled;
            case RequestState::OnHold:
                UpdateState(hasConnection ? RequestState::Accepted : RequestState::Invalid);
                return result::PendingConnection;
            case RequestState::Accepted:
            case RequestState::Blocking:
                return {};
        }

        return {};
    }

    Result IRequest::GetSystemEventReadableHandles(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto handle{state.process->InsertItem(event0)};
        LOGD("Request Event 0 Handle: 0x{:X}", handle);
        response.copyHandles.push_back(handle);

        handle = state.process->InsertItem(event1);
        LOGD("Request Event 1 Handle: 0x{:X}", handle);
        response.copyHandles.push_back(handle);

        return {};
    }

    Result IRequest::Cancel(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        UpdateState(RequestState::Free);
        return {};
    }

    Result IRequest::Submit(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        if (requestState == RequestState::Free)
            UpdateState(RequestState::OnHold);
        return {};
    }

    Result IRequest::SetConnectionConfirmationOption(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        [[maybe_unused]] const auto option{request.Pop<u32>()};
        return {};
    }

    Result IRequest::GetAppletInfo(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        [[maybe_unused]] const auto themeColor{request.Pop<u32>()};
        return result::AppletLaunchNotRequired;
    }
}
