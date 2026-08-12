// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <kernel/types/KProcess.h>
#include "IRequest.h"
#include <common/settings.h>

namespace skyline::service::nifm {
    namespace result {
        constexpr Result AppletLaunchNotRequired{110, 180};
    }

    IRequest::IRequest(const DeviceState &state, ServiceManager &manager)
        : event0(std::make_shared<type::KEvent>(state, false)),
          event1(std::make_shared<type::KEvent>(state, false)),
          requestState(RequestState::Free),
          BaseService(state, manager) {}

    Result IRequest::GetRequestState(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        response.Push(requestState);
        return {};
    }

    Result IRequest::GetResult(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
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
        // Cancelar volta pro estado de "nenhum request ativo" (Free), não Invalid
        // (Invalid é reservado pra quando um Submit é rejeitado por falta de rede).
        // Também acordamos quem estiver esperando em WaitSynchronization no handle 0
        // (ver GetSystemEventReadableHandles), senão o jogo trava esperando pra sempre.
        requestState = RequestState::Free;
        event0->Signal();
        return {};
    }

    Result IRequest::Submit(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        // No hardware real isso dispara um processo assíncrono de conexão; aqui resolvemos
        // na hora já que não emulamos handshake de rede de verdade.
        requestState = *state.settings->isInternetEnabled ? RequestState::Accepted : RequestState::Invalid;

        // Sinaliza o handle 0 retornado por GetSystemEventReadableHandles, pra qualquer
        // WaitSynchronization pendente acordar em vez de travar esperando pra sempre.
        event0->Signal();

        return {};
    }

    Result IRequest::SetConnectionConfirmationOption(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        return {};
    }

    Result IRequest::GetAppletInfo(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        return result::AppletLaunchNotRequired;
    }
}
