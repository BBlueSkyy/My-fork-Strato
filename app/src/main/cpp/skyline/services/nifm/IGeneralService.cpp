// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include "IScanRequest.h"
#include "IRequest.h"
#include "IGeneralService.h"
#include <common/settings.h>
#include <jvm.h>

namespace skyline::service::nifm {
    namespace {
        constexpr u32 DefaultClientId{1};
        constexpr std::string_view DefaultNetworkName{"Skyline Network"};
    }

    static std::array<u8, 4> ConvertIntToByteArray(i32 value) {
        std::array<u8, 4> result{};
        result[0] = value & 0xFF;
        result[1] = (value >> 8) & 0xFF;
        result[2] = (value >> 16) & 0xFF;
        result[3] = (value >> 24) & 0xFF;
        return result;
    }

    IGeneralService::IGeneralService(const DeviceState &state, ServiceManager &manager) : BaseService(state, manager) {}

    Result IGeneralService::GetClientId(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        request.outputBuf.at(0).as<ClientId>() = ClientId{DefaultClientId};
        return {};
    }

    Result IGeneralService::CreateScanRequest(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        manager.RegisterService(SRVREG(IScanRequest), session, response);
        return {};
    }

    Result IGeneralService::CreateRequest(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        [[maybe_unused]] const auto requirementPreset{request.Pop<i32>()};
        manager.RegisterService(SRVREG(IRequest), session, response);
        return {};
    }

    Result IGeneralService::GetCurrentNetworkProfile(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        if (!(*state.settings->isInternetEnabled))
            return result::NoInternetConnection;

        const UUID uuid{static_cast<u128>(0xdeadbeef) << 64};
        const auto dhcpInfo{state.jvm->GetDhcpInfo()};

        SfNetworkProfileData networkProfileData{
            .ipSettingData{
                .ipAddressSetting{
                    .isAutomatic{true},
                    .currentAddress{ConvertIntToByteArray(dhcpInfo.ipAddress)},
                    .subnetMask{ConvertIntToByteArray(dhcpInfo.subnet)},
                    .gateway{ConvertIntToByteArray(dhcpInfo.gateway)},
                },
                .dnsSetting{
                    .isAutomatic{true},
                    .primaryDns{ConvertIntToByteArray(dhcpInfo.dns1)},
                    .secondaryDns{ConvertIntToByteArray(dhcpInfo.dns2)},
                },
                .proxySetting{
                    .enabled{false},
                },
                .mtu{1500},
            },
            .uuid{uuid},
            .networkName{"Skyline Network"},
            .profileType{static_cast<u8>(NetworkProfileType::User)},
            .interfaceType{static_cast<u8>(NetworkInterfaceType::Wifi)},
            .isAutoConnect{1},
            .isLargeCapacity{0},
            .wirelessSettingData{
                .ssidLength{static_cast<u8>(DefaultNetworkName.size())},
                .ssid{"Skyline Network"},
            },
        };

        request.outputBuf.at(0).as<SfNetworkProfileData>() = networkProfileData;
        return {};
    }

    Result IGeneralService::GetCurrentIpAddress(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        if (!(*state.settings->isInternetEnabled))
            return result::NoInternetConnection;

        const auto dhcpInfo{state.jvm->GetDhcpInfo()};
        response.Push(ConvertIntToByteArray(dhcpInfo.ipAddress));
        return {};
    }

    Result IGeneralService::GetCurrentIpConfigInfo(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        if (!(*state.settings->isInternetEnabled))
            return result::NoInternetConnection;

        const auto dhcpInfo{state.jvm->GetDhcpInfo()};

        struct IpConfigInfo {
            IpAddressSetting ipAddressSetting;
            DnsSetting dnsSetting;
        };
        static_assert(sizeof(IpConfigInfo) == 0x16);

        const IpConfigInfo ipConfigInfo{
            .ipAddressSetting{
                .isAutomatic{true},
                .currentAddress{ConvertIntToByteArray(dhcpInfo.ipAddress)},
                .subnetMask{ConvertIntToByteArray(dhcpInfo.subnet)},
                .gateway{ConvertIntToByteArray(dhcpInfo.gateway)},
            },
            .dnsSetting{
                .isAutomatic{true},
                .primaryDns{ConvertIntToByteArray(dhcpInfo.dns1)},
                .secondaryDns{ConvertIntToByteArray(dhcpInfo.dns2)},
            },
        };

        response.Push(ipConfigInfo);
        return {};
    }

    Result IGeneralService::IsWirelessCommunicationEnabled(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        response.Push<u8>(*state.settings->isInternetEnabled);
        return {};
    }

    Result IGeneralService::GetInternetConnectionStatus(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        if (!(*state.settings->isInternetEnabled))
            return result::NoInternetConnection;

        struct Status {
            u8 type;
            u8 wifiStrength;
            u8 state;
        };
        static_assert(sizeof(Status) == 0x3);

        const Status status{
            .type{static_cast<u8>(NetworkInterfaceType::Wifi)},
            .wifiStrength{3},
            .state{4},
        };
        response.Push(status);
        return {};
    }

    Result IGeneralService::IsAnyInternetRequestAccepted(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        const auto clientId{request.inputBuf.at(0).as<ClientId>().id};
        response.Push<u8>(clientId == DefaultClientId && *state.settings->isInternetEnabled);
        return {};
    }
}
