// SPDX-License-Identifier: MPL-2.0
// Copyright © 2023 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <algorithm>
#include <array>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>
#include <common.h>
#include <kernel/types/KEvent.h>

namespace skyline::service::clkrst {
    struct ModuleState {
        u32 clockFrequency{};
        u8 clockEnabled{};
        u8 powerEnabled{};
        u8 resetAsserted{};
        u8 reserved{};
        u32 minVClockRate{};
    };
    static_assert(sizeof(ModuleState) == 0xC);

    enum class ClockRatesListType : i32 {
        Invalid = 0,
        Discrete = 1,
        Range = 2,
    };

    class ClockResetState {
      private:
        mutable std::mutex mutex;
        std::unordered_map<u32, ModuleState> modules;
        std::unordered_map<u32, u32> parentClocks;
        float notifiedTemperature{};
        std::shared_ptr<kernel::type::KEvent> updateEvent;

        ModuleState &GetOrCreateLocked(u32 deviceCode) {
            return modules.try_emplace(deviceCode, ModuleState{}).first->second;
        }

        void SignalUpdate() {
            updateEvent->Signal();
        }

      public:
        static constexpr std::array<u32, 26> SupportedDeviceCodes{
            0x40000001, // Cpu
            0x40000002, // Gpu
            0x40000011, // Disp1
            0x40000012, // Disp2
            0x4000001B, // Tsec
            0x4000001C, // Mselect
            0x40000020, // Sor1
            0x4000002C, // Host1x
            0x4000002F, // Vic
            0x40000030, // Nvenc
            0x40000031, // Nvjpg
            0x40000032, // Nvdec
            0x40000036, // Ape
            0x40000037, // AudioDsp
            0x40000039, // Emc
            0x4000003C, // Dsi
            0x40000042, // SysBus
            0x40000044, // XusbSs
            0x40000045, // XusbHost
            0x40000046, // XusbDevice
            0x4000004A, // Gpuaux
            0x4000004D, // Pcie
            0x40000052, // Apbdma
            0x40000015, // Sdmmc1
            0x40000016, // Sdmmc2
            0x40000018, // Sdmmc4
        };

        explicit ClockResetState(const DeviceState &state)
            : updateEvent(std::make_shared<kernel::type::KEvent>(state, false)) {}

        static bool IsSupportedDeviceCode(u32 deviceCode) {
            return std::find(SupportedDeviceCodes.begin(), SupportedDeviceCodes.end(), deviceCode) != SupportedDeviceCodes.end();
        }

        std::shared_ptr<kernel::type::KEvent> GetUpdateEvent() const {
            return updateEvent;
        }

        std::optional<ModuleState> GetModuleState(u32 deviceCode) const {
            if (!IsSupportedDeviceCode(deviceCode))
                return std::nullopt;

            std::scoped_lock lock{mutex};
            if (auto it{modules.find(deviceCode)}; it != modules.end())
                return it->second;
            return ModuleState{};
        }

        std::array<ModuleState, SupportedDeviceCodes.size()> GetStateTable() const {
            std::array<ModuleState, SupportedDeviceCodes.size()> result{};
            std::scoped_lock lock{mutex};
            for (size_t index{}; index < SupportedDeviceCodes.size(); index++) {
                if (auto it{modules.find(SupportedDeviceCodes[index])}; it != modules.end())
                    result[index] = it->second;
            }
            return result;
        }

        bool SetClockEnabled(u32 deviceCode, bool enabled) {
            if (!IsSupportedDeviceCode(deviceCode))
                return false;
            {
                std::scoped_lock lock{mutex};
                GetOrCreateLocked(deviceCode).clockEnabled = enabled;
            }
            SignalUpdate();
            return true;
        }

        bool SetPowerEnabled(u32 deviceCode, bool enabled) {
            if (!IsSupportedDeviceCode(deviceCode))
                return false;
            {
                std::scoped_lock lock{mutex};
                GetOrCreateLocked(deviceCode).powerEnabled = enabled;
            }
            SignalUpdate();
            return true;
        }

        bool SetResetAsserted(u32 deviceCode, bool asserted) {
            if (!IsSupportedDeviceCode(deviceCode))
                return false;
            {
                std::scoped_lock lock{mutex};
                GetOrCreateLocked(deviceCode).resetAsserted = asserted;
            }
            SignalUpdate();
            return true;
        }

        bool SetClockRate(u32 deviceCode, u32 hz) {
            if (!IsSupportedDeviceCode(deviceCode))
                return false;
            {
                std::scoped_lock lock{mutex};
                GetOrCreateLocked(deviceCode).clockFrequency = hz;
            }
            SignalUpdate();
            return true;
        }

        std::optional<u32> GetClockRate(u32 deviceCode) const {
            const auto module{GetModuleState(deviceCode)};
            if (!module)
                return std::nullopt;
            return module->clockFrequency;
        }

        bool SetMinimumVoltageClockRate(u32 deviceCode, u32 hz) {
            if (!IsSupportedDeviceCode(deviceCode))
                return false;
            {
                std::scoped_lock lock{mutex};
                GetOrCreateLocked(deviceCode).minVClockRate = hz;
            }
            SignalUpdate();
            return true;
        }

        std::vector<u32> GetPossibleClockRates(u32 deviceCode) const {
            if (!IsSupportedDeviceCode(deviceCode))
                return {};

            switch (deviceCode) {
                case 0x40000001:
                    return {1020000000U, 1224000000U, 1785000000U};
                case 0x40000002:
                    return {76800000U, 230400000U, 307200000U, 384000000U, 460800000U, 691200000U, 768000000U};
                case 0x40000039:
                    return {1065600000U, 1331200000U, 1600000000U};
                default: {
                    const auto rate{GetClockRate(deviceCode)};
                    if (rate && *rate)
                        return {*rate};
                    return {};
                }
            }
        }

        bool SetParentClock(u32 deviceCode, u32 parentDeviceCode) {
            if (!IsSupportedDeviceCode(deviceCode) || !IsSupportedDeviceCode(parentDeviceCode))
                return false;
            {
                std::scoped_lock lock{mutex};
                parentClocks[deviceCode] = parentDeviceCode;
            }
            SignalUpdate();
            return true;
        }

        std::optional<bool> IsParentClock(u32 deviceCode, u32 parentDeviceCode) const {
            if (!IsSupportedDeviceCode(deviceCode) || !IsSupportedDeviceCode(parentDeviceCode))
                return std::nullopt;

            std::scoped_lock lock{mutex};
            const auto it{parentClocks.find(deviceCode)};
            return it != parentClocks.end() && it->second == parentDeviceCode;
        }

        void NotifyTemperature(float temperature) {
            std::scoped_lock lock{mutex};
            notifiedTemperature = temperature;
        }

        float GetNotifiedTemperature() const {
            std::scoped_lock lock{mutex};
            return notifiedTemperature;
        }
    };
}
