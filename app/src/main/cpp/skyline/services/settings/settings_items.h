// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Strato Contributors

#pragma once

#include <common.h>

namespace skyline::service::settings {
    // Explicit HLE configuration. Do not synthesize a value for an unknown key.
    struct SettingsItem {
        std::string_view category;
        std::string_view name;
        u64 value;
        size_t size;
    };

    inline constexpr std::array SettingsItems{
        SettingsItem{"time", "notify_time_to_fs_interval_seconds", 600, sizeof(i32)},
        SettingsItem{"time", "standard_network_clock_sufficient_accuracy_minutes", 43200, sizeof(i32)},
        SettingsItem{"time", "standard_steady_clock_rtc_update_interval_minutes", 5, sizeof(i32)},
        SettingsItem{"time", "standard_steady_clock_test_offset_minutes", 0, sizeof(i32)},
        SettingsItem{"time", "standard_user_clock_initial_year", 2019, sizeof(i32)},
        SettingsItem{"settings_debug", "is_debug_mode_enabled", 0, sizeof(u8)},
        SettingsItem{"hbloader", "applet_heap_size", 0, sizeof(u64)},
        SettingsItem{"hbloader", "applet_heap_reservation_size", 0x8600000, sizeof(u64)},
        SettingsItem{"hid", "has_rail_interface", 1, sizeof(u8)},
        SettingsItem{"hid", "has_sio_mcu", 1, sizeof(u8)},
        SettingsItem{"hid_debug", "emulate_future_device", 0, sizeof(u8)},
        SettingsItem{"hid_debug", "emulate_mcu_hardware_error", 0, sizeof(u8)},
        SettingsItem{"hid_debug", "emulate_firmware_update_failure", 0, sizeof(u8)},
        SettingsItem{"hid_debug", "failure_firmware_update", 0, sizeof(i32)},
        SettingsItem{"hid_debug", "touch_firmware_auto_update_disabled", 0, sizeof(u8)},
        SettingsItem{"mii", "is_db_test_mode_enabled", 0, sizeof(u8)},
        SettingsItem{"err", "applet_auto_close", 0, sizeof(u8)},
    };

    inline const SettingsItem *FindSettingsItem(std::string_view category, std::string_view name) {
        for (const auto &item : SettingsItems)
            if (item.category == category && item.name == name)
                return &item;
        return nullptr;
    }
}
