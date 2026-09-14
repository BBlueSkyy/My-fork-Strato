// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <services/serviceman.h>
#include "common/uuid.h"

namespace skyline::service::nifm {
    namespace result {
        constexpr Result NoInternetConnection{110, 300};
    }

    struct ClientId {
        u32 id{};
    };
    static_assert(sizeof(ClientId) == 0x4);

    enum class NetworkProfileType : u32 {
        User = 1 << 0,
        SsidList = 1 << 1,
        Temporary = 1 << 2,
    };

    enum class NetworkInterfaceType : u32 {
        Invalid = 0,
        Wifi = 1,
        Ethernet = 2,
    };

    enum class Authentication : u32 {
        Invalid = 0,
        Open = 1,
        Shared = 2,
        Wpa = 3,
        WpaPsk = 4,
        Wpa2 = 5,
        Wpa2Psk = 6,
        Unknown = 7,
    };

    enum class Encryption : u32 {
        Invalid = 0,
        None = 1,
        Wep = 2,
        Tkip = 3,
        Aes = 4,
    };

    struct IpAddressSetting {
        bool isAutomatic{};
        std::array<u8, 4> currentAddress{};
        std::array<u8, 4> subnetMask{};
        std::array<u8, 4> gateway{};
    };
    static_assert(sizeof(IpAddressSetting) == 0xD);

    struct DnsSetting {
        bool isAutomatic{};
        std::array<u8, 4> primaryDns{};
        std::array<u8, 4> secondaryDns{};
    };
    static_assert(sizeof(DnsSetting) == 0x9);

    struct ProxySetting {
        bool enabled{};
        u8 _pad0_[0x1];
        u16 port{};
        std::array<char, 0x64> proxyServer{};
        bool automaticAuthEnabled{};
        std::array<char, 0x20> user{};
        std::array<char, 0x20> password{};
        u8 _pad1_[0x1];
    };
    static_assert(sizeof(ProxySetting) == 0xAA);

    struct IpSettingData {
        IpAddressSetting ipAddressSetting{};
        DnsSetting dnsSetting{};
        ProxySetting proxySetting{};
        u16 mtu{};
    };
    static_assert(sizeof(IpSettingData) == 0xC2);

    struct SfWirelessSettingData {
        u8 ssidLength{};
        std::array<char, 0x20> ssid{};
        u8 _unk0_[0x3];
        std::array<char, 0x41> passphrase{};
    };
    static_assert(sizeof(SfWirelessSettingData) == 0x65);

    struct NifmWirelessSettingData {
        u8 ssidLength{};
        std::array<char, 0x21> ssid{};
        u8 _unk0_[0x1];
        u8 _pad0_[0x1];
        u32 _unk1_[0x2];
        std::array<char, 0x41> passphrase{};
        u8 _pad1_[0x3];
    };
    static_assert(sizeof(NifmWirelessSettingData) == 0x70);

    #pragma pack(push, 1)
    struct SfNetworkProfileData {
        IpSettingData ipSettingData{};
        UUID uuid{};
        std::array<char, 0x40> networkName{};
        u8 profileType{};
        u8 interfaceType{};
        u8 isAutoConnect{};
        u8 isLargeCapacity{};
        SfWirelessSettingData wirelessSettingData{};
        u8 _pad0_[0x1];
    };
    static_assert(sizeof(SfNetworkProfileData) == 0x17C);

    struct NifmNetworkProfileData {
        UUID uuid{};
        std::array<char, 0x40> networkName{};
        NetworkProfileType profileType{};
        NetworkInterfaceType interfaceType{};
        u8 isAutoConnect{};
        u8 isLargeCapacity{};
        u8 _pad0_[0x2];
        NifmWirelessSettingData wirelessSettingData{};
        IpSettingData ipSettingData{};
    };
    static_assert(sizeof(NifmNetworkProfileData) == 0x18E);

    struct SfNetworkProfileBasicInfo {
        UUID uuid{};
        std::array<char, 0x40> networkName{};
        u8 profileType{};
        u8 interfaceType{};
        u8 ssidLength{};
        std::array<char, 0x20> ssid{};
        u8 authentication{};
        u8 encryption{};
    };
    static_assert(sizeof(SfNetworkProfileBasicInfo) == 0x75);
    #pragma pack(pop)

    class IGeneralService : public BaseService {
      public:
        IGeneralService(const DeviceState &state, ServiceManager &manager);

        Result GetClientId(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result CreateScanRequest(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result CreateRequest(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result GetCurrentNetworkProfile(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result GetCurrentIpAddress(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result GetCurrentIpConfigInfo(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result IsWirelessCommunicationEnabled(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result GetInternetConnectionStatus(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result IsAnyInternetRequestAccepted(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        SERVICE_DECL(
            SFUNC(0x1, IGeneralService, GetClientId),
            SFUNC(0x2, IGeneralService, CreateScanRequest),
            SFUNC(0x4, IGeneralService, CreateRequest),
            SFUNC(0x5, IGeneralService, GetCurrentNetworkProfile),
            SFUNC(0xC, IGeneralService, GetCurrentIpAddress),
            SFUNC(0xF, IGeneralService, GetCurrentIpConfigInfo),
            SFUNC(0x11, IGeneralService, IsWirelessCommunicationEnabled),
            SFUNC(0x12, IGeneralService, GetInternetConnectionStatus),
            SFUNC(0x15, IGeneralService, IsAnyInternetRequestAccepted)
        )
    };
}
