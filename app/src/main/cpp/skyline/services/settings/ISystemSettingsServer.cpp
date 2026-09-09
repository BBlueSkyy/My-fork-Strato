// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include "ISystemSettingsServer.h"
#include "ipc_helpers.h"
#include "settings_items.h"
#include <common/settings.h>

namespace skyline::service::settings {
    namespace {
        ResultValue<std::string_view> ReadSettingName(ipc::IpcRequest &request, size_t index) {
            if (request.inputBuf.size() <= index || !request.inputBuf[index].data() || request.inputBuf[index].empty())
                return Result{105, static_cast<u16>(201 + index)};
            const auto buffer{request.inputBuf[index]};
            const auto size{std::min<size_t>(buffer.size_bytes(), 0x48)};
            const auto *start{reinterpret_cast<const char *>(buffer.data())};
            const auto *end{static_cast<const char *>(std::memchr(start, 0, size))};
            if (!end)
                return Result{105, static_cast<u16>(241 + index)};
            if (start == end)
                return Result{105, static_cast<u16>(221 + index)};
            return std::string_view(start, end - start);
        }

        ResultValue<const SettingsItem *> ReadSettingsItem(ipc::IpcRequest &request) {
            auto category{ReadSettingName(request, 0)};
            if (!category)
                return category.result;
            auto name{ReadSettingName(request, 1)};
            if (!name)
                return name.result;
            const auto *item{FindSettingsItem(*category, *name)};
            if (!item) {
                LOGD("Unknown settings item: {}/{}", *category, *name);
                return Result{105, 11};
            }
            return item;
        }
    }

    ISystemSettingsServer::ISystemSettingsServer(const DeviceState &state, ServiceManager &manager, SettingsStore &store) : BaseService(state, manager), store(store) {}

    Result ISystemSettingsServer::GetFirmwareVersion(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        // Version 1 clears revision_minor; all other bytes match Version 2.
        const SysVerTitle version{.major=9, .minor=0, .micro=0, .revMajor=4, .revMinor=0, .platform="NX", .verHash="4de65c071fd0869695b7629f75eb97b2551dbf2f", .dispVer="9.0.0", .dispTitle="NintendoSDK Firmware for NX 9.0.0-4.0"};
        return WriteBuffer(request, version, result::NullFirmwareBuffer);
    }

    Result ISystemSettingsServer::GetFirmwareVersion2(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        // Strato has no installed system-version archive. Preserve the existing HLE
        // profile instead of advertising an unsupported firmware revision.
        return GetFirmwareVersion(session, request, response);
    }

    Result ISystemSettingsServer::GetColorSetId(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        return PushValue(response, store.Get<u32>(23, 0));
    }
    Result ISystemSettingsServer::Unsupported(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &) {
        LOGW("Unsupported settings command: {} (TIPC={})", request.isTipc ? static_cast<u32>(request.header->type) : request.payload->value, request.isTipc);
        return result::UnknownCommand;
    }

    Result ISystemSettingsServer::GetLockScreenFlag(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        return PushValue(response, store.Get<u8>(7, 1));
    }

    Result ISystemSettingsServer::SetLockScreenFlag(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto value{ReadArgument<u8>(request)};
        if (!value)
            return value.result;
        if (*value > 1)
            return kernel::result::InvalidArgument;
        return store.Set(7, *value);
    }

    Result ISystemSettingsServer::SetColorSetId(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto value{ReadArgument<u32>(request)};
        if (!value)
            return value.result;
        if (*value > 1)
            return kernel::result::InvalidArgument;
        return store.Set(23, *value);
    }

    Result ISystemSettingsServer::GetConsoleInformationUploadFlag(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        return PushValue(response, store.Get<u8>(25, 0));
    }

    Result ISystemSettingsServer::SetConsoleInformationUploadFlag(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto value{ReadArgument<u8>(request)};
        if (!value)
            return value.result;
        if (*value > 1)
            return kernel::result::InvalidArgument;
        return store.Set(25, *value);
    }

    Result ISystemSettingsServer::GetAutomaticApplicationDownloadFlag(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        return PushValue(response, store.Get<u8>(27, 0));
    }

    Result ISystemSettingsServer::SetAutomaticApplicationDownloadFlag(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto value{ReadArgument<u8>(request)};
        if (!value)
            return value.result;
        if (*value > 1)
            return kernel::result::InvalidArgument;
        return store.Set(27, *value);
    }

    Result ISystemSettingsServer::GetQuestFlag(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        return PushValue(response, store.Get<u8>(47, 0));
    }

    Result ISystemSettingsServer::SetQuestFlag(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto value{ReadArgument<u8>(request)};
        if (!value)
            return value.result;
        if (*value > 1)
            return kernel::result::InvalidArgument;
        return store.Set(47, *value);
    }

    Result ISystemSettingsServer::GetAutoUpdateEnableFlag(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        return PushValue(response, store.Get<u8>(95, 0));
    }

    Result ISystemSettingsServer::SetAutoUpdateEnableFlag(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto value{ReadArgument<u8>(request)};
        if (!value)
            return value.result;
        if (*value > 1)
            return kernel::result::InvalidArgument;
        return store.Set(95, *value);
    }

    Result ISystemSettingsServer::GetBatteryPercentageFlag(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        return PushValue(response, store.Get<u8>(99, 0));
    }

    Result ISystemSettingsServer::SetBatteryPercentageFlag(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto value{ReadArgument<u8>(request)};
        if (!value)
            return value.result;
        if (*value > 1)
            return kernel::result::InvalidArgument;
        return store.Set(99, *value);
    }

    Result ISystemSettingsServer::GetPushNotificationActivityModeOnSleep(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        return PushValue(response, store.Get<u32>(120, 0));
    }

    Result ISystemSettingsServer::SetPushNotificationActivityModeOnSleep(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto value{ReadArgument<u32>(request)};
        if (!value)
            return value.result;
        if (*value > 1)
            return kernel::result::InvalidArgument;
        return store.Set(120, *value);
    }

    Result ISystemSettingsServer::GetErrorReportSharePermission(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        return PushValue(response, store.Get<u32>(124, 2));
    }

    Result ISystemSettingsServer::SetErrorReportSharePermission(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto value{ReadArgument<u32>(request)};
        if (!value)
            return value.result;
        if (*value > 2)
            return kernel::result::InvalidArgument;
        return store.Set(124, *value);
    }

    Result ISystemSettingsServer::GetChineseTraditionalInputMethod(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        return PushValue(response, store.Get<u32>(170, 0));
    }

    Result ISystemSettingsServer::SetChineseTraditionalInputMethod(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto value{ReadArgument<u32>(request)};
        if (!value)
            return value.result;
        if (*value > 2)
            return kernel::result::InvalidArgument;
        return store.Set(170, *value);
    }

    Result ISystemSettingsServer::GetPlatformRegion(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        return PushValue(response, store.Get<u32>(183, 1));
    }

    Result ISystemSettingsServer::SetPlatformRegion(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto value{ReadArgument<u32>(request)};
        if (!value)
            return value.result;
        if (*value > 2 || *value < 1)
            return kernel::result::InvalidArgument;
        return store.Set(183, *value);
    }

    Result ISystemSettingsServer::GetTouchScreenMode(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        return PushValue(response, store.Get<u32>(187, 1));
    }

    Result ISystemSettingsServer::SetTouchScreenMode(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto value{ReadArgument<u32>(request)};
        if (!value)
            return value.result;
        if (*value > 1)
            return kernel::result::InvalidArgument;
        return store.Set(187, *value);
    }

    Result ISystemSettingsServer::GetT(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto region{store.Get<u32>(183, 1)};
        if (!region)
            return region.result;
        response.Push<u8>(*region == 2);
        return {};
    }

    Result ISystemSettingsServer::SetT(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto flag{ReadArgument<u8>(request)};
        if (!flag)
            return flag.result;
        return store.Set<u32>(183, 1 + (*flag & 1));
    }

    Result ISystemSettingsServer::GetDeviceNickName(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        std::array<char, 0x80> fallback{};
        std::memcpy(fallback.data(), "Strato", 6);
        auto name{store.Get(77, fallback)};
        if (!name)
            return name.result;
        return WriteBuffer(request, *name, Result{105, 808});
    }

    Result ISystemSettingsServer::SetDeviceNickName(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto name{ReadBuffer<std::array<char, 0x80>>(request)};
        if (!name)
            return name.result;
        auto end{std::find(name->begin(), name->end(), '\0')};
        if (end == name->end())
            return kernel::result::InvalidArgument;
        std::fill(end, name->end(), '\0');
        return store.Set(77, *name);
    }

    Result ISystemSettingsServer::GetProductModel(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        response.Push<u32>(1); // Nx: the emulated retail Switch model, independent of dock mode.
        return {};
    }

    Result ISystemSettingsServer::GetDebugModeFlag(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        response.Push<u8>(0); // Retail HLE profile; matches settings_debug item.
        return {};
    }

    Result ISystemSettingsServer::SetLanguageCode(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto code{ReadArgument<LanguageCode>(request)};
        if (!code)
            return code.result;
        auto it{std::find(language::LanguageCodeList.begin(), language::LanguageCodeList.end(), *code)};
        if (it == language::LanguageCodeList.end())
            return result::InvalidLanguage;
        auto result{store.Set(0, *code)};
        if (!result)
            state.settings->systemLanguage = static_cast<language::SystemLanguage>(it - language::LanguageCodeList.begin());
        return result;
    }

    Result ISystemSettingsServer::SetRegionCode(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto region{ReadArgument<i32>(request)};
        if (!region)
            return region.result;
        if (*region < 0 || *region > 5)
            return kernel::result::InvalidArgument;
        auto result{store.Set(57, *region)};
        if (!result)
            state.settings->systemRegion = static_cast<region::RegionCode>(*region);
        return result;
    }

    Result ISystemSettingsServer::GetSettingsItemValueSize(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto item{ReadSettingsItem(request)};
        if (!item)
            return item.result;
        response.Push<u64>((*item)->size);
        return {};
    }

    Result ISystemSettingsServer::GetSettingsItemValue(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto item{ReadSettingsItem(request)};
        if (!item)
            return item.result;
        if (request.outputBuf.empty())
            return Result{105, 205};
        auto output{request.outputBuf[0]};
        const auto count{std::min(output.size_bytes(), (*item)->size)};
        if (count && !output.data())
            return Result{105, 205};
        // The ABI is little endian and permits a short output buffer.
        for (size_t i{}; i < count; ++i)
            output[i] = static_cast<u8>((*item)->value >> (i * 8));
        response.Push<u64>(count);
        return {};
    }

}
