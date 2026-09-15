// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <mutex>
#include <unordered_map>
#include <kernel/types/KSession.h>
#include <services/am/applet_state.h>
#include "base_service.h"

namespace skyline::service {
    struct GlobalServiceState;

    class ServiceManager {
      private:
        const DeviceState &state;
        std::unordered_map<ServiceName, std::shared_ptr<BaseService>> serviceMap;
        std::mutex mutex;
        std::mutex appletStateMutex;
        std::unordered_map<u64, std::shared_ptr<am::AppletState>> appletStates;

      public:
        std::shared_ptr<BaseService> smUserInterface;
        std::shared_ptr<GlobalServiceState> globalServiceState;

        ServiceManager(const DeviceState &state);

        std::shared_ptr<BaseService> NewService(ServiceName name, type::KSession &session, ipc::IpcResponse &response);
        void RegisterService(std::shared_ptr<BaseService> serviceObject, type::KSession &session, ipc::IpcResponse &response);

        template<typename ServiceType>
        void RegisterService(std::shared_ptr<ServiceType> serviceObject, type::KSession &session, ipc::IpcResponse &response) {
            RegisterService(std::static_pointer_cast<BaseService>(serviceObject), session, response);
        }

        std::shared_ptr<BaseService> CreateOrGetService(ServiceName name);

        template<typename Type>
        constexpr std::shared_ptr<Type> CreateOrGetService(std::string_view name) {
            return std::static_pointer_cast<Type>(CreateOrGetService(util::MakeMagic<ServiceName>(name)));
        }

        std::shared_ptr<am::AppletState> GetOrCreateAppletState(u64 appletResourceUserId) {
            std::scoped_lock lock{appletStateMutex};
            auto &appletState{appletStates[appletResourceUserId]};
            if (!appletState) {
                appletState = std::make_shared<am::AppletState>(state);
                appletState->appletResourceUserId = appletResourceUserId;
            }
            return appletState;
        }

        void CloseSession(KHandle handle);
        void SyncRequestHandler(KHandle handle);
    };
}
