// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <kernel/types/KProcess.h>
#include <cstring>
#include "helpers.h"
#include "results.h"
#include "IDirectory.h"

namespace skyline::service::fssrv {
    struct __attribute__((packed)) DirectoryEntry {
        std::array<char, 0x301> name;

        struct {
            bool directory : 1;
            bool archive : 1;
            u8 _pad_ : 6;
        } attributes;

        u16 _pad0_;
        vfs::Directory::EntryType type;
        u8 _pad1_[3];
        u64 size;
    };
    static_assert(sizeof(DirectoryEntry) == 0x310);

    IDirectory::IDirectory(std::shared_ptr<vfs::Directory> backing, std::shared_ptr<vfs::FileSystem> backingFs, const DeviceState &state, ServiceManager &manager)
        : BaseService(state, manager), backing(std::move(backing)), backingFs(std::move(backingFs)), entries(this->backing->Read()) {}

    Result IDirectory::Read(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        if (request.outputBuf.empty()) {
            response.Push<u64>(0);
            return {};
        }
        auto outputEntries{request.outputBuf.at(0).cast<DirectoryEntry, std::dynamic_extent, true>()};
        const auto count{CalculateReadCount(entries.size(), cursor, outputEntries.size())};

        for (size_t i{}; i < count; ++i) {
            const auto &entry{entries.at(cursor + i)};
            DirectoryEntry output{};
            output.attributes.directory = (entry.type == vfs::Directory::EntryType::Directory);
            output.type = entry.type;
            output.size = entry.size;
            const auto nameSize{std::min(entry.name.size(), output.name.size() - 1)};
            std::memcpy(output.name.data(), entry.name.data(), nameSize);
            outputEntries[i] = output;
        }

        cursor += count;
        response.Push<u64>(count);
        return {};
    }

    Result IDirectory::GetEntryCount(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        response.Push<u64>(entries.size());
        return {};
    }
}
