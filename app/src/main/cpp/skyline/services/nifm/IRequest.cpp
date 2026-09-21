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
        LOGI("[NIFM-REQ] UpdateState enter old={} new={}", static_cast<u32>(requestState), static_cast<u32>(newState));

        if (requestState == newState) {
            LOGI("[NIFM-REQ] UpdateState no-op state={}", static_cast<u32>(requestState));
            return;
        }

        requestState = newState;
        LOGI("[NIFM-REQ] UpdateState before event0 Signal state={}", static_cast<u32>(requestState));
        event0->Signal();
        LOGI("[NIFM-REQ] UpdateState after event0 Signal state={}", static_cast<u32>(requestState));
    }

    Result IRequest::GetRequestState(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        LOGI("[NIFM-REQ] GetRequestState state={}", static_cast<u32>(requestState));
        response.Push(requestState);
        return {};
    }

    Result IRequest::GetResult(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        const bool hasConnection{*state.settings->isInternetEnabled};
        LOGI("[NIFM-REQ] GetResult enter state={} internet={}", static_cast<u32>(requestState), hasConnection);

        switch (requestState) {
            case RequestState::Invalid:
                LOGI("[NIFM-REQ] GetResult -> NetworkCommunicationDisabled");
                return result::NetworkCommunicationDisabled;
            case RequestState::Free:
                LOGI("[NIFM-REQ] GetResult Free -> {}", hasConnection ? "Success" : "NetworkCommunicationDisabled");
                return hasConnection ? Result{} : result::NetworkCommunicationDisabled;
            case RequestState::OnHold:
                LOGI("[NIFM-REQ] GetResult transition OnHold -> {}", hasConnection ? "Accepted" : "Invalid");
                UpdateState(hasConnection ? RequestState::Accepted : RequestState::Invalid);
                LOGI("[NIFM-REQ] GetResult after transition state={} -> PendingConnection", static_cast<u32>(requestState));
                return result::PendingConnection;
            case RequestState::Accepted:
            case RequestState::Blocking:
                LOGI("[NIFM-REQ] GetResult state={} -> Success", static_cast<u32>(requestState));
                return {};
        }

        LOGI("[NIFM-REQ] GetResult unexpected state={} -> Success", static_cast<u32>(requestState));
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
        LOGI("[NIFM-REQ] Submit enter state={}", static_cast<u32>(requestState));

        if (requestState == RequestState::Free) {
            LOGI("[NIFM-REQ] Submit before UpdateState Free -> OnHold");
            UpdateState(RequestState::OnHold);
            LOGI("[NIFM-REQ] Submit after UpdateState state={}", static_cast<u32>(requestState));
        } else {
            LOGI("[NIFM-REQ] Submit no transition state={}", static_cast<u32>(requestState));
        }

        LOGI("[NIFM-REQ] Submit return state={}", static_cast<u32>(requestState));
        return {};
    }

    Result IRequest::SetConnectionConfirmationOption(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        [[maybe_unused]] const auto option{request.Pop<u8>()};
        return {};
    }

    Result IRequest::GetAppletInfo(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        [[maybe_unused]] const auto themeColor{request.Pop<u32>()};
        return result::AppletLaunchNotRequired;
    }
}
