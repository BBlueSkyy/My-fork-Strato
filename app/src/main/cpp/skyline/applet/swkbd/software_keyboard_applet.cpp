// SPDX-License-Identifier: MPL-2.0
// Copyright © 2022 Skyline Team and Contributors (https://github.com/skyline-emu/)
// Copyright © 2019-2022 Ryujinx Team and Contributors

#include <codecvt>
#include <services/am/storage/ObjIStorage.h>
#include <common/settings.h>
#include "software_keyboard_applet.h"
#include <jvm.h>

class Utf8Utf16Converter : public std::codecvt<char16_t, char8_t, std::mbstate_t> {
  public:
    ~Utf8Utf16Converter() override = default;
};

namespace skyline::applet::swkbd {
    static size_t WriteStringToSpan(span<u8> chars, std::u16string_view text, bool useUtf8Storage) {
        if (useUtf8Storage) {
            auto u8chars{chars.cast<char8_t>()};
            Utf8Utf16Converter::state_type convert_state{};
            const char16_t *from_next{text.data()};
            char8_t *to_next{u8chars.data()};
            Utf8Utf16Converter().out(convert_state, text.data(), text.end(), from_next, u8chars.data(), u8chars.end().base(), to_next);
            // Null terminate the string, if it isn't out of bounds
            if (to_next < u8chars.end().base())
                *to_next = u8'\0';
            return static_cast<size_t>(to_next - u8chars.data());
        } else {
            const auto textBytes{std::min(text.size() * sizeof(char16_t), chars.size())};
            std::memcpy(chars.data(), text.data(), textBytes);
            // Null terminate the string, if it isn't out of bounds
            if (textBytes < chars.size())
                *reinterpret_cast<char16_t *>(chars.data() + textBytes) = u'\0';
            return textBytes;
        }
    }

    static std::u16string ReadStringFromSpan(span<u8> chars, bool useUtf8Storage) {
        if (!useUtf8Storage) {
            auto utf16Chars{chars.cast<char16_t>()};
            const auto end{std::find(utf16Chars.begin(), utf16Chars.end(), u'\0')};
            return {utf16Chars.begin(), end};
        }

        auto utf8Chars{chars.cast<char8_t>()};
        const auto utf8End{std::find(utf8Chars.begin(), utf8Chars.end(), u8'\0')};
        std::u16string text(chars.size() / sizeof(char16_t), u'\0');
        Utf8Utf16Converter::state_type convertState{};
        const char8_t *fromNext{utf8Chars.data()};
        char16_t *toNext{text.data()};
        Utf8Utf16Converter().in(convertState, utf8Chars.data(), utf8End.base(), fromNext,
                                text.data(), text.data() + text.size(), toNext);
        text.resize(static_cast<size_t>(toNext - text.data()));
        return text;
    }

    SoftwareKeyboardApplet::ValidationRequest::ValidationRequest(std::u16string_view text, bool useUtf8Storage) : size{} {
        const auto textBytes{WriteStringToSpan(chars, text, useUtf8Storage)};
        const size_t terminatorBytes{useUtf8Storage ? sizeof(char8_t) : sizeof(char16_t)};
        size = std::min(chars.size(), textBytes + terminatorBytes);
    }

    SoftwareKeyboardApplet::OutputResult::OutputResult(CloseResult closeResult, std::u16string_view text, bool useUtf8Storage) : closeResult{closeResult} {
        WriteStringToSpan(chars, text, useUtf8Storage);
    }

    static std::u16string FillDefaultText(u32 minLength, u32 maxLength) {
        std::u16string text{u"Skyline"};
        while (text.size() < minLength)
            text += u"Emulator" + text;
        if (text.size() > maxLength)
            text.resize(maxLength);
        return text;
    }

    void SoftwareKeyboardApplet::SendResult() {
        if (dialog) {
            state.jvm->CloseKeyboard(dialog);
            dialog = {};
        }
        PushNormalDataAndSignal(std::make_shared<service::am::ObjIStorage<OutputResult>>(state, manager, OutputResult{currentResult, currentText, config.commonConfig.isUseUtf8}));
        onAppletStateChanged->Signal();
    }

    SoftwareKeyboardApplet::SoftwareKeyboardApplet(
        const DeviceState &state,
        service::ServiceManager &manager,
        std::shared_ptr<kernel::type::KEvent> onAppletStateChanged,
        std::shared_ptr<kernel::type::KEvent> onNormalDataPushFromApplet,
        std::shared_ptr<kernel::type::KEvent> onInteractiveDataPushFromApplet,
        service::applet::LibraryAppletMode appletMode)
        : IApplet{state,
                  manager,
                  std::move(onAppletStateChanged),
                  std::move(onNormalDataPushFromApplet),
                  std::move(onInteractiveDataPushFromApplet),
                  appletMode}, mode{appletMode} {
    }

    Result SoftwareKeyboardApplet::Start() {
        if (mode != service::applet::LibraryAppletMode::AllForeground) {
            LOGW("Stubbing out InlineKeyboard!");
            SendResult();
            return {};
        }

        std::scoped_lock lock{normalInputDataMutex};
        auto commonArgs{normalInputData.front()->GetSpan().as<service::applet::CommonArguments>()};
        normalInputData.pop();

        auto configSpan{normalInputData.front()->GetSpan()};
        normalInputData.pop();
        config = [&] {
            if (commonArgs.apiVersion < 0x30007)
                return KeyboardConfigVB{configSpan.as<KeyboardConfigV0>()};
            else if (commonArgs.apiVersion < 0x6000B)
                return KeyboardConfigVB{configSpan.as<KeyboardConfigV7>()};
            else
                return configSpan.as<KeyboardConfigVB>();
        }();
        LOGD("Swkbd Config:\n* KeyboardMode: {}\n* InvalidCharFlags: {:#09b}\n* TextMaxLength: {}\n* TextMinLength: {}\n* PasswordMode: {}\n* InputFormMode: {}\n* IsUseNewLine: {}\n* IsUseTextCheck: {}",
                      static_cast<u32>(config.commonConfig.keyboardMode),
                      config.commonConfig.invalidCharFlags.raw,
                      config.commonConfig.textMaxLength,
                      config.commonConfig.textMinLength,
                      static_cast<u32>(config.commonConfig.passwordMode),
                      static_cast<u32>(config.commonConfig.inputFormMode),
                      config.commonConfig.isUseNewLine,
                      config.commonConfig.isUseTextCheck
        );

        auto maxChars{static_cast<u32>(SwkbdTextBytes / (config.commonConfig.isUseUtf8 ? sizeof(char8_t) : sizeof(char16_t)))};
        config.commonConfig.textMaxLength = std::min(config.commonConfig.textMaxLength, maxChars);
        if (config.commonConfig.textMaxLength == 0)
            config.commonConfig.textMaxLength = maxChars;
        config.commonConfig.textMinLength = std::min(config.commonConfig.textMinLength, config.commonConfig.textMaxLength);

        if (config.commonConfig.textMaxLength > MaxOneLineChars)
            config.commonConfig.inputFormMode = InputFormMode::MultiLine;

        if (!normalInputData.empty() && config.commonConfig.initialStringLength > 0) {
            const auto initialData{normalInputData.front()->GetSpan()};
            const auto offset{static_cast<size_t>(config.commonConfig.initialStringOffset)};
            const auto length{static_cast<size_t>(config.commonConfig.initialStringLength)};
            if (offset > initialData.size() || length > (initialData.size() - offset) / sizeof(char16_t))
                throw exception("Software keyboard initial text is out of bounds");
            const auto initialChars{initialData.subspan(offset).cast<char16_t>()};
            currentText.assign(initialChars.data(), length);
        }

        dialog = state.jvm->ShowKeyboard(*reinterpret_cast<JvmManager::KeyboardConfig *>(&config), currentText);
        if (!dialog) {
            LOGW("Couldn't show keyboard dialog, using default text");
            currentResult = CloseResult::Enter;
            currentText = FillDefaultText(config.commonConfig.textMinLength, config.commonConfig.textMaxLength);
        } else {
            auto result{state.jvm->WaitForSubmitOrCancel(dialog)};
            currentResult = static_cast<CloseResult>(result.first);
            currentText = result.second;
        }
        if (config.commonConfig.isUseTextCheck && currentResult == CloseResult::Enter) {
            PushInteractiveDataAndSignal(std::make_shared<service::am::ObjIStorage<ValidationRequest>>(state, manager, ValidationRequest{currentText, config.commonConfig.isUseUtf8}));
            validationPending = true;
        } else {
            SendResult();
        }
        return {};
    }

    Result SoftwareKeyboardApplet::GetResult() {
        return {};
    }

    void SoftwareKeyboardApplet::PushNormalDataToApplet(std::shared_ptr<service::am::IStorage> data) {
        PushNormalInput(data);
    }

    void SoftwareKeyboardApplet::PushInteractiveDataToApplet(std::shared_ptr<service::am::IStorage> data) {
        if (validationPending) {
            auto dataSpan{data->GetSpan()};
            auto validationResult{dataSpan.as<ValidationResult>()};
            const auto message{ReadStringFromSpan(
                {reinterpret_cast<u8 *>(validationResult.chars.data()), sizeof(validationResult.chars)},
                config.commonConfig.isUseUtf8)};
            if (validationResult.result == TextCheckResult::Success) {
                validationPending = false;
                SendResult();
            } else {
                if (validationResult.result != TextCheckResult::ShowFailureDialog &&
                    validationResult.result != TextCheckResult::ShowConfirmDialog &&
                    validationResult.result != TextCheckResult::Silent)
                    throw exception("Unknown software keyboard text-check result: 0x{:X}",
                                    static_cast<u32>(validationResult.result));

                if (dialog) {
                    const bool accepted{validationResult.result != TextCheckResult::Silent &&
                                        static_cast<CloseResult>(state.jvm->ShowValidationResult(
                                            dialog,
                                            static_cast<JvmManager::KeyboardTextCheckResult>(validationResult.result),
                                            message)) == CloseResult::Enter};
                    if (accepted) {
                        // Accepted on confirmation dialog
                        validationPending = false;
                        SendResult();
                    } else {
                        // Cancelled or failed validation, go back to waiting for text
                        auto result{state.jvm->WaitForSubmitOrCancel(dialog)};
                        currentResult = static_cast<CloseResult>(result.first);
                        currentText = result.second;
                        if (currentResult == CloseResult::Enter) {
                            PushInteractiveDataAndSignal(std::make_shared<service::am::ObjIStorage<ValidationRequest>>(state, manager, ValidationRequest{currentText, config.commonConfig.isUseUtf8}));
                        } else {
                            validationPending = false;
                            SendResult();
                        }
                    }
                } else {
                    std::array<u8, SwkbdTextBytes> chars{};
                    WriteStringToSpan(chars, message, true);
                    std::string message{reinterpret_cast<char *>(chars.data())};
                    if (validationResult.result == TextCheckResult::ShowFailureDialog)
                        LOGW("Sending default text despite being rejected by the guest with message: \"{}\"", message);
                    else if (validationResult.result == TextCheckResult::ShowConfirmDialog)
                        LOGD("Guest asked to confirm default text with message: \"{}\"", message);
                    validationPending = false;
                    SendResult();
                }
            }
        }
    }
}