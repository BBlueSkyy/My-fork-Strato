// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <common/language.h>
#include "ISettingsServer.h"
#include "ipc_helpers.h"
#include <os.h>
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
        auto port{ReadArgument<u32>(request)};
        if (!port)
            return port.result;
        // Eden exposes one configured layout for every virtual keyboard port.
        return GetKeyCodeMap2(session, request, response);
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

    Result ISettingsServer::GetKeyCodeMap2(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto layout{manager.CreateOrGetService<ISystemSettingsServer>("set:sys")->GetKeyboardLayoutValue()};
        if (!layout)
            return layout.result;
        constexpr std::array<const char *, 15> maps{
            "Default", "EnglishUsInternational", "EnglishUsInternational", "EnglishUk",
            "French", "FrenchCa", "Spanish", "SpanishLatin", "German", "Italian",
            "Portuguese", "Russian", "Korean", "ChineseSimplified", "ChineseTraditional"
        };
        u32 index{*layout};
        if (index == 1) {
            auto language{*state.settings->systemLanguage};
            if (language == language::SystemLanguage::Korean)
                index = 12;
            else if (language == language::SystemLanguage::SimplifiedChinese)
                index = 13;
            else if (language == language::SystemLanguage::TraditionalChinese)
                index = 14;
        }
        auto file{state.os->assetFileSystem->OpenFileUnchecked(fmt::format("keymaps/{}.bin", maps[index]))};
        if (!file || file->size != 0x1000)
            return kernel::result::NotImplemented;
        std::array<u8, 0x1000> map{};
        if (file->ReadUnchecked(map) != map.size())
            return kernel::result::NotImplemented;
        return WriteBuffer(request, map, Result{105, 1261});
    }

    Result ISettingsServer::GetKeyCodeMap(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto layout{manager.CreateOrGetService<ISystemSettingsServer>("set:sys")->GetKeyboardLayoutValue()};
        if (!layout)
            return layout.result;
        auto result{GetKeyCodeMap2(session, request, response)};
        if (!result && *layout == 0)
            request.outputBuf[0][0] = 1;
        return result;
    }

}
