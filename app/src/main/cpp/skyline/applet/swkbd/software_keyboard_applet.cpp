// SPDX-License-Identifier: MPL-2.0
// Copyright © 2022 Skyline Team and Contributors (https://github.com/skyline-emu/)
// Copyright © 2019-2022 Ryujinx Team and Contributors

#include <algorithm>
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
        template<typename T>
        void WriteValue(std::vector<u8> &data, size_t offset, const T &value) {
            if (offset + sizeof(T) <= data.size())
                std::memcpy(data.data() + offset, &value, sizeof(T));
        }

        template<size_t Size>
        std::u16string ReadFixedString(const std::array<char16_t, Size> &chars) {
            size_t length{};
            while (length < chars.size() && chars[length])
                ++length;
            return {chars.data(), length};
        }

        inline_protocol::Reply SelectChangedReply(bool utf8, bool v2) {
            using inline_protocol::Reply;
            if (utf8)
                return v2 ? Reply::ChangedStringUtf8V2 : Reply::ChangedStringUtf8;
            return v2 ? Reply::ChangedStringV2 : Reply::ChangedString;
        }

        inline_protocol::Reply SelectMovedReply(bool utf8, bool v2) {
            using inline_protocol::Reply;
            if (utf8)
                return v2 ? Reply::MovedCursorUtf8V2 : Reply::MovedCursorUtf8;
            return v2 ? Reply::MovedCursorV2 : Reply::MovedCursor;
        }
    }

    static void WriteStringToSpan(span<u8> chars, std::u16string_view text, bool useUtf8Storage) {
        if (useUtf8Storage) {
            auto u8chars{chars.cast<char8_t>()};
            Utf8Utf16Converter::state_type convertState{};
            const char16_t *fromNext{};
            char8_t *toNext{};
            Utf8Utf16Converter().out(convertState, text.data(), text.end(), fromNext,
                                      u8chars.data(), u8chars.data() + u8chars.size(), toNext);
            if (toNext < u8chars.data() + u8chars.size())
                *toNext = u8'\0';
        } else {
            std::memcpy(chars.data(), text.data(), std::min(text.size() * sizeof(char16_t), chars.size()));
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
        if (mode != service::applet::LibraryAppletMode::AllForeground) {
            state.jvm->ClearInlineKeyboardCallback();
            state.jvm->CloseInlineKeyboard();
        }
    }

    Result SoftwareKeyboardApplet::StartInline() {
        {
            std::scoped_lock lock{normalInputDataMutex};
            if (normalInputData.size() < 2) {
                LOGW("Inline SWKBD started without CommonArguments and InitializeArg");
                return result::NotAvailable;
            }

            const auto commonArgs{normalInputData.front()->GetSpan().as<service::applet::CommonArguments>()};
            normalInputData.pop();

            const auto initializeData{normalInputData.front()->GetSpan()};
            normalInputData.pop();
            if (initializeData.size() != sizeof(inline_protocol::InitializeArg)) {
                LOGW("Invalid inline SWKBD InitializeArg size: 0x{:X}", initializeData.size());
                return result::NotAvailable;
            }

            std::memcpy(&inlineInitializeArg, initializeData.data(), sizeof(inlineInitializeArg));

            const auto expectedMode{inlineInitializeArg.libraryAppletModeFlag
                                        ? service::applet::LibraryAppletMode::PartialForeground
                                        : service::applet::LibraryAppletMode::PartialForegroundWithIndirectDisplay};
            if (mode != expectedMode) {
                LOGW("Inline SWKBD mode mismatch: init flag {}, applet mode 0x{:X}",
                     inlineInitializeArg.libraryAppletModeFlag, static_cast<u32>(mode));
                return result::NotAvailable;
            }

            inlineStarted = true;
            inlineState = inline_protocol::State::NotInitialized;
            inlineUseUtf8 = false;
            inlineUseChangedStringV2 = false;
            inlineUseMovedCursorV2 = false;
            inlineEnableBackspace = true;
            inlineCursorPosition = 0;
            currentText.clear();

            LOGD("Starting inline SWKBD apiVersion=0x{:X}, mode=0x{:X}", commonArgs.apiVersion, static_cast<u32>(mode));
        }

        std::weak_ptr<SoftwareKeyboardApplet> weakSelf{weak_from_this()};
        state.jvm->SetInlineKeyboardCallback([weakSelf](JvmManager::InlineKeyboardUpdate update) mutable {
            if (auto self{weakSelf.lock()})
                self->HandleInlineFrontendUpdate(std::move(update));
        });
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

    void SoftwareKeyboardApplet::SendInlineReply(inline_protocol::Reply reply, span<const u8> payload) {
        std::vector<u8> output(sizeof(u32) * 2 + payload.size());
        const u32 stateValue{static_cast<u32>(inlineState)};
        const u32 replyValue{static_cast<u32>(reply)};
        WriteValue(output, 0, stateValue);
        WriteValue(output, sizeof(u32), replyValue);
        if (!payload.empty())
            std::memcpy(output.data() + sizeof(u32) * 2, payload.data(), payload.size());
        PushInteractiveDataAndSignal(std::make_shared<service::am::VectorIStorage>(state, manager, std::move(output)));
    }

    void SoftwareKeyboardApplet::SendInlineTextReply(inline_protocol::Reply reply, std::u16string_view text, i32 cursor) {
        using inline_protocol::Reply;

        const bool utf8{reply == Reply::ChangedStringUtf8 || reply == Reply::MovedCursorUtf8 ||
                        reply == Reply::DecidedEnterUtf8 || reply == Reply::ChangedStringUtf8V2 ||
                        reply == Reply::MovedCursorUtf8V2};
        const bool changed{reply == Reply::ChangedString || reply == Reply::ChangedStringUtf8 ||
                           reply == Reply::ChangedStringV2 || reply == Reply::ChangedStringUtf8V2};
        const bool moved{reply == Reply::MovedCursor || reply == Reply::MovedCursorUtf8 ||
                         reply == Reply::MovedCursorV2 || reply == Reply::MovedCursorUtf8V2};
        const bool decided{reply == Reply::DecidedEnter || reply == Reply::DecidedEnterUtf8};
        const bool v2{reply == Reply::ChangedStringV2 || reply == Reply::ChangedStringUtf8V2 ||
                      reply == Reply::MovedCursorV2 || reply == Reply::MovedCursorUtf8V2};
        if (!changed && !moved && !decided)
            return;

        const size_t textBytes{utf8 ? inline_protocol::Utf8TextBytes : inline_protocol::Utf16TextBytes};
        const size_t argBytes{changed ? sizeof(inline_protocol::ChangedStringArg)
                                      : moved ? sizeof(inline_protocol::MovedCursorArg)
                                              : sizeof(inline_protocol::DecidedEnterArg)};
        std::vector<u8> payload(textBytes + argBytes + (v2 ? 1 : 0));
        WriteStringToSpan(span<u8>{payload.data(), textBytes}, text, utf8);

        const i32 clampedCursor{std::clamp<i32>(cursor, 0, static_cast<i32>(text.size()))};
        if (changed) {
            const inline_protocol::ChangedStringArg arg{
                .textLength = static_cast<u32>(text.size()),
                .dictionaryStartCursorPosition = -1,
                .dictionaryEndCursorPosition = -1,
                .cursorPosition = clampedCursor,
            };
            WriteValue(payload, textBytes, arg);
        } else if (moved) {
            const inline_protocol::MovedCursorArg arg{
                .textLength = static_cast<u32>(text.size()),
                .cursorPosition = clampedCursor,
            };
            WriteValue(payload, textBytes, arg);
        } else {
            const inline_protocol::DecidedEnterArg arg{.textLength = static_cast<u32>(text.size())};
            WriteValue(payload, textBytes, arg);
        }

        SendInlineReply(reply, span<const u8>{payload.data(), payload.size()});
    }

    void SoftwareKeyboardApplet::ChangeInlineState(inline_protocol::State newState) {
        inlineState = newState;
        SendInlineReply(inline_protocol::Reply::Default);
    }

    void SoftwareKeyboardApplet::ConfigureInlineKeyboardOld() {
        const auto &appear{inlineCalcOld.appearArg};
        config = KeyboardConfigVB{};
        auto &common{config.commonConfig};
        common.keyboardMode = static_cast<KeyboardMode>(appear.type);
        std::copy(appear.okText.begin(), appear.okText.end(), common.okText.begin());
        common.leftOptionalSymbolKey = appear.leftOptionalSymbolKey;
        common.rightOptionalSymbolKey = appear.rightOptionalSymbolKey;
        common.isPredictionEnabled = appear.usePrediction;
        common.invalidCharFlags.raw = appear.keyDisableFlags;
        common.textMaxLength = appear.maxTextLength > 0 && appear.maxTextLength <= 500 ? appear.maxTextLength : 500;
        common.textMinLength = appear.minTextLength <= common.textMaxLength ? appear.minTextLength : 0;
        common.passwordMode = PasswordMode::Show;
        common.inputFormMode = common.textMaxLength <= MaxOneLineChars ? InputFormMode::OneLine : InputFormMode::MultiLine;
        common.isUseNewLine = appear.enableReturnButton;
        common.isUseUtf8 = inlineUseUtf8;
        common.initialCursorPos = inlineCursorPosition > 0 ? InitialCursorPos::Last : InitialCursorPos::First;
        config.isCancelButtonDisabled = appear.disableCancelButton;
        inlineEnableBackspace = inlineCalcOld.enableBackspaceButton;
    }

    void SoftwareKeyboardApplet::ConfigureInlineKeyboardNew() {
        const auto &appear{inlineCalcNew.appearArg};
        config = KeyboardConfigVB{};
        auto &common{config.commonConfig};
        common.keyboardMode = static_cast<KeyboardMode>(appear.type);
        std::copy(appear.okText.begin(), appear.okText.end(), common.okText.begin());
        common.leftOptionalSymbolKey = appear.leftOptionalSymbolKey;
        common.rightOptionalSymbolKey = appear.rightOptionalSymbolKey;
        common.isPredictionEnabled = appear.usePrediction;
        common.invalidCharFlags.raw = appear.keyDisableFlags;
        common.textMaxLength = appear.maxTextLength > 0 && appear.maxTextLength <= 500 ? appear.maxTextLength : 500;
        common.textMinLength = appear.minTextLength <= common.textMaxLength ? appear.minTextLength : 0;
        common.passwordMode = PasswordMode::Show;
        common.inputFormMode = common.textMaxLength <= MaxOneLineChars ? InputFormMode::OneLine : InputFormMode::MultiLine;
        common.isUseNewLine = appear.enableReturnButton;
        common.isUseUtf8 = inlineUseUtf8;
        common.initialCursorPos = inlineCursorPosition > 0 ? InitialCursorPos::Last : InitialCursorPos::First;
        config.isCancelButtonDisabled = appear.disableCancelButton;
        inlineEnableBackspace = inlineCalcNew.enableBackspaceButton;
    }

    void SoftwareKeyboardApplet::ShowInlineKeyboard() {
        if (inlineState != inline_protocol::State::InitializedIsHidden)
            return;

        ChangeInlineState(inline_protocol::State::InitializedIsAppearing);
        state.jvm->ShowInlineKeyboard(*reinterpret_cast<JvmManager::KeyboardConfig *>(&config), currentText, inlineCursorPosition, inlineEnableBackspace);
        ChangeInlineState(inline_protocol::State::InitializedIsShown);
    }

    void SoftwareKeyboardApplet::HideInlineKeyboard() {
        if (inlineState != inline_protocol::State::InitializedIsShown)
            return;

        ChangeInlineState(inline_protocol::State::InitializedIsDisappearing);
        state.jvm->HideInlineKeyboard();
        ChangeInlineState(inline_protocol::State::InitializedIsHidden);
    }

    void SoftwareKeyboardApplet::CloseInlineKeyboard() {
        state.jvm->ClearInlineKeyboardCallback();
        state.jvm->CloseInlineKeyboard();
    }

    void SoftwareKeyboardApplet::HandleInlineFrontendUpdate(JvmManager::InlineKeyboardUpdate update) {
        std::scoped_lock lock{inlineMutex};
        if (!inlineStarted || inlineState != inline_protocol::State::InitializedIsShown)
            return;

        currentText = std::move(update.text);
        inlineCursorPosition = std::clamp<i32>(update.cursor, 0, static_cast<i32>(currentText.size()));

        switch (update.kind) {
            case JvmManager::InlineKeyboardUpdate::Kind::ChangedString:
                SendInlineTextReply(SelectChangedReply(inlineUseUtf8, inlineUseChangedStringV2), currentText, inlineCursorPosition);
                break;
            case JvmManager::InlineKeyboardUpdate::Kind::MovedCursor:
                SendInlineTextReply(SelectMovedReply(inlineUseUtf8, inlineUseMovedCursorV2), currentText, inlineCursorPosition);
                break;
            case JvmManager::InlineKeyboardUpdate::Kind::Enter:
                SendInlineTextReply(inlineUseUtf8 ? inline_protocol::Reply::DecidedEnterUtf8 : inline_protocol::Reply::DecidedEnter,
                                    currentText, inlineCursorPosition);
                break;
            case JvmManager::InlineKeyboardUpdate::Kind::Cancel:
                SendInlineReply(inline_protocol::Reply::DecidedCancel);
                break;
        }
    }

    void SoftwareKeyboardApplet::ProcessInlineCalcOld() {
        const auto flags{inlineCalcCommon.flags};

        if (flags & inline_protocol::SetInputText)
            currentText = ReadFixedString(inlineCalcOld.inputText);
        if (flags & inline_protocol::SetCursorPosition)
            inlineCursorPosition = inlineCalcOld.cursorPosition;
        if (flags & inline_protocol::SetUtf8Mode)
            inlineUseUtf8 = inlineCalcOld.utf8Mode;
        inlineCursorPosition = std::clamp<i32>(inlineCursorPosition, 0, static_cast<i32>(currentText.size()));

        if (inlineState <= inline_protocol::State::InitializedIsHidden && (flags & inline_protocol::UnsetCustomizeDic))
            SendInlineReply(inline_protocol::Reply::UnsetCustomizeDic);
        if (inlineState <= inline_protocol::State::InitializedIsHidden && (flags & inline_protocol::UnsetUserWordInfo))
            SendInlineReply(inline_protocol::Reply::ReleasedUserWordInfo);

        if (inlineState == inline_protocol::State::NotInitialized && (flags & inline_protocol::SetInitializeArg)) {
            ConfigureInlineKeyboardOld();
            ChangeInlineState(inline_protocol::State::InitializedIsHidden);
            SendInlineReply(inline_protocol::Reply::FinishedInitialize, span<const u8>{reinterpret_cast<const u8 *>("\0"), 1});
        } else if (flags & (inline_protocol::SetInputText | inline_protocol::SetCursorPosition)) {
            state.jvm->UpdateInlineKeyboard(currentText, inlineCursorPosition);
            SendInlineTextReply(SelectChangedReply(inlineUseUtf8, inlineUseChangedStringV2), currentText, inlineCursorPosition);
        }

        if (inlineState == inline_protocol::State::InitializedIsHidden && (flags & inline_protocol::Appear))
            ShowInlineKeyboard();
        else if (inlineState == inline_protocol::State::InitializedIsShown && (flags & inline_protocol::Disappear))
            HideInlineKeyboard();
    }

    void SoftwareKeyboardApplet::ProcessInlineCalcNew() {
        const auto flags{inlineCalcCommon.flags};

        if (flags & inline_protocol::SetInputText)
            currentText = ReadFixedString(inlineCalcNew.inputText);
        if (flags & inline_protocol::SetCursorPosition)
            inlineCursorPosition = inlineCalcNew.cursorPosition;
        if (flags & inline_protocol::SetUtf8Mode)
            inlineUseUtf8 = inlineCalcNew.utf8Mode;
        inlineCursorPosition = std::clamp<i32>(inlineCursorPosition, 0, static_cast<i32>(currentText.size()));

        if (inlineState <= inline_protocol::State::InitializedIsHidden && (flags & inline_protocol::UnsetCustomizeDic))
            SendInlineReply(inline_protocol::Reply::UnsetCustomizeDic);
        if (inlineState <= inline_protocol::State::InitializedIsHidden && (flags & inline_protocol::UnsetUserWordInfo))
            SendInlineReply(inline_protocol::Reply::ReleasedUserWordInfo);

        if (inlineState == inline_protocol::State::NotInitialized && (flags & inline_protocol::SetInitializeArg)) {
            ConfigureInlineKeyboardNew();
            ChangeInlineState(inline_protocol::State::InitializedIsHidden);
            SendInlineReply(inline_protocol::Reply::FinishedInitialize, span<const u8>{reinterpret_cast<const u8 *>("\0"), 1});
        } else if (flags & (inline_protocol::SetInputText | inline_protocol::SetCursorPosition)) {
            state.jvm->UpdateInlineKeyboard(currentText, inlineCursorPosition);
            SendInlineTextReply(SelectChangedReply(inlineUseUtf8, inlineUseChangedStringV2), currentText, inlineCursorPosition);
        }

        if (inlineState == inline_protocol::State::InitializedIsHidden && (flags & inline_protocol::Appear))
            ShowInlineKeyboard();
        else if (inlineState == inline_protocol::State::InitializedIsShown && (flags & inline_protocol::Disappear))
            HideInlineKeyboard();
    }

    void SoftwareKeyboardApplet::ProcessInlineCalc(span<u8> data) {
        constexpr size_t commandSize{sizeof(u32)};
        if (data.size() < commandSize + sizeof(inline_protocol::CalcArgCommon)) {
            LOGW("Inline SWKBD Calc too small: 0x{:X}", data.size());
            return;
        }

        std::memcpy(&inlineCalcCommon, data.data() + commandSize, sizeof(inlineCalcCommon));
        const size_t expectedSize{commandSize + inlineCalcCommon.calcArgSize};
        if (data.size() != expectedSize) {
            LOGW("Inline SWKBD Calc size mismatch: got 0x{:X}, expected 0x{:X}", data.size(), expectedSize);
            return;
        }

        const auto *body{data.data() + commandSize + sizeof(inline_protocol::CalcArgCommon)};
        switch (inlineCalcCommon.calcArgSize) {
            case sizeof(inline_protocol::CalcArgCommon) + sizeof(inline_protocol::CalcArgOldBody):
                std::memcpy(&inlineCalcOld, body, sizeof(inlineCalcOld));
                ProcessInlineCalcOld();
                break;
            case sizeof(inline_protocol::CalcArgCommon) + sizeof(inline_protocol::CalcArgNewBody):
                std::memcpy(&inlineCalcNew, body, sizeof(inlineCalcNew));
                ProcessInlineCalcNew();
                break;
            default:
                LOGW("Unsupported inline SWKBD CalcArg size: 0x{:X}", inlineCalcCommon.calcArgSize);
                break;
        }
    }

    void SoftwareKeyboardApplet::ProcessInlineRequest(span<u8> data) {
        if (data.size() < sizeof(u32)) {
            LOGW("Inline SWKBD request too small: 0x{:X}", data.size());
            return;
        }

        u32 command{};
        std::memcpy(&command, data.data(), sizeof(command));
        switch (static_cast<inline_protocol::Request>(command)) {
            case inline_protocol::Request::Finalize:
                inlineStarted = false;
                ChangeInlineState(inline_protocol::State::NotInitialized);
                CloseInlineKeyboard();
                onAppletStateChanged->Signal();
                break;
            case inline_protocol::Request::SetUserWordInfo:
                SendInlineReply(inline_protocol::Reply::ReleasedUserWordInfo);
                break;
            case inline_protocol::Request::SetCustomizeDic:
            case inline_protocol::Request::SetCustomizedDictionaries:
                break;
            case inline_protocol::Request::Calc:
                ProcessInlineCalc(data);
                break;
            case inline_protocol::Request::UnsetCustomizedDictionaries:
                SendInlineReply(inline_protocol::Reply::UnsetCustomizedDictionaries);
                break;
            case inline_protocol::Request::SetChangedStringV2Flag:
                if (data.size() == sizeof(u32) + 1)
                    inlineUseChangedStringV2 = data[sizeof(u32)] != 0;
                else
                    LOGW("Invalid SetChangedStringV2Flag size: 0x{:X}", data.size());
                break;
            case inline_protocol::Request::SetMovedCursorV2Flag:
                if (data.size() == sizeof(u32) + 1)
                    inlineUseMovedCursorV2 = data[sizeof(u32)] != 0;
                else
                    LOGW("Invalid SetMovedCursorV2Flag size: 0x{:X}", data.size());
                break;
            default:
                LOGW("Unknown inline SWKBD request: 0x{:X}", command);
                break;
        }
    }

    void SoftwareKeyboardApplet::ProcessInlineStorage(std::shared_ptr<service::am::IStorage> data) {
        if (!data)
            return;
        std::scoped_lock lock{inlineMutex};
        if (!inlineStarted)
            return;
        ProcessInlineRequest(data->GetSpan());
    }

    void SoftwareKeyboardApplet::PushNormalDataToApplet(std::shared_ptr<service::am::IStorage> data) {
        PushNormalInput(data);
    }

    void SoftwareKeyboardApplet::PushInteractiveDataToApplet(std::shared_ptr<service::am::IStorage> data) {
        if (mode != service::applet::LibraryAppletMode::AllForeground) {
            ProcessInlineStorage(std::move(data));
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
