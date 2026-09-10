// SPDX-License-Identifier: MPL-2.0
// Copyright © 2022 Skyline Team and Contributors (https://github.com/skyline-emu/)
// Copyright © 2019-2022 Ryujinx Team and Contributors

#include <codecvt>
#include <services/am/storage/ObjIStorage.h>
#include <services/am/storage/VectorIStorage.h>
#include <common/settings.h>
#include "software_keyboard_applet.h"
#include <jvm.h>

class Utf8Utf16Converter : public std::codecvt<char16_t, char8_t, std::mbstate_t> {
  public:
    ~Utf8Utf16Converter() override = default;
};

namespace skyline::applet::swkbd {
    namespace {
        constexpr size_t InlineReplyHeaderSize{sizeof(u32) * 2};
        constexpr size_t InlineInputTextBytes{0x3F4};
        constexpr size_t InlineUtf16TextBytes{0x3EC};
        constexpr size_t InlineUtf8TextBytes{0x7D4};
        constexpr size_t InlineCalcOldSize{0x4A0};
        constexpr size_t InlineCalcNewSize{0x4E8};

        constexpr u64 InlineFlagInitialize{0x1};
        constexpr u64 InlineFlagAppear{0x4};
        constexpr u64 InlineFlagSetInputText{0x8};
        constexpr u64 InlineFlagSetCursorPosition{0x10};
        constexpr u64 InlineFlagSetUtf8Mode{0x20};
        constexpr u64 InlineFlagUnsetCustomizeDictionary{0x40};
        constexpr u64 InlineFlagDisappear{0x80};
        constexpr u64 InlineFlagUnsetUserWordInfo{0x400};

        template<typename T>
        T ReadInlineValue(span<u8> data, size_t offset) {
            if (offset + sizeof(T) > data.size())
                throw exception("Software keyboard inline request is truncated");

            T value{};
            std::memcpy(&value, data.data() + offset, sizeof(T));
            return value;
        }

        template<typename T>
        void WriteInlineValue(std::vector<u8> &data, size_t offset, T value) {
            if (offset + sizeof(T) > data.size())
                throw exception("Software keyboard inline reply is too small");

            std::memcpy(data.data() + offset, &value, sizeof(T));
        }

        std::u16string ReadInlineString(span<u8> data, size_t offset, size_t size) {
            if (offset + size > data.size())
                throw exception("Software keyboard inline string is truncated");

            std::u16string text;
            text.reserve(size / sizeof(char16_t));
            for (size_t index{}; index < size; index += sizeof(char16_t)) {
                const auto character{ReadInlineValue<char16_t>(data, offset + index)};
                if (character == u'\0')
                    break;
                text.push_back(character);
            }
            return text;
        }
    }

    static void WriteStringToSpan(span<u8> chars, std::u16string_view text, bool useUtf8Storage) {
        if (useUtf8Storage) {
            auto u8chars{chars.cast<char8_t>()};
            Utf8Utf16Converter::state_type convert_state{};
            const char16_t *from_next;
            char8_t *to_next;
            Utf8Utf16Converter().out(convert_state, text.data(), text.end(), from_next, u8chars.data(), u8chars.end().base(), to_next);
            // Null terminate the string, if it isn't out of bounds
            if (to_next < u8chars.end().base())
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
        if (dialog)
            state.jvm->CloseKeyboard(dialog);
        PushNormalDataAndSignal(std::make_shared<service::am::ObjIStorage<OutputResult>>(state, manager, OutputResult{currentResult, currentText, config.commonConfig.isUseUtf8}));
        onAppletStateChanged->Signal();
    }

    Result SoftwareKeyboardApplet::StartInline() {
        std::scoped_lock lock{normalInputDataMutex};
        if (normalInputData.size() < 2)
            throw exception("Software keyboard inline mode requires common arguments and InitializeArg");

        const auto commonArguments{normalInputData.front()->GetSpan()};
        if (commonArguments.size() < sizeof(service::applet::CommonArguments))
            throw exception("Software keyboard common arguments are truncated");
        normalInputData.pop();

        const auto initializeArgument{normalInputData.front()->GetSpan()};
        if (initializeArgument.size() != sizeof(u64))
            throw exception("Software keyboard inline InitializeArg has an invalid size");

        const bool partialForeground{ReadInlineValue<u8>(initializeArgument, sizeof(u32)) != 0};
        const auto expectedMode{partialForeground
                                    ? service::applet::LibraryAppletMode::PartialForeground
                                    : service::applet::LibraryAppletMode::PartialForegroundWithIndirectDisplay};
        if (mode != expectedMode)
            LOGW("Inline keyboard InitializeArg mode does not match LibraryAppletMode (expected=0x{:X}, actual=0x{:X})",
                 static_cast<u32>(expectedMode), static_cast<u32>(mode));

        normalInputData.pop();
        inlineState = InlineState::Uninitialized;
        return {};
    }

    void SoftwareKeyboardApplet::ChangeInlineState(InlineState state) {
        inlineState = state;
        SendInlineReply(InlineReply::Default);
    }

    void SoftwareKeyboardApplet::SendInlineReply(InlineReply reply) {
        const size_t size{InlineReplyHeaderSize + (reply == InlineReply::FinishedInitialize ? 1 : 0)};
        std::vector<u8> response(size);
        WriteInlineValue(response, 0, inlineState);
        WriteInlineValue(response, sizeof(u32), reply);
        if (reply == InlineReply::FinishedInitialize)
            response[InlineReplyHeaderSize] = 1;

        PushInteractiveDataAndSignal(std::make_shared<service::am::VectorIStorage>(state, manager, std::move(response)));
    }

    void SoftwareKeyboardApplet::SendInlineTextReply(InlineReply reply) {
        const bool utf8{reply == InlineReply::ChangedStringUtf8 ||
                        reply == InlineReply::ChangedStringUtf8V2 ||
                        reply == InlineReply::DecidedEnterUtf8};
        const bool changed{reply == InlineReply::ChangedString ||
                           reply == InlineReply::ChangedStringV2 ||
                           reply == InlineReply::ChangedStringUtf8 ||
                           reply == InlineReply::ChangedStringUtf8V2};
        const bool decidedEnter{reply == InlineReply::DecidedEnter || reply == InlineReply::DecidedEnterUtf8};
        const bool version2{reply == InlineReply::ChangedStringV2 || reply == InlineReply::ChangedStringUtf8V2};
        if (!changed && !decidedEnter)
            throw exception("Invalid software keyboard inline text reply");

        const size_t textBytes{utf8 ? InlineUtf8TextBytes : InlineUtf16TextBytes};
        const size_t argumentSize{changed ? sizeof(u32) * 4 : sizeof(u32)};
        std::vector<u8> response(InlineReplyHeaderSize + textBytes + argumentSize + (version2 ? 1 : 0));

        WriteInlineValue(response, 0, inlineState);
        WriteInlineValue(response, sizeof(u32), reply);
        WriteStringToSpan({response.data() + InlineReplyHeaderSize, textBytes}, currentText, utf8);

        const size_t argumentOffset{InlineReplyHeaderSize + textBytes};
        WriteInlineValue(response, argumentOffset, static_cast<u32>(currentText.size()));
        if (changed) {
            WriteInlineValue(response, argumentOffset + sizeof(u32), static_cast<i32>(-1));
            WriteInlineValue(response, argumentOffset + sizeof(u32) * 2, static_cast<i32>(-1));
            WriteInlineValue(response, argumentOffset + sizeof(u32) * 3,
                             std::clamp(inlineCursorPosition, 0, static_cast<i32>(currentText.size())));
        }

        PushInteractiveDataAndSignal(std::make_shared<service::am::VectorIStorage>(state, manager, std::move(response)));
    }

    void SoftwareKeyboardApplet::ConfigureInlineKeyboard(span<u8> calc, bool extendedLayout) {
        const size_t appearOffset{0x20};

        config = KeyboardConfigVB{};
        config.commonConfig.keyboardMode = ReadInlineValue<KeyboardMode>(calc, appearOffset);
        std::memcpy(config.commonConfig.okText.data(), calc.data() + appearOffset + 0x4, sizeof(config.commonConfig.okText));
        config.commonConfig.leftOptionalSymbolKey = ReadInlineValue<char16_t>(calc, appearOffset + 0x16);
        config.commonConfig.rightOptionalSymbolKey = ReadInlineValue<char16_t>(calc, appearOffset + 0x18);
        config.commonConfig.isPredictionEnabled = ReadInlineValue<u8>(calc, appearOffset + 0x1A) != 0;
        config.commonConfig.invalidCharFlags = ReadInlineValue<InvalidCharFlags>(calc, appearOffset + 0x1C);
        config.commonConfig.textMaxLength = ReadInlineValue<u32>(calc, appearOffset + 0x20);
        config.commonConfig.textMinLength = ReadInlineValue<u32>(calc, appearOffset + 0x24);
        config.commonConfig.isUseNewLine = ReadInlineValue<u8>(calc, appearOffset + 0x28) != 0;
        config.isCancelButtonDisabled = ReadInlineValue<u8>(calc, appearOffset + 0x1B) != 0;

        constexpr u32 InlineMaxTextLength{500};
        if (config.commonConfig.textMaxLength == 0 || config.commonConfig.textMaxLength > InlineMaxTextLength)
            config.commonConfig.textMaxLength = InlineMaxTextLength;
        config.commonConfig.textMinLength = std::min(config.commonConfig.textMinLength, config.commonConfig.textMaxLength);
        config.commonConfig.passwordMode = PasswordMode::Show;
        config.commonConfig.inputFormMode = config.commonConfig.textMaxLength > MaxOneLineChars
                                                ? InputFormMode::MultiLine
                                                : InputFormMode::OneLine;
        config.commonConfig.initialCursorPos = inlineCursorPosition > 0
                                                   ? InitialCursorPos::Last
                                                   : InitialCursorPos::First;
        config.commonConfig.isUseUtf8 = inlineUseUtf8;
    }

    void SoftwareKeyboardApplet::ShowInlineKeyboard() {
        ChangeInlineState(InlineState::Appearing);
        dialog = state.jvm->ShowKeyboard(*reinterpret_cast<JvmManager::KeyboardConfig *>(&config), currentText);
        ChangeInlineState(InlineState::Shown);

        if (!dialog) {
            LOGW("Couldn't show inline keyboard dialog");
            SendInlineReply(InlineReply::DecidedCancel);
        } else {
            auto result{state.jvm->WaitForSubmitOrCancel(dialog)};
            currentResult = static_cast<CloseResult>(result.first);
            currentText = std::move(result.second);
            inlineCursorPosition = static_cast<i32>(currentText.size());

            if (currentResult == CloseResult::Enter) {
                SendInlineTextReply(inlineUseUtf8 ? InlineReply::DecidedEnterUtf8 : InlineReply::DecidedEnter);
            } else {
                SendInlineReply(InlineReply::DecidedCancel);
            }
        }

        HideInlineKeyboard();
    }

    void SoftwareKeyboardApplet::HideInlineKeyboard() {
        if (inlineState != InlineState::Shown)
            return;

        ChangeInlineState(InlineState::Disappearing);
        if (dialog) {
            state.jvm->CloseKeyboard(dialog);
            dialog = {};
        }
        ChangeInlineState(InlineState::Hidden);
    }

    void SoftwareKeyboardApplet::ProcessInlineCalc(span<u8> calc) {
        if (calc.size() < 0x18)
            throw exception("Software keyboard inline Calc is truncated");

        const u16 declaredSize{ReadInlineValue<u16>(calc, 0x4)};
        const bool extendedLayout{declaredSize == InlineCalcNewSize ||
                                  (declaredSize != InlineCalcOldSize && calc.size() == InlineCalcNewSize)};
        const size_t expectedSize{extendedLayout ? InlineCalcNewSize : InlineCalcOldSize};
        if (declaredSize != expectedSize || calc.size() < expectedSize) {
            LOGW("Unsupported inline keyboard Calc size (declared=0x{:X}, storage=0x{:X})", declaredSize, calc.size());
            return;
        }

        const u64 flags{ReadInlineValue<u64>(calc, 0x8)};
        const size_t cursorOffset{0x1C};
        const size_t inputTextOffset{static_cast<size_t>(extendedLayout ? 0x90 : 0x68)};
        const size_t utf8Offset{static_cast<size_t>(extendedLayout ? 0x484 : 0x45C)};

        if (flags & InlineFlagSetInputText)
            currentText = ReadInlineString(calc, inputTextOffset, InlineInputTextBytes);
        if (flags & InlineFlagSetCursorPosition)
            inlineCursorPosition = ReadInlineValue<i32>(calc, cursorOffset);
        if (flags & InlineFlagSetUtf8Mode)
            inlineUseUtf8 = ReadInlineValue<u8>(calc, utf8Offset) != 0;

        if (flags & InlineFlagUnsetCustomizeDictionary)
            SendInlineReply(InlineReply::UnsetCustomizeDictionary);
        if (flags & InlineFlagUnsetUserWordInfo)
            SendInlineReply(InlineReply::ReleasedUserWordInfo);

        const bool initialize{(flags & InlineFlagInitialize) != 0};
        if (initialize && inlineState == InlineState::Uninitialized) {
            ConfigureInlineKeyboard(calc, extendedLayout);
            ChangeInlineState(InlineState::Hidden);
            SendInlineReply(InlineReply::FinishedInitialize);
        }

        if (!initialize && (flags & (InlineFlagSetInputText | InlineFlagSetCursorPosition))) {
            const InlineReply reply{inlineUseUtf8
                                        ? (inlineUseChangedStringV2 ? InlineReply::ChangedStringUtf8V2 : InlineReply::ChangedStringUtf8)
                                        : (inlineUseChangedStringV2 ? InlineReply::ChangedStringV2 : InlineReply::ChangedString)};
            SendInlineTextReply(reply);
        }

        if ((flags & InlineFlagAppear) && inlineState == InlineState::Hidden) {
            ConfigureInlineKeyboard(calc, extendedLayout);
            ShowInlineKeyboard();
        } else if ((flags & InlineFlagDisappear) && inlineState == InlineState::Shown) {
            HideInlineKeyboard();
        }
    }

    void SoftwareKeyboardApplet::ProcessInlineRequest(span<u8> data) {
        if (data.size() < sizeof(InlineRequest)) {
            LOGW("Software keyboard inline request is truncated");
            return;
        }

        const auto request{ReadInlineValue<InlineRequest>(data, 0)};
        switch (request) {
            case InlineRequest::Finalize:
                if (dialog) {
                    state.jvm->CloseKeyboard(dialog);
                    dialog = {};
                }
                ChangeInlineState(InlineState::Uninitialized);
                onAppletStateChanged->Signal();
                break;
            case InlineRequest::SetUserWordInfo:
                SendInlineReply(InlineReply::ReleasedUserWordInfo);
                break;
            case InlineRequest::SetCustomizeDictionary:
            case InlineRequest::SetCustomizedDictionaries:
                break;
            case InlineRequest::Calc:
                ProcessInlineCalc(data.subspan(sizeof(InlineRequest)));
                break;
            case InlineRequest::UnsetCustomizedDictionaries:
                SendInlineReply(InlineReply::UnsetCustomizedDictionaries);
                break;
            case InlineRequest::SetChangedStringV2:
                if (data.size() >= sizeof(InlineRequest) + sizeof(u8))
                    inlineUseChangedStringV2 = ReadInlineValue<u8>(data, sizeof(InlineRequest)) != 0;
                break;
            case InlineRequest::SetMovedCursorV2:
                break;
            default:
                LOGW("Unknown software keyboard inline request: 0x{:X}", static_cast<u32>(request));
                break;
        }
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
        if (mode == service::applet::LibraryAppletMode::PartialForeground ||
            mode == service::applet::LibraryAppletMode::PartialForegroundWithIndirectDisplay)
            return StartInline();
        if (mode != service::applet::LibraryAppletMode::AllForeground)
            throw exception("Invalid LibraryAppletMode for software keyboard");

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

        if (!normalInputData.empty() && config.commonConfig.initialStringLength > 0)
            currentText = std::u16string(normalInputData.front()->GetSpan().subspan(config.commonConfig.initialStringOffset).cast<char16_t>().data(), config.commonConfig.initialStringLength);

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
        if (mode == service::applet::LibraryAppletMode::PartialForeground ||
            mode == service::applet::LibraryAppletMode::PartialForegroundWithIndirectDisplay) {
            ProcessInlineRequest(data->GetSpan());
            return;
        }

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
