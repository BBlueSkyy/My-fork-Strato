// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <array>
#include <mutex>
#include <optional>
#include <common.h>
#include <common/settings.h>

namespace skyline::service::apm {
    enum class PerformanceMode : i32 {
        Invalid = -1,
        Normal = 0,
        Boost = 1,
    };

    enum class PerformanceConfiguration : u32 {
        Invalid = 0,
        Config1 = 0x00010000,
        Config2 = 0x00010001,
        Config3 = 0x00010002,
        Config4 = 0x00020000,
        Config5 = 0x00020001,
        Config6 = 0x00020002,
        Config7 = 0x00020003,
        Config8 = 0x00020004,
        Config9 = 0x00020005,
        Config10 = 0x00020006,
        Config11 = 0x92220007,
        Config12 = 0x92220008,
        Config13 = 0x92220009,
        Config14 = 0x9222000A,
        Config15 = 0x9222000B,
        Config16 = 0x9222000C,
    };

    class PerformanceState {
      private:
        const DeviceState &state;
        mutable std::mutex mutex;
        std::array<PerformanceConfiguration, 2> configurations{
            PerformanceConfiguration::Config7,
            PerformanceConfiguration::Config7,
        };
        bool cpuOverclockEnabled{};

        static std::optional<size_t> ModeIndex(PerformanceMode mode) {
            switch (mode) {
                case PerformanceMode::Normal: return 0;
                case PerformanceMode::Boost: return 1;
                default: return std::nullopt;
            }
        }

        static bool IsConfigurationAvailable(PerformanceMode mode, PerformanceConfiguration configuration) {
            switch (configuration) {
                case PerformanceConfiguration::Config1:
                case PerformanceConfiguration::Config5:
                case PerformanceConfiguration::Config7:
                case PerformanceConfiguration::Config8:
                case PerformanceConfiguration::Config9:
                case PerformanceConfiguration::Config10:
                case PerformanceConfiguration::Config11:
                case PerformanceConfiguration::Config12:
                case PerformanceConfiguration::Config13:
                case PerformanceConfiguration::Config14:
                case PerformanceConfiguration::Config15:
                case PerformanceConfiguration::Config16:
                    return true;

                // These profiles are only exposed while docked.
                case PerformanceConfiguration::Config2:
                case PerformanceConfiguration::Config4:
                    return mode == PerformanceMode::Boost;

                // These profiles are SDEV-only and are not exposed by a retail NX.
                case PerformanceConfiguration::Config3:
                case PerformanceConfiguration::Config6:
                case PerformanceConfiguration::Invalid:
                default:
                    return false;
            }
        }

      public:
        explicit PerformanceState(const DeviceState &state) : state(state) {}

        PerformanceMode GetCurrentPerformanceMode() const {
            return *state.settings->isDocked ? PerformanceMode::Boost : PerformanceMode::Normal;
        }

        bool SetPerformanceConfiguration(PerformanceMode mode, PerformanceConfiguration configuration) {
            const auto index{ModeIndex(mode)};
            if (!index || !IsConfigurationAvailable(mode, configuration))
                return false;

            std::scoped_lock lock{mutex};
            configurations[*index] = configuration;
            return true;
        }

        std::optional<PerformanceConfiguration> GetPerformanceConfiguration(PerformanceMode mode) const {
            const auto index{ModeIndex(mode)};
            if (!index)
                return std::nullopt;

            std::scoped_lock lock{mutex};
            return configurations[*index];
        }

        void SetCpuOverclockEnabled(bool enabled) {
            std::scoped_lock lock{mutex};
            cpuOverclockEnabled = enabled;
        }

        bool IsCpuOverclockEnabled() const {
            std::scoped_lock lock{mutex};
            return cpuOverclockEnabled;
        }
    };
}
