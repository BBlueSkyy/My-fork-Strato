// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <common.h>

namespace skyline::service::hid::result {
    constexpr Result InvalidNpadHandle(202, 100);
    constexpr Result InvalidNpadDeviceIndex(202, 107);
    constexpr Result VibrationNotInitialized(202, 121);
    constexpr Result VibrationInvalidStyleIndex(202, 122);
    constexpr Result VibrationInvalidNpadId(202, 123);
    constexpr Result VibrationDeviceIndexOutOfRange(202, 124);
    constexpr Result VibrationStrengthOutOfRange(202, 126);
    constexpr Result VibrationArraySizeMismatch(202, 131);
    constexpr Result InvalidSixAxisFusionRange(202, 423);
    constexpr Result InvalidNpadId(202, 709);
    constexpr Result InvalidArraySize(202, 715);
    constexpr Result UndefinedStyleSet(202, 716);
    constexpr Result MultipleStyleSetsSelected(202, 717);
    constexpr Result AppletResourceNotInitialized(202, 1042);
    constexpr Result AruidAlreadyRegistered(202, 1046);
    constexpr Result AruidNotRegistered(202, 1047);
}
