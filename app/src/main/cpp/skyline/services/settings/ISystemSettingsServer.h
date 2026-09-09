// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include "settings_store.h"

#include <services/serviceman.h>

namespace skyline::service::settings {
    /**
     * @brief ISystemSettingsServer or set:sys service provides access to system settings
     */
    class ISystemSettingsServer : public BaseService {
      private:
        /**
         * @brief Encapsulates the system version, this is sent to the application in GetFirmwareVersion
         * @url https://switchbrew.org/wiki/System_Version_Title
         */
        struct SysVerTitle {
            u8 major; //!< The major version
            u8 minor; //!< The minor vision
            u8 micro; //!< The micro vision
            u8 _pad0_;
            u8 revMajor; //!< The major revision
            u8 revMinor; //!< The major revision
            u16 _pad1_;
            u8 platform[0x20]; //!< "NX"
            u8 verHash[0x40]; //!< The hash of the version string
            u8 dispVer[0x18]; //!< The version number string
            u8 dispTitle[0x80]; //!< The version title string
        };
        static_assert(sizeof(SysVerTitle) == 0x100);

        SettingsStore &store;

      public:
        ISystemSettingsServer(const DeviceState &state, ServiceManager &manager, SettingsStore &store);

        /**
         * @brief Writes the Firmware version to a 0xA buffer
         * @url https://switchbrew.org/wiki/Settings_services#GetFirmwareVersion
         */
        Result GetFirmwareVersion(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result GetFirmwareVersion2(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        /**
         * @url https://switchbrew.org/wiki/Settings_services#GetColorSetId
         */
        Result GetColorSetId(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result GetLockScreenFlag(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result SetLockScreenFlag(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result SetColorSetId(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result GetConsoleInformationUploadFlag(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result SetConsoleInformationUploadFlag(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result GetAutomaticApplicationDownloadFlag(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result SetAutomaticApplicationDownloadFlag(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result GetQuestFlag(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result SetQuestFlag(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result GetAutoUpdateEnableFlag(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result SetAutoUpdateEnableFlag(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result GetBatteryPercentageFlag(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result SetBatteryPercentageFlag(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result GetPushNotificationActivityModeOnSleep(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result SetPushNotificationActivityModeOnSleep(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result GetErrorReportSharePermission(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result SetErrorReportSharePermission(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result GetChineseTraditionalInputMethod(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result SetChineseTraditionalInputMethod(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result GetPlatformRegion(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result SetPlatformRegion(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result GetTouchScreenMode(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result SetTouchScreenMode(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result GetT(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result SetT(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result GetDeviceNickName(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result SetDeviceNickName(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result GetProductModel(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result GetDebugModeFlag(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result SetLanguageCode(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result SetRegionCode(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result GetSettingsItemValueSize(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result GetSettingsItemValue(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result Unsupported(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

      protected:
        ServiceFunctionDescriptor GetServiceFunction(u32 id, bool isTipc) override {
            static const auto functions = frozen::make_unordered_map({
                SFUNC(38, ISystemSettingsServer, GetSettingsItemValue),
                SFUNC(37, ISystemSettingsServer, GetSettingsItemValueSize),
                SFUNC(57, ISystemSettingsServer, SetRegionCode),
                SFUNC(0, ISystemSettingsServer, SetLanguageCode),
                SFUNC(62, ISystemSettingsServer, GetDebugModeFlag),
                SFUNC(79, ISystemSettingsServer, GetProductModel),
                SFUNC(78, ISystemSettingsServer, SetDeviceNickName),
                SFUNC(77, ISystemSettingsServer, GetDeviceNickName),
                SFUNC(182, ISystemSettingsServer, SetT),
                SFUNC(181, ISystemSettingsServer, GetT),
                SFUNC(188, ISystemSettingsServer, SetTouchScreenMode),
                SFUNC(187, ISystemSettingsServer, GetTouchScreenMode),
                SFUNC(184, ISystemSettingsServer, SetPlatformRegion),
                SFUNC(183, ISystemSettingsServer, GetPlatformRegion),
                SFUNC(171, ISystemSettingsServer, SetChineseTraditionalInputMethod),
                SFUNC(170, ISystemSettingsServer, GetChineseTraditionalInputMethod),
                SFUNC(125, ISystemSettingsServer, SetErrorReportSharePermission),
                SFUNC(124, ISystemSettingsServer, GetErrorReportSharePermission),
                SFUNC(121, ISystemSettingsServer, SetPushNotificationActivityModeOnSleep),
                SFUNC(120, ISystemSettingsServer, GetPushNotificationActivityModeOnSleep),
                SFUNC(100, ISystemSettingsServer, SetBatteryPercentageFlag),
                SFUNC(99, ISystemSettingsServer, GetBatteryPercentageFlag),
                SFUNC(96, ISystemSettingsServer, SetAutoUpdateEnableFlag),
                SFUNC(95, ISystemSettingsServer, GetAutoUpdateEnableFlag),
                SFUNC(48, ISystemSettingsServer, SetQuestFlag),
                SFUNC(47, ISystemSettingsServer, GetQuestFlag),
                SFUNC(28, ISystemSettingsServer, SetAutomaticApplicationDownloadFlag),
                SFUNC(27, ISystemSettingsServer, GetAutomaticApplicationDownloadFlag),
                SFUNC(26, ISystemSettingsServer, SetConsoleInformationUploadFlag),
                SFUNC(25, ISystemSettingsServer, GetConsoleInformationUploadFlag),
                SFUNC(24, ISystemSettingsServer, SetColorSetId),
                SFUNC(8, ISystemSettingsServer, SetLockScreenFlag),
                SFUNC(7, ISystemSettingsServer, GetLockScreenFlag),
            SFUNC(0x3, ISystemSettingsServer, GetFirmwareVersion),
            SFUNC(0x4, ISystemSettingsServer, GetFirmwareVersion2),
            SFUNC(0x17, ISystemSettingsServer, GetColorSetId)
            });
            if (!isTipc) {
                auto it{functions.find(id)};
                if (it != functions.end())
                    return {reinterpret_cast<DerivedService *>(this),
                            reinterpret_cast<decltype(ServiceFunctionDescriptor::function)>(it->second.first), it->second.second};
            }
            return {reinterpret_cast<DerivedService *>(this),
                    reinterpret_cast<decltype(ServiceFunctionDescriptor::function)>(&ISystemSettingsServer::Unsupported), "ISystemSettingsServer::Unsupported"};
        }
    };
}
