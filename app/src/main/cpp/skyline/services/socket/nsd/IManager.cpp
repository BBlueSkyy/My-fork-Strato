// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include "IManager.h"

namespace skyline::service::socket {
    IManager::IManager(const DeviceState &state, ServiceManager &manager) : BaseService(state, manager) {}

    Result IManager::ResolveEx(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
       return {};
     }
 
     Result IManager::GetEnvironmentIdentifier(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
     struct EnvironmentIdentifier {
        std::array<char, 8> identifier{};
    };

    EnvironmentIdentifier identifier{};
    std::string_view value{"lp1"};
    std::copy(value.begin(), value.end(), identifier.identifier.begin());

    request.outputBuf.at(0).as<EnvironmentIdentifier>() = identifier;
         return {};
      }    
  }     
