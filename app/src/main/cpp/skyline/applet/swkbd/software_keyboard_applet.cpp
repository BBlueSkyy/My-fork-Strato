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
        template<typename T>
        void WriteValue(span<u8> data, size_t offset, const T &value) {
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

        bool IsInlineRequest(u32 value) {
            using inline_protocol::Request;
            switch (static_cast<Request>(value)) {
                case Request::Finalize:
                case Request::SetUserWordInfo:
                case Request::SetCustomizeDic:
                case Request::Calc:
                case Request::SetCustomizedDictionaries:
                case Request::UnsetCustomizedDictionaries:
                case Request::SetChangedStringV2Flag:
                case Request::SetMovedCursorV2Flag:
                    return true;
                default:
                    return false;
            }
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
            if (inlineFrontend) {
                state.jvm->CloseInlineKeyboard(inlineFrontend);
                inlineFrontend = {};
            }
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
        if (normalInputData.size() < 2) {
            LOGW("Inline SWKBD started without CommonArguments and InitializeArg");
            return {};
        }

        const auto commonArgs{normalInputData.front()->GetSpan().as<service::applet::CommonArguments>()};
        normalInputData.pop();

        const auto initializeData{normalInputData.front()->GetSpan()};
        normalInputData.pop();
        if (initializeData.size() != sizeof(inline_protocol::InitializeArg)) {
            LOGW("Invalid inline SWKBD InitializeArg size: 0x{:X}", initializeData.size());
            return {};
        }
        std::memcpy(&inlineInitializeArg, initializeData.data(), sizeof(inlineInitializeArg));

        const bool expectsIndirect{inlineInitializeArg.mode == 0};
        const bool modeMatches = expectsIndirect
                                     ? mode == service::applet::LibraryAppletMode::PartialForegroundWithIndirectDisplay
                                     : mode == service::applet::LibraryAppletMode::PartialForeground;
        if (!modeMatches) {
            LOGW("Inline SWKBD mode mismatch: init mode {}, applet mode 0x{:X}", inlineInitializeArg.mode, static_cast<u32>(mode));
            return {};
        }

        LOGD("Starting inline SWKBD apiVersion=0x{:X}, mode=0x{:X}", commonArgs.apiVersion, static_cast<u32>(mode));
        inlineStarted = true;
        inlineState = inline_protocol::State::NotInitialized;
        inlineUseUtf8 = false;
        inlineUseChangedStringV2 = false;
        inlineUseMovedCursorV2 = false;
        inlineUsesNewLayout = false;
        inlineCursorPosition = 0;
        inlineDictionaryStorage.reset();
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
            currentText = std::move(result.second);
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

        SendInlineReply(reply, payload);
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
        common.isPredictionEnabled = appear.usePrediction != 0;
        common.invalidCharFlags.raw = appear.keyDisableFlags;
        common.textMaxLength = appear.maxTextLength > 0 && appear.maxTextLength <= 500 ? appear.maxTextLength : 500;
        common.textMinLength = appear.minTextLength <= common.textMaxLength ? appear.minTextLength : 0;
        common.passwordMode = PasswordMode::Show;
        common.inputFormMode = common.textMaxLength <= MaxOneLineChars ? InputFormMode::OneLine : InputFormMode::MultiLine;
        common.isUseNewLine = appear.enableReturnButton != 0;
        common.isUseUtf8 = inlineUseUtf8;
        common.initialCursorPos = inlineCursorPosition > 0 ? InitialCursorPos::Last : InitialCursorPos::First;
        config.isCancelButtonDisabled = appear.disableCancelButton != 0;
    }

    void SoftwareKeyboardApplet::ConfigureInlineKeyboardNew() {
        const auto &appear{inlineCalcNew.appearArg};
        config = KeyboardConfigVB{};
        auto &common{config.commonConfig};
        common.keyboardMode = static_cast<KeyboardMode>(appear.type);
        std::copy(appear.okText.begin(), appear.okText.end(), common.okText.begin());
        common.leftOptionalSymbolKey = appear.leftOptionalSymbolKey;
        common.rightOptionalSymbolKey = appear.rightOptionalSymbolKey;
        common.isPredictionEnabled = appear.usePrediction != 0;
        common.invalidCharFlags.raw = appear.keyDisableFlags;
        common.textMaxLength = appear.maxTextLength > 0 && appear.maxTextLength <= 500 ? appear.maxTextLength : 500;
        common.textMinLength = appear.minTextLength <= common.textMaxLength ? appear.minTextLength : 0;
        common.passwordMode = PasswordMode::Show;
        common.inputFormMode = common.textMaxLength <= MaxOneLineChars ? InputFormMode::OneLine : InputFormMode::MultiLine;
        common.isUseNewLine = appear.enableReturnButton != 0;
        common.isUseUtf8 = inlineUseUtf8;
        common.initialCursorPos = inlineCursorPosition > 0 ? InitialCursorPos::Last : InitialCursorPos::First;
        config.isCancelButtonDisabled = appear.disableCancelButton != 0;
    }

    void SoftwareKeyboardApplet::ShowInlineKeyboard() {
        if (inlineState != inline_protocol::State::InitializedIsHidden || inlineFrontend)
            return;

        ChangeInlineState(inline_protocol::State::InitializedIsAppearing);
        inlineFrontend = state.jvm->ShowInlineKeyboard(*reinterpret_cast<JvmManager::KeyboardConfig *>(&config), currentText);
        if (!inlineFrontend) {
            ChangeInlineState(inline_protocol::State::InitializedIsHidden);
            return;
        }

        pendingInlineWaitDialog = state.jvm->CloneKeyboardHandle(inlineFrontend);
        ChangeInlineState(inline_protocol::State::InitializedIsShown);
    }

    void SoftwareKeyboardApplet::HideInlineKeyboard() {
        if (inlineState != inline_protocol::State::InitializedIsShown)
            return;

        ChangeInlineState(inline_protocol::State::InitializedIsDisappearing);
        if (inlineFrontend) {
            state.jvm->CloseInlineKeyboard(inlineFrontend);
            inlineFrontend = {};
        }
        ChangeInlineState(inline_protocol::State::InitializedIsHidden);
    }

    void SoftwareKeyboardApplet::ProcessInlineCalcOld() {
        const u64 flags{inlineCalcCommon.flags};

        if (flags & inline_protocol::SetInputText)
            currentText = ReadFixedString(inlineCalcOld.inputText);
        if (flags & inline_protocol::SetCursorPosition)
            inlineCursorPosition = std::clamp<i32>(inlineCalcOld.cursorPosition, 0, static_cast<i32>(currentText.size()));
        if (flags & inline_protocol::SetUtf8Mode)
            inlineUseUtf8 = inlineCalcOld.utf8Mode != 0;

        if (inlineState <= inline_protocol::State::InitializedIsHidden && (flags & inline_protocol::UnsetCustomizeDic)) {
            inlineDictionaryStorage.reset();
            SendInlineReply(inline_protocol::Reply::UnsetCustomizeDic);
        }
        if (inlineState <= inline_protocol::State::InitializedIsHidden && (flags & inline_protocol::UnsetUserWordInfo))
            SendInlineReply(inline_protocol::Reply::ReleasedUserWordInfo);

        if (inlineState == inline_protocol::State::NotInitialized && (flags & inline_protocol::SetInitializeArg)) {
            inlineInitializeArg = inlineCalcCommon.initializeArg;
            ConfigureInlineKeyboardOld();
            ChangeInlineState(inline_protocol::State::InitializedIsHidden);
            const std::array<u8, 1> payload{};
            SendInlineReply(inline_protocol::Reply::FinishedInitialize, payload);
        }

        if (!(flags & inline_protocol::SetInitializeArg) && (flags & (inline_protocol::SetInputText | inline_protocol::SetCursorPosition)))
            SendInlineTextReply(SelectChangedReply(inlineUseUtf8, inlineUseChangedStringV2), currentText, inlineCursorPosition);

        if (inlineState == inline_protocol::State::InitializedIsHidden && (flags & inline_protocol::Appear))
            ShowInlineKeyboard();
        else if (inlineState == inline_protocol::State::InitializedIsShown && (flags & inline_protocol::Disappear))
            HideInlineKeyboard();
    }

    void SoftwareKeyboardApplet::ProcessInlineCalcNew() {
        const u64 flags{inlineCalcCommon.flags};

        if (flags & inline_protocol::SetInputText)
            currentText = ReadFixedString(inlineCalcNew.inputText);
        if (flags & inline_protocol::SetCursorPosition)
            inlineCursorPosition = std::clamp<i32>(inlineCalcNew.cursorPosition, 0, static_cast<i32>(currentText.size()));
        if (flags & inline_protocol::SetUtf8Mode)
            inlineUseUtf8 = inlineCalcNew.utf8Mode != 0;

        if (inlineState <= inline_protocol::State::InitializedIsHidden && (flags & inline_protocol::UnsetCustomizeDic)) {
            inlineDictionaryStorage.reset();
            SendInlineReply(inline_protocol::Reply::UnsetCustomizeDic);
        }
        if (inlineState <= inline_protocol::State::InitializedIsHidden && (flags & inline_protocol::UnsetUserWordInfo))
            SendInlineReply(inline_protocol::Reply::ReleasedUserWordInfo);

        if (inlineState == inline_protocol::State::NotInitialized && (flags & inline_protocol::SetInitializeArg)) {
            inlineInitializeArg = inlineCalcCommon.initializeArg;
            ConfigureInlineKeyboardNew();
            ChangeInlineState(inline_protocol::State::InitializedIsHidden);
            const std::array<u8, 1> payload{};
            SendInlineReply(inline_protocol::Reply::FinishedInitialize, payload);
        }

        if (!(flags & inline_protocol::SetInitializeArg) && (flags & (inline_protocol::SetInputText | inline_protocol::SetCursorPosition)))
            SendInlineTextReply(SelectChangedReply(inlineUseUtf8, inlineUseChangedStringV2), currentText, inlineCursorPosition);

        if (inlineState == inline_protocol::State::InitializedIsHidden && (flags & inline_protocol::Appear))
            ShowInlineKeyboard();
        else if (inlineState == inline_protocol::State::InitializedIsShown && (flags & inline_protocol::Disappear))
            HideInlineKeyboard();
    }

    void SoftwareKeyboardApplet::ProcessInlineCalc(span<u8> data) {
        if (data.size() < sizeof(inline_protocol::CalcArgCommon)) {
            LOGW("Inline SWKBD Calc is too small: 0x{:X}", data.size());
            return;
        }

        std::memcpy(&inlineCalcCommon, data.data(), sizeof(inlineCalcCommon));
        if (inlineCalcCommon.calcArgSize != data.size()) {
            LOGW("Inline SWKBD Calc size mismatch: header=0x{:X}, storage=0x{:X}", inlineCalcCommon.calcArgSize, data.size());
            return;
        }

        if (inlineCalcCommon.calcArgSize == sizeof(inline_protocol::CalcArgCommon) + sizeof(inline_protocol::CalcArgOldBody)) {
            std::memcpy(&inlineCalcOld, data.data() + sizeof(inlineCalcCommon), sizeof(inlineCalcOld));
            inlineUsesNewLayout = false;
            ProcessInlineCalcOld();
        } else if (inlineCalcCommon.calcArgSize == sizeof(inline_protocol::CalcArgCommon) + sizeof(inline_protocol::CalcArgNewBody)) {
            std::memcpy(&inlineCalcNew, data.data() + sizeof(inlineCalcCommon), sizeof(inlineCalcNew));
            inlineUsesNewLayout = true;
            ProcessInlineCalcNew();
        } else {
            LOGW("Unsupported inline SWKBD Calc layout: 0x{:X}", inlineCalcCommon.calcArgSize);
        }
    }

    void SoftwareKeyboardApplet::ProcessInlineRequest(span<u8> data) {
        if (data.size() < sizeof(u32))
            return;

        u32 rawRequest{};
        std::memcpy(&rawRequest, data.data(), sizeof(rawRequest));
        const auto request{static_cast<inline_protocol::Request>(rawRequest)};
        auto payload{data.subspan(sizeof(u32))};

        switch (request) {
            case inline_protocol::Request::Finalize:
                if (inlineFrontend) {
                    state.jvm->CloseInlineKeyboard(inlineFrontend);
                    inlineFrontend = {};
                }
                inlineStarted = false;
                inlineDictionaryStorage.reset();
                ChangeInlineState(inline_protocol::State::NotInitialized);
                onAppletStateChanged->Signal();
                break;
            case inline_protocol::Request::SetUserWordInfo:
                SendInlineReply(inline_protocol::Reply::ReleasedUserWordInfo);
                break;
            case inline_protocol::Request::SetCustomizeDic:
                break;
            case inline_protocol::Request::Calc:
                ProcessInlineCalc(payload);
                break;
            case inline_protocol::Request::SetCustomizedDictionaries:
                break;
            case inline_protocol::Request::UnsetCustomizedDictionaries:
                inlineDictionaryStorage.reset();
                SendInlineReply(inline_protocol::Reply::UnsetCustomizedDictionaries);
                break;
            case inline_protocol::Request::SetChangedStringV2Flag:
                if (payload.size() == 1)
                    inlineUseChangedStringV2 = payload.front() != 0;
                break;
            case inline_protocol::Request::SetMovedCursorV2Flag:
                if (payload.size() == 1)
                    inlineUseMovedCursorV2 = payload.front() != 0;
                break;
        }
    }

    void SoftwareKeyboardApplet::ProcessInlineStorage(std::shared_ptr<service::am::IStorage> data) {
        auto dataSpan{data->GetSpan()};
        if (dataSpan.size() < sizeof(u32)) {
            inlineDictionaryStorage = std::move(data);
            return;
        }

        u32 rawRequest{};
        std::memcpy(&rawRequest, dataSpan.data(), sizeof(rawRequest));
        if (!IsInlineRequest(rawRequest)) {
            inlineDictionaryStorage = std::move(data);
            return;
        }

        ProcessInlineRequest(dataSpan);
    }

    void SoftwareKeyboardApplet::WaitForInlineKeyboardInput(JvmManager::KeyboardHandle workerDialog) {
        bool done{};
        while (!done && workerDialog) {
            auto update{state.jvm->WaitForInlineKeyboardUpdate(workerDialog)};
            std::scoped_lock lock{inlineMutex};
            if (!inlineStarted)
                break;

            switch (update.type) {
                case JvmManager::KeyboardUpdate::Type::Changed:
                    currentText = std::move(update.text);
                    inlineCursorPosition = std::clamp<i32>(update.cursor, 0, static_cast<i32>(currentText.size()));
                    SendInlineTextReply(SelectChangedReply(inlineUseUtf8, inlineUseChangedStringV2), currentText, inlineCursorPosition);
                    break;
                case JvmManager::KeyboardUpdate::Type::Enter:
                    currentText = std::move(update.text);
                    inlineCursorPosition = std::clamp<i32>(update.cursor, 0, static_cast<i32>(currentText.size()));
                    currentResult = CloseResult::Enter;
                    SendInlineTextReply(inlineUseUtf8 ? inline_protocol::Reply::DecidedEnterUtf8 : inline_protocol::Reply::DecidedEnter,
                                        currentText, inlineCursorPosition);
                    HideInlineKeyboard();
                    done = true;
                    break;
                case JvmManager::KeyboardUpdate::Type::Cancel:
                    currentResult = CloseResult::Cancel;
                    SendInlineReply(inline_protocol::Reply::DecidedCancel);
                    HideInlineKeyboard();
                    done = true;
                    break;
                case JvmManager::KeyboardUpdate::Type::Closed:
                    done = true;
                    break;
            }
        }

        state.jvm->ReleaseKeyboardHandle(workerDialog);
    }

    void SoftwareKeyboardApplet::PushNormalDataToApplet(std::shared_ptr<service::am::IStorage> data) {
        PushNormalInput(std::move(data));
    }

    void SoftwareKeyboardApplet::PushInteractiveDataToApplet(std::shared_ptr<service::am::IStorage> data) {
        if (mode != service::applet::LibraryAppletMode::AllForeground) {
            JvmManager::KeyboardHandle waitDialog{};
            {
                std::scoped_lock lock{inlineMutex};
                if (!inlineStarted)
                    return;
                ProcessInlineStorage(std::move(data));
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
                        currentText = std::move(result.second);
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
