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
    static void WriteStringToSpan(span<u8> chars, std::u16string_view text, bool useUtf8Storage) {
        if (useUtf8Storage) {
            auto u8chars{chars.cast<char8_t>()};
            Utf8Utf16Converter::state_type convert_state;
            const char16_t *from_next;
            char8_t *to_next;
            Utf8Utf16Converter().out(convert_state, text.data(), text.end(), from_next, u8chars.data(), u8chars.end().base(), to_next);
            // Null terminate the string, if it isn't out of bounds
            if (to_next < reinterpret_cast<const char8_t *>(text.end()))
                *to_next = u8'\0';
        } else {
            std::memcpy(chars.data(), text.data(), std::min(text.size() * sizeof(char16_t), chars.size()));
            // Null terminate the string, if it isn't out of bounds
            if (text.size() * sizeof(char16_t) < chars.size())
                *(reinterpret_cast<char16_t *>(chars.data()) + text.size()) = u'\0';
        }
    }

    SoftwareKeyboardApplet::ValidationRequest::ValidationRequest(std::u16string_view text, bool useUtf8Storage) : size{sizeof(ValidationRequest)} {
        WriteStringToSpan(chars, text, useUtf8Storage);
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
        LOGD("Swkbd trace: SendResult entering (result={}, textLength={}, utf8={}, dialog={})",
             static_cast<u32>(currentResult), currentText.size(), config.commonConfig.isUseUtf8, dialog != nullptr);
        if (dialog) {
            LOGD("Swkbd trace: closing host keyboard dialog");
            state.jvm->CloseKeyboard(dialog);
            LOGD("Swkbd trace: host keyboard dialog closed");
        }

        OutputResult outputResult{currentResult, currentText, config.commonConfig.isUseUtf8};
        auto outputStorage{std::make_shared<service::am::ObjIStorage<OutputResult>>(state, manager, std::move(outputResult))};

        LOGD("Swkbd trace: pushing normal output (size=0x{:X})", sizeof(OutputResult));
        PushNormalDataAndSignal(std::move(outputStorage));

        LOGD("Swkbd trace: normal output pushed; signalling applet state change");
        onAppletStateChanged->Signal();
        LOGD("Swkbd trace: SendResult completed");
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
        LOGD("Swkbd trace: Start entered (mode=0x{:X})", mode);
        if (mode != service::applet::LibraryAppletMode::AllForeground) {
            LOGW("Stubbing out InlineKeyboard!");
            SendResult();
            return {};
        }

        std::scoped_lock lock{normalInputDataMutex};
        LOGD("Swkbd trace: normal input queue contains {} storage object(s)", normalInputData.size());
        auto commonArgsSpan{normalInputData.front()->GetSpan()};
        auto commonArgs{commonArgsSpan.as<service::applet::CommonArguments>()};
        normalInputData.pop();

        auto configSpan{normalInputData.front()->GetSpan()};
        normalInputData.pop();
        LOGD("Swkbd trace: parsing inputs (apiVersion=0x{:X}, commonArgsSize=0x{:X}, configSize=0x{:X}, remainingStorageCount={})",
             commonArgs.apiVersion, commonArgsSpan.size(), configSpan.size(), normalInputData.size());
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

        const size_t workBufferSize{normalInputData.empty() ? 0 : normalInputData.front()->GetSpan().size()};
        LOGD("Swkbd trace: normalized config (textMinLength={}, textMaxLength={}, inputFormMode={}, initialStringOffset=0x{:X}, initialStringLength={}, workBufferSize=0x{:X})",
             config.commonConfig.textMinLength,
             config.commonConfig.textMaxLength,
             static_cast<u32>(config.commonConfig.inputFormMode),
             config.commonConfig.initialStringOffset,
             config.commonConfig.initialStringLength,
             workBufferSize);

        if (!normalInputData.empty() && config.commonConfig.initialStringLength > 0)
            currentText = std::u16string(normalInputData.front()->GetSpan().subspan(config.commonConfig.initialStringOffset).cast<char16_t>().data(), config.commonConfig.initialStringLength);

        LOGD("Swkbd trace: calling ShowKeyboard (initialTextLength={})", currentText.size());
        dialog = state.jvm->ShowKeyboard(*reinterpret_cast<JvmManager::KeyboardConfig *>(&config), currentText);
        LOGD("Swkbd trace: ShowKeyboard returned (dialog={})", dialog != nullptr);
        if (!dialog) {
            LOGW("Couldn't show keyboard dialog, using default text");
            currentResult = CloseResult::Enter;
            currentText = FillDefaultText(config.commonConfig.textMinLength, config.commonConfig.textMaxLength);
        } else {
            LOGD("Swkbd trace: waiting for submit or cancel");
            auto result{state.jvm->WaitForSubmitOrCancel(dialog)};
            currentResult = static_cast<CloseResult>(result.first);
            currentText = result.second;
            LOGD("Swkbd trace: submit/cancel returned (result={}, textLength={})", static_cast<u32>(currentResult), currentText.size());
        }
        if (config.commonConfig.isUseTextCheck && currentResult == CloseResult::Enter) {
            LOGD("Swkbd trace: pushing interactive validation request");
            PushInteractiveDataAndSignal(std::make_shared<service::am::ObjIStorage<ValidationRequest>>(state, manager, ValidationRequest{currentText, config.commonConfig.isUseUtf8}));
            validationPending = true;
        } else {
            LOGD("Swkbd trace: sending final result without text validation");
            SendResult();
        }
        LOGD("Swkbd trace: Start completed");
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
            if (validationResult.result == TextCheckResult::Success) {
                validationPending = false;
                SendResult();
            } else {
                if (dialog) {
                    if (static_cast<CloseResult>(state.jvm->ShowValidationResult(dialog, static_cast<JvmManager::KeyboardTextCheckResult>(validationResult.result), std::u16string(validationResult.chars.data()))) == CloseResult::Enter) {
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
                            SendResult();
                        }
                    }
                } else {
                    std::array<u8, SwkbdTextBytes> chars{};
                    WriteStringToSpan(chars, std::u16string(validationResult.chars.data()), true);
                    std::string message{reinterpret_cast<char *>(chars.data())};
                    if (validationResult.result == TextCheckResult::ShowFailureDialog)
                        LOGW("Sending default text despite being rejected by the guest with message: \"{}\"", message);
                    else
                        LOGD("Guest asked to confirm default text with message: \"{}\"", message);
                    PushNormalDataAndSignal(std::make_shared<service::am::ObjIStorage<OutputResult>>(state, manager, OutputResult{CloseResult::Enter, currentText, config.commonConfig.isUseUtf8}));
                }
            }
        }
    }
}
