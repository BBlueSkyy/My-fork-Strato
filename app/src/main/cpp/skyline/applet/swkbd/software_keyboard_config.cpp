// SPDX-License-Identifier: MPL-2.0
// Copyright © 2022 Skyline Team and Contributors (https://github.com/skyline-emu/)
// Copyright © 2019-2022 Ryujinx Team and Contributors

#include "software_keyboard_config.h"
#include "software_keyboard_text.h"

namespace skyline::applet::swkbd {
    KeyboardConfigVB::KeyboardConfigVB() = default;

    KeyboardConfigVB::KeyboardConfigVB(const KeyboardConfigV7 &v7config) : commonConfig{v7config.commonConfig}, separateTextPos{v7config.separateTextPos} {}

    KeyboardConfigVB::KeyboardConfigVB(const KeyboardConfigV0 &v0config) : commonConfig{v0config.commonConfig} {}

    void NormalizeNormalConfig(KeyboardConfigVB &config) {
        constexpr u32 MaxOneLineChars{32};
        const u32 maxCharacters{static_cast<u32>(SwkbdTextBytes / (config.commonConfig.isUseUtf8 ? sizeof(char8_t) : sizeof(char16_t)))};
        if (config.commonConfig.textMaxLength == 0)
            config.commonConfig.textMaxLength = maxCharacters;
        else
            config.commonConfig.textMaxLength = std::min(config.commonConfig.textMaxLength, maxCharacters);
        config.commonConfig.textMinLength = std::min(config.commonConfig.textMinLength, config.commonConfig.textMaxLength);
        if (config.commonConfig.textMaxLength > MaxOneLineChars)
            config.commonConfig.inputFormMode = InputFormMode::MultiLine;
    }

    std::optional<std::u16string> ReadInitialText(span<const u8> storage, size_t byteOffset, size_t codeUnits) {
        return ReadUtf16Text(storage, byteOffset, codeUnits);
    }
}
