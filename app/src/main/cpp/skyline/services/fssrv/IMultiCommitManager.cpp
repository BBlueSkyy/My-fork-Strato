// SPDX-License-Identifier: MPL-2.0
// Copyright © 2023 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include "results.h"
#include "IMultiCommitManager.h"

namespace skyline::service::fssrv {
    IMultiCommitManager::IMultiCommitManager(const DeviceState &state, ServiceManager &manager) : BaseService(state, manager) {}

    Result IMultiCommitManager::Add(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &) {
        std::shared_ptr<BaseService> object;
        try {
            object = request.PopService<BaseService>(0, session);
        } catch (const std::out_of_range &) {
            return result::InvalidArgument;
        }

        auto fileSystem{std::dynamic_pointer_cast<IFileSystem>(object)};
        if (!fileSystem)
            return result::InvalidArgument;
        fileSystems.emplace_back(std::move(fileSystem));
        return {};
    }

    Result IMultiCommitManager::Commit(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
        for (const auto &fileSystem : fileSystems) {
            const auto result{fileSystem->CommitBacking()};
            if (result)
                return result;
        }
        return {};
    }
}
