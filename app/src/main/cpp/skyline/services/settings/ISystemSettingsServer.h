// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include "settings_store.h"
#include <services/timesrv/core.h>

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
        timesrv::core::TimeServiceObject &timeCore;

      public:
        ISystemSettingsServer(const DeviceState &state, ServiceManager &manager, SettingsStore &store, timesrv::core::TimeServiceObject &timeCore);

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

        ResultValue<u32> GetKeyboardLayoutValue();

        Result GetKeyboardLayout(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result SetKeyboardLayout(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result GetExternalSteadyClockSourceId(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result GetUserSystemClockContext(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result GetNetworkSystemClockContext(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result SetUserSystemClockContext(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result SetNetworkSystemClockContext(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result IsUserSystemClockAutomaticCorrectionEnabled(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result SetUserSystemClockAutomaticCorrectionEnabled(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result GetExternalSteadyClockInternalOffset(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result GetExternalRtcResetFlag(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result GetDeviceTimeZoneLocationName(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result GetDeviceTimeZoneLocationUpdatedTime(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result GetUserSystemClockAutomaticCorrectionUpdatedTime(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result SetExternalSteadyClockSourceId(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result SetExternalSteadyClockInternalOffset(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result SetUserSystemClockAutomaticCorrectionUpdatedTime(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result SetExternalRtcResetFlag(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result SetDeviceTimeZoneLocationName(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result SetDeviceTimeZoneLocationUpdatedTime(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result GetWirelessLanEnableFlag(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result SetWirelessLanEnableFlag(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result GetUsb30EnableFlag(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result SetUsb30EnableFlag(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result GetNfcEnableFlag(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result SetNfcEnableFlag(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result GetBluetoothEnableFlag(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result SetBluetoothEnableFlag(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result GetUsbFullKeyEnableFlag(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result SetUsbFullKeyEnableFlag(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result GetBluetoothAfhEnableFlag(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result SetBluetoothAfhEnableFlag(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result GetBluetoothBoostEnableFlag(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result SetBluetoothBoostEnableFlag(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result GetUsb30HostEnableFlag(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result SetUsb30HostEnableFlag(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result GetUsb30DeviceEnableFlag(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result SetUsb30DeviceEnableFlag(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result GetWebInspectorFlag(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result GetMemoryUsageRateFlag(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result GetFieldTestingFlag(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result GetAccountSettings(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result SetAccountSettings(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result GetNotificationSettings(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result SetNotificationSettings(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result GetSleepSettings(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result SetSleepSettings(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result GetEulaVersions(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result SetEulaVersions(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result GetAccountNotificationSettings(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result SetAccountNotificationSettings(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result Unsupported(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

      protected:
        ServiceFunctionDescriptor GetServiceFunction(u32 id, bool isTipc) override {
            static const auto functions = frozen::make_unordered_map({
                SFUNC(0, ISystemSettingsServer, SetLanguageCode),
                SFUNC(3, ISystemSettingsServer, GetFirmwareVersion),
                SFUNC(4, ISystemSettingsServer, GetFirmwareVersion2),
                SFUNC(7, ISystemSettingsServer, GetLockScreenFlag),
                SFUNC(8, ISystemSettingsServer, SetLockScreenFlag),
                SFUNC(13, ISystemSettingsServer, GetExternalSteadyClockSourceId),
                SFUNC(14, ISystemSettingsServer, SetExternalSteadyClockSourceId),
                SFUNC(15, ISystemSettingsServer, GetUserSystemClockContext),
                SFUNC(16, ISystemSettingsServer, SetUserSystemClockContext),
                SFUNC(17, ISystemSettingsServer, GetAccountSettings),
                SFUNC(18, ISystemSettingsServer, SetAccountSettings),
                SFUNC(21, ISystemSettingsServer, GetEulaVersions),
                SFUNC(22, ISystemSettingsServer, SetEulaVersions),
                SFUNC(23, ISystemSettingsServer, GetColorSetId),
                SFUNC(24, ISystemSettingsServer, SetColorSetId),
                SFUNC(25, ISystemSettingsServer, GetConsoleInformationUploadFlag),
                SFUNC(26, ISystemSettingsServer, SetConsoleInformationUploadFlag),
                SFUNC(27, ISystemSettingsServer, GetAutomaticApplicationDownloadFlag),
                SFUNC(28, ISystemSettingsServer, SetAutomaticApplicationDownloadFlag),
                SFUNC(29, ISystemSettingsServer, GetNotificationSettings),
                SFUNC(30, ISystemSettingsServer, SetNotificationSettings),
                SFUNC(31, ISystemSettingsServer, GetAccountNotificationSettings),
                SFUNC(32, ISystemSettingsServer, SetAccountNotificationSettings),
                SFUNC(37, ISystemSettingsServer, GetSettingsItemValueSize),
                SFUNC(38, ISystemSettingsServer, GetSettingsItemValue),
                SFUNC(47, ISystemSettingsServer, GetQuestFlag),
                SFUNC(48, ISystemSettingsServer, SetQuestFlag),
                SFUNC(53, ISystemSettingsServer, GetDeviceTimeZoneLocationName),
                SFUNC(54, ISystemSettingsServer, SetDeviceTimeZoneLocationName),
                SFUNC(57, ISystemSettingsServer, SetRegionCode),
                SFUNC(58, ISystemSettingsServer, GetNetworkSystemClockContext),
                SFUNC(59, ISystemSettingsServer, SetNetworkSystemClockContext),
                SFUNC(60, ISystemSettingsServer, IsUserSystemClockAutomaticCorrectionEnabled),
                SFUNC(61, ISystemSettingsServer, SetUserSystemClockAutomaticCorrectionEnabled),
                SFUNC(62, ISystemSettingsServer, GetDebugModeFlag),
                SFUNC(65, ISystemSettingsServer, GetUsb30EnableFlag),
                SFUNC(66, ISystemSettingsServer, SetUsb30EnableFlag),
                SFUNC(69, ISystemSettingsServer, GetNfcEnableFlag),
                SFUNC(70, ISystemSettingsServer, SetNfcEnableFlag),
                SFUNC(71, ISystemSettingsServer, GetSleepSettings),
                SFUNC(72, ISystemSettingsServer, SetSleepSettings),
                SFUNC(73, ISystemSettingsServer, GetWirelessLanEnableFlag),
                SFUNC(74, ISystemSettingsServer, SetWirelessLanEnableFlag),
                SFUNC(77, ISystemSettingsServer, GetDeviceNickName),
                SFUNC(78, ISystemSettingsServer, SetDeviceNickName),
                SFUNC(79, ISystemSettingsServer, GetProductModel),
                SFUNC(88, ISystemSettingsServer, GetBluetoothEnableFlag),
                SFUNC(89, ISystemSettingsServer, SetBluetoothEnableFlag),
                SFUNC(95, ISystemSettingsServer, GetAutoUpdateEnableFlag),
                SFUNC(96, ISystemSettingsServer, SetAutoUpdateEnableFlag),
                SFUNC(99, ISystemSettingsServer, GetBatteryPercentageFlag),
                SFUNC(100, ISystemSettingsServer, SetBatteryPercentageFlag),
                SFUNC(101, ISystemSettingsServer, GetExternalRtcResetFlag),
                SFUNC(102, ISystemSettingsServer, SetExternalRtcResetFlag),
                SFUNC(103, ISystemSettingsServer, GetUsbFullKeyEnableFlag),
                SFUNC(104, ISystemSettingsServer, SetUsbFullKeyEnableFlag),
                SFUNC(105, ISystemSettingsServer, SetExternalSteadyClockInternalOffset),
                SFUNC(106, ISystemSettingsServer, GetExternalSteadyClockInternalOffset),
                SFUNC(111, ISystemSettingsServer, GetBluetoothAfhEnableFlag),
                SFUNC(112, ISystemSettingsServer, SetBluetoothAfhEnableFlag),
                SFUNC(113, ISystemSettingsServer, GetBluetoothBoostEnableFlag),
                SFUNC(114, ISystemSettingsServer, SetBluetoothBoostEnableFlag),
                SFUNC(120, ISystemSettingsServer, GetPushNotificationActivityModeOnSleep),
                SFUNC(121, ISystemSettingsServer, SetPushNotificationActivityModeOnSleep),
                SFUNC(124, ISystemSettingsServer, GetErrorReportSharePermission),
                SFUNC(125, ISystemSettingsServer, SetErrorReportSharePermission),
                SFUNC(136, ISystemSettingsServer, GetKeyboardLayout),
                SFUNC(137, ISystemSettingsServer, SetKeyboardLayout),
                SFUNC(138, ISystemSettingsServer, GetWebInspectorFlag),
                SFUNC(150, ISystemSettingsServer, GetDeviceTimeZoneLocationUpdatedTime),
                SFUNC(151, ISystemSettingsServer, SetDeviceTimeZoneLocationUpdatedTime),
                SFUNC(152, ISystemSettingsServer, GetUserSystemClockAutomaticCorrectionUpdatedTime),
                SFUNC(153, ISystemSettingsServer, SetUserSystemClockAutomaticCorrectionUpdatedTime),
                SFUNC(164, ISystemSettingsServer, GetUsb30HostEnableFlag),
                SFUNC(165, ISystemSettingsServer, SetUsb30HostEnableFlag),
                SFUNC(166, ISystemSettingsServer, GetUsb30DeviceEnableFlag),
                SFUNC(167, ISystemSettingsServer, SetUsb30DeviceEnableFlag),
                SFUNC(170, ISystemSettingsServer, GetChineseTraditionalInputMethod),
                SFUNC(171, ISystemSettingsServer, SetChineseTraditionalInputMethod),
                SFUNC(181, ISystemSettingsServer, GetT),
                SFUNC(182, ISystemSettingsServer, SetT),
                SFUNC(183, ISystemSettingsServer, GetPlatformRegion),
                SFUNC(184, ISystemSettingsServer, SetPlatformRegion),
                SFUNC(186, ISystemSettingsServer, GetMemoryUsageRateFlag),
                SFUNC(187, ISystemSettingsServer, GetTouchScreenMode),
                SFUNC(188, ISystemSettingsServer, SetTouchScreenMode),
                SFUNC(201, ISystemSettingsServer, GetFieldTestingFlag)
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
