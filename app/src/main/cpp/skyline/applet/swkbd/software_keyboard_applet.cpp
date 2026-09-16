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
    template<typename T>
    static T ReadValue(span<u8> data, size_t offset) {
        T value{};
        if (offset + sizeof(T) <= data.size())
            std::memcpy(&value, data.data() + offset, sizeof(T));
        return value;
    }

    template<typename T>
    static void WriteValue(span<u8> data, size_t offset, T value) {
        if (offset + sizeof(T) <= data.size())
            std::memcpy(data.data() + offset, &value, sizeof(T));
    }

    static void WriteStringToSpan(span<u8> chars, std::u16string_view text, bool useUtf8Storage) {
        std::fill(chars.begin(), chars.end(), 0);
        if (useUtf8Storage) {
            auto u8chars{chars.cast<char8_t>()};
            Utf8Utf16Converter::state_type convertState{};
            const char16_t *fromNext{};
            char8_t *toNext{};
            Utf8Utf16Converter().out(convertState, text.data(), text.end(), fromNext,
                                      u8chars.data(), u8chars.end().base(), toNext);
            if (toNext < u8chars.end().base())
                *toNext = u8'\0';
        } else {
            const size_t bytes{std::min(text.size() * sizeof(char16_t), chars.size())};
            std::memcpy(chars.data(), text.data(), bytes);
            if (bytes + sizeof(char16_t) <= chars.size())
                *reinterpret_cast<char16_t *>(chars.data() + bytes) = u'\0';
        }
    }

    static std::u16string ReadInlineString(span<u8> data, size_t offset, size_t bytes) {
        if (offset >= data.size())
            return {};
        bytes = std::min(bytes, data.size() - offset);
        auto chars{data.subspan(offset, bytes).cast<char16_t>()};
        size_t length{};
        while (length < chars.size() && chars[length])
            ++length;
        return {chars.data(), length};
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

    SoftwareKeyboardApplet::~SoftwareKeyboardApplet() {
        {
            std::scoped_lock lock{inlineMutex};
            inlineStarted = false;
            if (dialog) {
                state.jvm->CloseKeyboard(dialog);
                dialog = {};
            }
            if (pendingInlineWaitDialog) {
                state.jvm->ReleaseKeyboardHandle(pendingInlineWaitDialog);
                pendingInlineWaitDialog = {};
            }
        }
        if (inlineInputFuture.valid())
            inlineInputFuture.wait();
    }

    Result SoftwareKeyboardApplet::StartInline() {
        std::scoped_lock lock{normalInputDataMutex};
        if (normalInputData.size() < 2)
            return {};

        normalInputData.pop(); // CommonArguments
        auto initData{normalInputData.front()->GetSpan()};
        normalInputData.pop();
        if (initData.size() < 8) {
            LOGW("Invalid inline SWKBD initialize data size: 0x{:X}", initData.size());
            return {};
        }

        inlineStarted = true;
        inlineState = InlineState::Uninitialized;
        inlineUseUtf8 = false;
        inlineUseChangedStringV2 = false;
        inlineUseMovedCursorV2 = false;
        inlineCursorPosition = 0;
        currentText.clear();
        return {};
    }

    Result SoftwareKeyboardApplet::Start() {
        if (mode != service::applet::LibraryAppletMode::AllForeground)
            return StartInline();

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
             config.commonConfig.isUseTextCheck);

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

    void SoftwareKeyboardApplet::SendInlineReply(InlineReply reply, span<const u8> payload) {
        std::vector<u8> output(8 + payload.size());
        WriteValue<u32>(output, 0, static_cast<u32>(inlineState));
        WriteValue<u32>(output, 4, static_cast<u32>(reply));
        if (!payload.empty())
            std::memcpy(output.data() + 8, payload.data(), payload.size());
        PushInteractiveDataAndSignal(std::make_shared<service::am::VectorIStorage>(state, manager, std::move(output)));
    }

    void SoftwareKeyboardApplet::SendInlineTextReply(InlineReply reply, std::u16string_view text, u32 cursor) {
        const bool utf8{reply == InlineReply::ChangedStringUtf8 || reply == InlineReply::MovedCursorUtf8 ||
                        reply == InlineReply::DecidedEnterUtf8 || reply == InlineReply::ChangedStringUtf8V2 ||
                        reply == InlineReply::MovedCursorUtf8V2};
        const bool changed{reply == InlineReply::ChangedString || reply == InlineReply::ChangedStringUtf8 ||
                           reply == InlineReply::ChangedStringV2 || reply == InlineReply::ChangedStringUtf8V2};
        const bool moved{reply == InlineReply::MovedCursor || reply == InlineReply::MovedCursorUtf8 ||
                         reply == InlineReply::MovedCursorV2 || reply == InlineReply::MovedCursorUtf8V2};
        const bool v2{reply == InlineReply::ChangedStringV2 || reply == InlineReply::ChangedStringUtf8V2 ||
                      reply == InlineReply::MovedCursorV2 || reply == InlineReply::MovedCursorUtf8V2};
        const size_t textBytes{utf8 ? InlineUtf8TextBytes : InlineUtf16TextBytes};
        const size_t argBytes = changed ? 16 : moved ? 8 : 4;
        std::vector<u8> payload(textBytes + argBytes + (v2 ? 1 : 0));
        WriteStringToSpan(span<u8>{payload.data(), textBytes}, text, utf8);

        const u32 length{static_cast<u32>(text.size())};
        cursor = std::min(cursor, length);
        WriteValue<u32>(payload, textBytes, length);
        if (changed) {
            WriteValue<i32>(payload, textBytes + 4, -1);
            WriteValue<i32>(payload, textBytes + 8, -1);
            WriteValue<u32>(payload, textBytes + 12, cursor);
        } else if (moved) {
            WriteValue<u32>(payload, textBytes + 4, cursor);
        }
        SendInlineReply(reply, payload);
    }

    void SoftwareKeyboardApplet::ConfigureInlineKeyboard(span<u8> calc, bool extended) {
        const size_t appearOffset{0x20};
        const size_t inputOffset = extended ? 0x90 : 0x68;
        const size_t utf8Offset = extended ? 0x484 : 0x45C;

        config = KeyboardConfigVB{};
        auto &common{config.commonConfig};
        common.keyboardMode = static_cast<KeyboardMode>(ReadValue<u32>(calc, appearOffset));
        std::memcpy(common.okText.data(), calc.data() + appearOffset + 4, sizeof(common.okText));
        common.leftOptionalSymbolKey = ReadValue<char16_t>(calc, appearOffset + 0x16);
        common.rightOptionalSymbolKey = ReadValue<char16_t>(calc, appearOffset + 0x18);
        common.isPredictionEnabled = ReadValue<u8>(calc, appearOffset + 0x1A) != 0;
        common.invalidCharFlags.raw = ReadValue<u32>(calc, appearOffset + 0x1C);

        const i32 maxLength{ReadValue<i32>(calc, appearOffset + 0x20)};
        const i32 minLength{ReadValue<i32>(calc, appearOffset + 0x24)};
        common.textMaxLength = maxLength > 0 ? std::min<u32>(static_cast<u32>(maxLength), 0x1F4) : 0x1F4;
        common.textMinLength = minLength > 0 ? std::min<u32>(static_cast<u32>(minLength), common.textMaxLength) : 0;
        common.isUseNewLine = ReadValue<u8>(calc, appearOffset + 0x28) != 0;
        common.isUseUtf8 = ReadValue<u8>(calc, utf8Offset) != 0;
        common.passwordMode = PasswordMode::Show;
        common.inputFormMode = (common.isUseNewLine || common.textMaxLength > MaxOneLineChars) ? InputFormMode::MultiLine : InputFormMode::OneLine;
        common.initialCursorPos = inlineCursorPosition == 0 ? InitialCursorPos::First : InitialCursorPos::Last;
        config.isCancelButtonDisabled = extended && ReadValue<u8>(calc, appearOffset + 0x1B) != 0;

        inlineUseUtf8 = common.isUseUtf8;
        if (currentText.empty())
            currentText = ReadInlineString(calc, inputOffset, 0x3F4);
    }

    void SoftwareKeyboardApplet::ShowInlineKeyboard() {
        if (dialog)
            return;

        inlineState = InlineState::Appearing;
        dialog = state.jvm->ShowKeyboard(*reinterpret_cast<JvmManager::KeyboardConfig *>(&config), currentText);
        if (!dialog) {
            inlineState = InlineState::Initialized;
            return;
        }

        pendingInlineWaitDialog = state.jvm->CloneKeyboardHandle(dialog);
        inlineState = InlineState::Shown;
        const InlineReply changedReply{inlineUseUtf8
                                           ? (inlineUseChangedStringV2 ? InlineReply::ChangedStringUtf8V2 : InlineReply::ChangedStringUtf8)
                                           : (inlineUseChangedStringV2 ? InlineReply::ChangedStringV2 : InlineReply::ChangedString)};
        SendInlineTextReply(changedReply, currentText, static_cast<u32>(std::max(inlineCursorPosition, 0)));
    }

    void SoftwareKeyboardApplet::HideInlineKeyboard() {
        if (dialog) {
            state.jvm->CloseKeyboard(dialog);
            dialog = {};
        }
        if (inlineStarted)
            inlineState = InlineState::Initialized;
    }

    void SoftwareKeyboardApplet::ProcessInlineCalc(span<u8> calc) {
        const bool standard{calc.size() == 0x4A0};
        const bool extended{calc.size() == 0x4C8 || calc.size() == 0x4E8};
        if (!standard && !extended) {
            LOGW("Invalid inline SWKBD Calc size: 0x{:X}", calc.size());
            return;
        }

        const u64 flags{ReadValue<u64>(calc, 0x8)};
        const size_t inputOffset = extended ? 0x90 : 0x68;
        const size_t utf8Offset = extended ? 0x484 : 0x45C;

        if (flags & 0x1) {
            const std::array<u8, 1> initialized{1};
            SendInlineReply(InlineReply::FinishedInitialize, initialized);
            inlineState = InlineState::Initialized;
        }

        if (flags & 0x10)
            inlineCursorPosition = std::max(ReadValue<i32>(calc, 0x1C), 0);
        if (flags & 0x8) {
            currentText = ReadInlineString(calc, inputOffset, 0x3F4);
            inlineCursorPosition = static_cast<i32>(currentText.size());
        }
        if (flags & 0x20)
            inlineUseUtf8 = ReadValue<u8>(calc, utf8Offset) != 0;

        ConfigureInlineKeyboard(calc, extended);

        if (flags & 0x40)
            SendInlineReply(InlineReply::UnsetCustomizeDic);
        if (flags & 0x400)
            SendInlineReply(InlineReply::ReleasedUserWordInfo);

        if (flags & 0x80) {
            inlineState = InlineState::Disappearing;
            HideInlineKeyboard();
        } else if (flags & (0x4 | 0x8 | 0x10000)) {
            ShowInlineKeyboard();
        }

        SendInlineReply(InlineReply::Default);
    }

    void SoftwareKeyboardApplet::ProcessInlineRequest(span<u8> data) {
        if (data.size() < sizeof(u32)) {
            LOGW("Inline SWKBD request is too small: 0x{:X}", data.size());
            return;
        }

        const auto request{static_cast<InlineRequest>(ReadValue<u32>(data, 0))};
        auto payload{data.subspan(sizeof(u32))};
        switch (request) {
            case InlineRequest::Finalize:
                HideInlineKeyboard();
                inlineStarted = false;
                inlineState = InlineState::Uninitialized;
                onAppletStateChanged->Signal();
                break;
            case InlineRequest::SetUserWordInfo:
                SendInlineReply(InlineReply::ReleasedUserWordInfo);
                break;
            case InlineRequest::SetCustomizeDic:
            case InlineRequest::SetCustomizedDictionaries:
                break;
            case InlineRequest::Calc:
                ProcessInlineCalc(payload);
                break;
            case InlineRequest::UnsetCustomizedDictionaries:
                SendInlineReply(InlineReply::UnsetCustomizedDictionaries);
                break;
            case InlineRequest::SetChangedStringV2Flag:
                if (!payload.empty())
                    inlineUseChangedStringV2 = payload.front() != 0;
                break;
            case InlineRequest::SetMovedCursorV2Flag:
                if (!payload.empty())
                    inlineUseMovedCursorV2 = payload.front() != 0;
                break;
            default:
                LOGW("Unknown inline SWKBD request: 0x{:X}", static_cast<u32>(request));
                break;
        }
    }

    void SoftwareKeyboardApplet::WaitForInlineKeyboardInput(JvmManager::KeyboardHandle workerDialog) {
        while (workerDialog) {
            auto update{state.jvm->WaitForInlineKeyboardUpdate(workerDialog)};
            std::scoped_lock lock{inlineMutex};
            if (!inlineStarted)
                break;

            switch (update.type) {
                case JvmManager::KeyboardUpdate::Type::Changed: {
                    currentText = std::move(update.text);
                    inlineCursorPosition = std::clamp<i32>(update.cursor, 0, static_cast<i32>(currentText.size()));
                    const InlineReply reply{inlineUseUtf8
                                                ? (inlineUseChangedStringV2 ? InlineReply::ChangedStringUtf8V2 : InlineReply::ChangedStringUtf8)
                                                : (inlineUseChangedStringV2 ? InlineReply::ChangedStringV2 : InlineReply::ChangedString)};
                    SendInlineTextReply(reply, currentText, static_cast<u32>(inlineCursorPosition));
                    break;
                }
                case JvmManager::KeyboardUpdate::Type::Enter:
                    currentText = std::move(update.text);
                    inlineCursorPosition = std::clamp<i32>(update.cursor, 0, static_cast<i32>(currentText.size()));
                    currentResult = CloseResult::Enter;
                    SendInlineTextReply(inlineUseUtf8 ? InlineReply::DecidedEnterUtf8 : InlineReply::DecidedEnter, currentText);
                    inlineState = InlineState::Disappearing;
                    HideInlineKeyboard();
                    workerDialog = {};
                    break;
                case JvmManager::KeyboardUpdate::Type::Cancel:
                    currentResult = CloseResult::Cancel;
                    SendInlineReply(InlineReply::DecidedCancel);
                    inlineState = InlineState::Disappearing;
                    HideInlineKeyboard();
                    workerDialog = {};
                    break;
                case JvmManager::KeyboardUpdate::Type::Closed:
                    if (dialog) {
                        state.jvm->ReleaseKeyboardHandle(dialog);
                        dialog = {};
                    }
                    inlineState = InlineState::Initialized;
                    workerDialog = {};
                    break;
            }
        }

        state.jvm->ReleaseKeyboardHandle(workerDialog);
    }

    bool SoftwareKeyboardApplet::GetIndirectLayerImage(span<u8> image) {
        std::scoped_lock lock{inlineMutex};
        if (!inlineStarted || mode != service::applet::LibraryAppletMode::PartialForegroundWithIndirectDisplay)
            return false;

        // The Android frontend is composited by the host. The guest indirect surface therefore
        // remains a valid transparent RGBA8 layer while preserving the VI image contract.
        std::fill(image.begin(), image.end(), 0);
        return true;
    }

    void SoftwareKeyboardApplet::PushNormalDataToApplet(std::shared_ptr<service::am::IStorage> data) {
        PushNormalInput(std::move(data));
    }

    void SoftwareKeyboardApplet::PushInteractiveDataToApplet(std::shared_ptr<service::am::IStorage> data) {
        if (mode != service::applet::LibraryAppletMode::AllForeground) {
            JvmManager::KeyboardHandle waitDialog{};
            {
                std::scoped_lock lock{inlineMutex};
                ProcessInlineRequest(data->GetSpan());
                waitDialog = std::exchange(pendingInlineWaitDialog, {});
            }

            if (waitDialog) {
                if (inlineInputFuture.valid())
                    inlineInputFuture.wait();
                inlineInputFuture = std::async(std::launch::async, [this, waitDialog] {
                    WaitForInlineKeyboardInput(waitDialog);
                });
            }
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
                        validationPending = false;
                        SendResult();
                    } else {
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
