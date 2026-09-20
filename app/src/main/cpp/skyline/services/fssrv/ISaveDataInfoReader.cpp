// SPDX-License-Identifier: MPL-2.0
// Copyright © 2023 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <cstring>
#include "helpers.h"
#include "ISaveDataInfoReader.h"

namespace skyline::service::fssrv {
    ISaveDataInfoReader::ISaveDataInfoReader(const DeviceState &state, ServiceManager &manager, std::vector<SaveDataInfo> entries, std::optional<SaveDataSpaceId> spaceFilter, bool onlyCache)
        : BaseService(state, manager), entries(std::move(entries)) {
        std::erase_if(this->entries, [&](const SaveDataInfo &entry) {
            return (spaceFilter && entry.spaceId != *spaceFilter) || (onlyCache && entry.type != SaveDataType::Cache);
        });
    }

    Result ISaveDataInfoReader::ReadSaveDataInfo(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        size_t capacity{};
        if (!request.outputBuf.empty())
            capacity = request.outputBuf[0].size_bytes() / sizeof(SaveDataInfo);
        const auto count{CalculateReadCount(entries.size(), cursor, capacity)};

        if (count != 0) {
            auto output{request.outputBuf[0].first(count * sizeof(SaveDataInfo))};
            std::memcpy(output.data(), entries.data() + cursor, count * sizeof(SaveDataInfo));
            cursor += count;
        }

        response.Push<i64>(static_cast<i64>(count));
        return {};
    }
}
