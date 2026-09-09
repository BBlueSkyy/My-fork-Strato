// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <common/language.h>
#include "ISettingsServer.h"
#include "ipc_helpers.h"
#include "ISystemSettingsServer.h"
#include <services/serviceman.h>
#include <common/settings.h>

namespace skyline::service::settings {
    ISettingsServer::ISettingsServer(const DeviceState &state, ServiceManager &manager, SettingsStore &store) : BaseService(state, manager), store(store) {}

    Result ISettingsServer::GetLanguageCode(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        const auto index{static_cast<size_t>(*state.settings->systemLanguage)};
        if (index >= language::LanguageCodeList.size())
            return result::InvalidLanguage;
        response.Push(language::LanguageCodeList[index]);
        return {};
    }

    Result ISettingsServer::GetAvailableLanguageCodes(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        if (request.outputBuf.empty())
            return kernel::result::InvalidArgument;
        const auto count{std::min(constant::OldLanguageCodeListSize, request.outputBuf[0].size_bytes() / sizeof(LanguageCode))};
        if (count)
            std::memcpy(request.outputBuf[0].data(), language::LanguageCodeList.data(), count * sizeof(LanguageCode));
        response.Push(static_cast<i32>(count));
        return {};
    }

    Result ISettingsServer::MakeLanguageCode(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto index{ReadArgument<i32>(request)};
        if (!index)
            return index.result;
        if (*index < 0 || static_cast<size_t>(*index) >= language::LanguageCodeList.size())
            return result::InvalidLanguage;
        response.Push(language::LanguageCodeList[*index]);
        return {};
    }

    Result ISettingsServer::GetAvailableLanguageCodeCount(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        response.Push<i32>(constant::OldLanguageCodeListSize);
        return {};
    }

    Result ISettingsServer::GetAvailableLanguageCodes2(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        if (request.outputBuf.empty())
            return kernel::result::InvalidArgument;
        const auto count{std::min(constant::NewLanguageCodeListSize, request.outputBuf[0].size_bytes() / sizeof(LanguageCode))};
        if (count)
            std::memcpy(request.outputBuf[0].data(), language::LanguageCodeList.data(), count * sizeof(LanguageCode));
        response.Push(static_cast<i32>(count));
        return {};
    }

    Result ISettingsServer::GetAvailableLanguageCodeCount2(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        response.Push<i32>(constant::NewLanguageCodeListSize);
        return {};
    }

    Result ISettingsServer::GetRegionCode(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        region::RegionCode regionCode{*state.settings->systemRegion};

        if (regionCode == region::RegionCode::Auto)
            regionCode = region::GetRegionCodeForSystemLanguage(*state.settings->systemLanguage);

        response.Push(regionCode);
        return {};
    }
   
    Result ISettingsServer::GetKeyCodeMapByPort(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        // Replaced by the verified key-map implementation in the input block.
        return result::UnknownCommand;
    }

    Result ISettingsServer::Unsupported(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &) {
        LOGW("Unsupported settings command: {} (TIPC={})", request.isTipc ? static_cast<u32>(request.header->type) : request.payload->value, request.isTipc);
        return result::UnknownCommand;
    }

    Result ISettingsServer::GetQuestFlag(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        return manager.CreateOrGetService<ISystemSettingsServer>("set:sys")->GetQuestFlag(session, request, response);
    }

    Result ISettingsServer::GetDeviceNickName(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        return manager.CreateOrGetService<ISystemSettingsServer>("set:sys")->GetDeviceNickName(session, request, response);
    }

}
