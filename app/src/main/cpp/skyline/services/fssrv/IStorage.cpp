// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include "results.h"
#include "IStorage.h"

namespace skyline::service::fssrv {
    IStorage::IStorage(std::shared_ptr<vfs::Backing> backing, const DeviceState &state, ServiceManager &manager) : backing(std::move(backing)), BaseService(state, manager) {}

    Result IStorage::Read(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto offset{request.Pop<u64>()};
        auto size{request.Pop<u64>()};

        if (request.outputBuf.empty())
            return {};

        auto &output{request.outputBuf.at(0)};

        // The 'size' requested by the guest may not match the actual size of the output buffer
        // (corrupted/incomplete dump, malformed buffer descriptor, etc.). Previously, this value
        // was read and discarded, and the backing->Read() blindly relied on the entire span of the
        // output buffer, which could lead to unexpected reads and silent memory corruption later on (invalid span size elsewhere in the code).
        auto readSize{std::min(static_cast<size_t>(size), output.size())};
        if (readSize == 0)
            return {};

        backing->Read(output.subspan(0, readSize), static_cast<size_t>(offset));

        return {};
    }

    Result IStorage::GetSize(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        response.Push<u64>(backing->size);

        return {};
    }
}
