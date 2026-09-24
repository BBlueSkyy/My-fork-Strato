// SPDX-License-Identifier: MPL-2.0
// Copyright © 2022 Skyline Team and Contributors (https://github.com/skyline-emu/)
// Copyright © 2019-2022 Ryujinx Team and Contributors

#include <services/am/storage/ObjIStorage.h>
#include <services/am/storage/VectorIStorage.h>
#include <utility>
#include <jvm.h>
#include "software_keyboard_applet.h"
#include "software_keyboard_text.h"

namespace skyline::applet::swkbd {
    namespace {
        constexpr size_t InlineReplyHeaderSize{sizeof(u32) * 2};
        constexpr size_t InlineInputTextBytes{0x3F4};
        constexpr size_t InlineUtf16TextBytes{0x3EC};
        constexpr size_t InlineUtf8TextBytes{0x7D4};
        constexpr size_t InlineCalcOldSize{0x4A0};
        constexpr size_t InlineCalcLegacyExtendedSize{0x4C8};
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
            if (offset > data.size() || sizeof(T) > data.size() - offset)
                throw exception("Software keyboard inline request is truncated");
            T value{};
            std::memcpy(&value, data.data() + offset, sizeof(T));
            return value;
        }

        template<typename T>
        void WriteInlineValue(std::vector<u8> &data, size_t offset, T value) {
            if (offset > data.size() || sizeof(T) > data.size() - offset)
                throw exception("Software keyboard inline reply is too small");
            std::memcpy(data.data() + offset, &value, sizeof(T));
        }

        std::u16string ReadInlineString(span<u8> data, size_t offset, size_t size) {
            if (offset > data.size() || size > data.size() - offset)
                throw exception("Software keyboard inline string is truncated");
            const auto text{ReadNullTerminatedText(span<const u8>{data.data() + offset, size}, TextEncoding::Utf16)};
            if (!text)
                throw exception("Software keyboard inline string is invalid");
            return *text;
        }

        template<typename T>
        T ReadStruct(span<u8> data, const char *description) {
            if (data.size() < sizeof(T))
                throw exception(description);
            T value{};
            std::memcpy(&value, data.data(), sizeof(T));
            return value;
        }

        bool CanSerializeText(std::u16string_view text, TextEncoding encoding) {
            std::array<u8, SwkbdTextBytes> output{};
            const auto result{WriteText(output, text, encoding)};
            return result.valid && !result.truncated;
        }
    }

    SoftwareKeyboardApplet::ValidationRequest::ValidationRequest(std::u16string_view text, bool useUtf8Storage) : size{} {
        const auto write{WriteText(chars, text, useUtf8Storage ? TextEncoding::Utf8 : TextEncoding::Utf16)};
        const size_t terminatorBytes{useUtf8Storage ? sizeof(char8_t) : sizeof(char16_t)};
        size = std::min(chars.size(), write.bytesWritten + terminatorBytes);
    }

    SoftwareKeyboardApplet::OutputResult::OutputResult(CloseResult closeResult, std::u16string_view text,
                                                       bool useUtf8Storage) : closeResult{closeResult} {
        WriteText(chars, text, useUtf8Storage ? TextEncoding::Utf8 : TextEncoding::Utf16);
    }

    FrontendKeyboardConfig SoftwareKeyboardApplet::CopyFrontendConfig() const {
        FrontendKeyboardConfig frontendConfig{};
        static_assert(sizeof(frontendConfig) == sizeof(config));
        std::memcpy(frontendConfig.data(), &config, sizeof(config));
        return frontendConfig;
    }

    SoftwareKeyboardApplet::SoftwareKeyboardApplet(
        const DeviceState &state,
        service::ServiceManager &manager,
        std::shared_ptr<kernel::type::KEvent> onAppletStateChanged,
        std::shared_ptr<kernel::type::KEvent> onNormalDataPushFromApplet,
        std::shared_ptr<kernel::type::KEvent> onInteractiveDataPushFromApplet,
        service::applet::LibraryAppletMode appletMode)
        : IApplet{state, manager, std::move(onAppletStateChanged), std::move(onNormalDataPushFromApplet),
                  std::move(onInteractiveDataPushFromApplet), appletMode}, mode{appletMode} {}

    SoftwareKeyboardApplet::~SoftwareKeyboardApplet() {
        RequestExit();
    }

    Result SoftwareKeyboardApplet::Start() {
        if (mode == service::applet::LibraryAppletMode::PartialForeground ||
            mode == service::applet::LibraryAppletMode::PartialForegroundWithIndirectDisplay)
            return StartInline();
        if (mode != service::applet::LibraryAppletMode::AllForeground)
            throw exception("Invalid LibraryAppletMode for software keyboard");
        return StartNormal();
    }

    Result SoftwareKeyboardApplet::StartNormal() {
        std::shared_ptr<service::am::IStorage> commonStorage;
        std::shared_ptr<service::am::IStorage> configStorage;
        std::shared_ptr<service::am::IStorage> initialStorage;
        {
            std::scoped_lock lock{normalInputDataMutex};
            if (normalInputData.size() < 2)
                throw exception("Software keyboard requires common arguments and configuration");
            commonStorage = normalInputData.front();
            normalInputData.pop();
            configStorage = normalInputData.front();
            normalInputData.pop();
            if (!normalInputData.empty()) {
                initialStorage = normalInputData.front();
                normalInputData.pop();
            }
        }

        const auto commonArgs{ReadStruct<service::applet::CommonArguments>(
            commonStorage->GetSpan(), "Software keyboard common arguments are truncated")};
        const auto configSpan{configStorage->GetSpan()};
        if (commonArgs.apiVersion < 0x30007)
            config = KeyboardConfigVB{ReadStruct<KeyboardConfigV0>(configSpan, "Software keyboard V0 config is truncated")};
        else if (commonArgs.apiVersion < 0x6000B)
            config = KeyboardConfigVB{ReadStruct<KeyboardConfigV7>(configSpan, "Software keyboard V7 config is truncated")};
        else
            config = ReadStruct<KeyboardConfigVB>(configSpan, "Software keyboard VB config is truncated");
        NormalizeNormalConfig(config);

        std::u16string initialText;
        if (config.commonConfig.initialStringLength) {
            if (!initialStorage)
                throw exception("Software keyboard initial text storage is missing");
            const auto value{ReadInitialText(initialStorage->GetSpan(), config.commonConfig.initialStringOffset,
                                             config.commonConfig.initialStringLength)};
            if (!value)
                throw exception("Software keyboard initial text is out of bounds");
            initialText = *value;
        }

        LOGD("SWKBD frontend open: mode={}, initialChars={}, min={}, max={}, textCheck={}",
             static_cast<u32>(config.commonConfig.keyboardMode), initialText.size(),
             config.commonConfig.textMinLength, config.commonConfig.textMaxLength,
             config.commonConfig.isUseTextCheck);

        {
            std::scoped_lock lock{normalMutex};
            normalState = std::make_unique<NormalKeyboardStateMachine>(config.commonConfig.isUseTextCheck);
            normalState->Open(initialText);
            normalCompleted = false;
        }

        auto callbacks{std::dynamic_pointer_cast<SoftwareKeyboardFrontendCallbacks>(shared_from_this())};
        const auto sessionId{state.jvm->CreateSoftwareKeyboardSession(callbacks)};
        {
            std::scoped_lock lock{normalMutex};
            normalSessionId = sessionId;
        }
        if (!state.jvm->ShowSoftwareKeyboard(sessionId, CopyFrontendConfig(), initialText, false)) {
            NormalAction action;
            {
                std::scoped_lock lock{normalMutex};
                action = normalState->Cancel();
            }
            ExecuteNormalAction(std::move(action));
            return {};
        }
        return {};
    }

    void SoftwareKeyboardApplet::ExecuteNormalAction(NormalAction action) {
        switch (action.type) {
            case NormalActionType::None:
                return;
            case NormalActionType::RequestTextCheck:
                PushInteractiveDataAndSignal(std::make_shared<service::am::ObjIStorage<ValidationRequest>>(
                    state, manager, ValidationRequest{action.text, config.commonConfig.isUseUtf8}));
                return;
            case NormalActionType::ShowTextCheck: {
                std::optional<FrontendSessionId> sessionId;
                {
                    std::scoped_lock lock{normalMutex};
                    sessionId = normalSessionId;
                }
                if (sessionId)
                    state.jvm->ShowSoftwareKeyboardTextCheck(*sessionId, static_cast<u32>(action.textCheckResult), action.message);
                return;
            }
            case NormalActionType::ResumeEditing: {
                std::optional<FrontendSessionId> sessionId;
                {
                    std::scoped_lock lock{normalMutex};
                    sessionId = normalSessionId;
                }
                if (sessionId)
                    state.jvm->ResumeSoftwareKeyboard(*sessionId);
                return;
            }
            case NormalActionType::Complete:
                CompleteNormal(action.closeResult, std::move(action.text));
                return;
        }
    }

    void SoftwareKeyboardApplet::CompleteNormal(CloseResult closeResult, std::u16string text) {
        std::optional<FrontendSessionId> sessionId;
        {
            std::scoped_lock lock{normalMutex};
            if (normalCompleted)
                return;
            normalCompleted = true;
            sessionId = std::exchange(normalSessionId, std::nullopt);
            if (normalState)
                normalState->Close();
        }

        if (sessionId)
            state.jvm->CloseSoftwareKeyboardSession(*sessionId);
        if (closeResult == CloseResult::Cancel)
            text.clear();
        LOGD("SWKBD frontend result: {}, chars={}", closeResult == CloseResult::Enter ? "confirmed" : "cancelled", text.size());
        PushNormalDataAndSignal(std::make_shared<service::am::ObjIStorage<OutputResult>>(
            state, manager, OutputResult{closeResult, text, config.commonConfig.isUseUtf8}));
        onAppletStateChanged->Signal();
    }

    Result SoftwareKeyboardApplet::StartInline() {
        LOGI("SWKBD StartInline: entered, mode=0x{:X}", static_cast<u32>(mode));
        std::shared_ptr<service::am::IStorage> commonStorage;
        std::shared_ptr<service::am::IStorage> initializeStorage;
        {
            std::scoped_lock lock{normalInputDataMutex};
            if (normalInputData.size() < 2)
                throw exception("Software keyboard inline mode requires common arguments and InitializeArg");
            commonStorage = normalInputData.front();
            normalInputData.pop();
            initializeStorage = normalInputData.front();
            normalInputData.pop();
        }
        if (commonStorage->GetSpan().size() < sizeof(service::applet::CommonArguments))
            throw exception("Software keyboard common arguments are truncated");
        const auto initializeArgument{initializeStorage->GetSpan()};
        if (initializeArgument.size() != sizeof(u64))
            throw exception("Software keyboard inline InitializeArg has an invalid size");

        const bool partialForeground{ReadInlineValue<u8>(initializeArgument, sizeof(u32)) != 0};
        const auto expectedMode{partialForeground ? service::applet::LibraryAppletMode::PartialForeground
                                                  : service::applet::LibraryAppletMode::PartialForegroundWithIndirectDisplay};
        if (mode != expectedMode)
            LOGW("Inline SWKBD mode mismatch: expected=0x{:X}, actual=0x{:X}", static_cast<u32>(expectedMode), static_cast<u32>(mode));

        auto callbacks{std::dynamic_pointer_cast<SoftwareKeyboardFrontendCallbacks>(shared_from_this())};
        const auto sessionId{state.jvm->CreateSoftwareKeyboardSession(callbacks)};
        LOGI("SWKBD StartInline: frontend session created, sessionId={}", sessionId);
        {
            std::scoped_lock lock{inlineMutex};
            inlineSessionId = sessionId;
            inlineState = InlineState::Uninitialized;
            inlineStarted = true;
        }
        return {};
    }

    void SoftwareKeyboardApplet::ChangeInlineStateLocked(InlineState newState) {
        if (inlineState == newState)
            return;
        inlineState = newState;
        LOGD("Inline SWKBD state -> 0x{:X}", static_cast<u32>(inlineState));
        SendInlineReplyLocked(InlineReply::Default);
    }

    void SoftwareKeyboardApplet::SendInlineReplyLocked(InlineReply reply) {
        const size_t size{InlineReplyHeaderSize + (reply == InlineReply::FinishedInitialize ? 1 : 0)};
        std::vector<u8> response(size);
        WriteInlineValue(response, 0, inlineState);
        WriteInlineValue(response, sizeof(u32), reply);
        PushInteractiveDataAndSignal(std::make_shared<service::am::VectorIStorage>(state, manager, std::move(response)));
    }

    void SoftwareKeyboardApplet::SendInlineTextReplyLocked(InlineReply reply) {
        const bool utf8{reply == InlineReply::ChangedStringUtf8 || reply == InlineReply::ChangedStringUtf8V2 ||
                        reply == InlineReply::DecidedEnterUtf8};
        const bool changed{reply == InlineReply::ChangedString || reply == InlineReply::ChangedStringV2 ||
                           reply == InlineReply::ChangedStringUtf8 || reply == InlineReply::ChangedStringUtf8V2};
        const bool decidedEnter{reply == InlineReply::DecidedEnter || reply == InlineReply::DecidedEnterUtf8};
        const bool version2{reply == InlineReply::ChangedStringV2 || reply == InlineReply::ChangedStringUtf8V2};
        if (!changed && !decidedEnter)
            throw exception("Invalid software keyboard inline text reply");

        const size_t textBytes{utf8 ? InlineUtf8TextBytes : InlineUtf16TextBytes};
        const size_t argumentSize{changed ? sizeof(u32) * 4 : sizeof(u32)};
        std::vector<u8> response(InlineReplyHeaderSize + textBytes + argumentSize + (version2 ? 1 : 0));
        WriteInlineValue(response, 0, inlineState);
        WriteInlineValue(response, sizeof(u32), reply);
        WriteText({response.data() + InlineReplyHeaderSize, textBytes}, inlineText,
                  utf8 ? TextEncoding::Utf8 : TextEncoding::Utf16);

        const size_t argumentOffset{InlineReplyHeaderSize + textBytes};
        WriteInlineValue(response, argumentOffset, static_cast<u32>(inlineText.size()));
        if (changed) {
            WriteInlineValue(response, argumentOffset + sizeof(u32), static_cast<i32>(-1));
            WriteInlineValue(response, argumentOffset + sizeof(u32) * 2, static_cast<i32>(-1));
            WriteInlineValue(response, argumentOffset + sizeof(u32) * 3,
                             std::clamp(inlineCursorPosition, 0, static_cast<i32>(inlineText.size())));
        }
        PushInteractiveDataAndSignal(std::make_shared<service::am::VectorIStorage>(state, manager, std::move(response)));
    }

    void SoftwareKeyboardApplet::ConfigureInlineKeyboardLocked(span<u8> calc, bool extendedLayout) {
        const size_t appearOffset{extendedLayout ? 0x18U : 0x20U};
        config = KeyboardConfigVB{};
        config.commonConfig.keyboardMode = ReadInlineValue<KeyboardMode>(calc, appearOffset);
        if (appearOffset + 0x4 + sizeof(config.commonConfig.okText) > calc.size())
            throw exception("Software keyboard inline appear argument is truncated");
        std::memcpy(config.commonConfig.okText.data(), calc.data() + appearOffset + 0x4, sizeof(config.commonConfig.okText));
        config.commonConfig.leftOptionalSymbolKey = ReadInlineValue<char16_t>(calc, appearOffset + 0x16);
        config.commonConfig.rightOptionalSymbolKey = ReadInlineValue<char16_t>(calc, appearOffset + 0x18);
        config.commonConfig.isPredictionEnabled = ReadInlineValue<u8>(calc, appearOffset + 0x1A) != 0;
        config.isCancelButtonDisabled = ReadInlineValue<u8>(calc, appearOffset + 0x1B) != 0;
        config.commonConfig.invalidCharFlags = ReadInlineValue<InvalidCharFlags>(calc, appearOffset + 0x1C);
        const auto textMaxLength{ReadInlineValue<i32>(calc, appearOffset + 0x20)};
        const auto textMinLength{ReadInlineValue<i32>(calc, appearOffset + 0x24)};
        config.commonConfig.isUseNewLine = ReadInlineValue<u8>(calc, appearOffset + 0x28) != 0;

        NormalizeInlineConfig(config, textMaxLength, textMinLength);
        config.commonConfig.passwordMode = PasswordMode::Show;
        config.commonConfig.initialCursorPos = inlineCursorPosition > 0 ? InitialCursorPos::Last : InitialCursorPos::First;
        config.commonConfig.isUseUtf8 = inlineUseUtf8;
    }

    void SoftwareKeyboardApplet::HideInlineKeyboardLocked() {
        if (inlineState != InlineState::Shown && inlineState != InlineState::Appearing)
            return;
        ChangeInlineStateLocked(InlineState::Disappearing);
        ChangeInlineStateLocked(InlineState::Hidden);
    }

    SoftwareKeyboardApplet::InlineFrontendAction SoftwareKeyboardApplet::ProcessInlineCalcLocked(span<u8> calc) {
        InlineFrontendAction action;
        if (calc.size() < 0x18) {
            LOGW("Software keyboard inline Calc is truncated: 0x{:X}", calc.size());
            return action;
        }
        const auto calcArgSize{ReadInlineValue<u16>(calc, 0x4)};
        if (calcArgSize != calc.size()) {
            LOGW("Software keyboard inline Calc size mismatch: header=0x{:X}, storage=0x{:X}", calcArgSize, calc.size());
            return action;
        }

        bool extendedInputLayout{};
        bool newLayout{};
        switch (calcArgSize) {
            case InlineCalcOldSize:
                break;
            case InlineCalcLegacyExtendedSize:
                extendedInputLayout = true;
                break;
            case InlineCalcNewSize:
                extendedInputLayout = true;
                newLayout = true;
                break;
            default:
                LOGW("Unsupported inline keyboard Calc size: 0x{:X}", calcArgSize);
                return action;
        }

        const u64 flags{ReadInlineValue<u64>(calc, 0x8)};
        LOGI("SWKBD Calc: calcArgSize=0x{:X}, flags=0x{:X}, state=0x{:X}, Initialize={}, Appear={}",
             calcArgSize, flags, static_cast<u32>(inlineState), (flags & InlineFlagInitialize) != 0,
             (flags & InlineFlagAppear) != 0);
        const size_t cursorOffset{newLayout ? 0x8CU : 0x1CU};
        const size_t inputTextOffset{extendedInputLayout ? 0x90U : 0x68U};
        const size_t utf8Offset{extendedInputLayout ? 0x484U : 0x45CU};
        if (flags & InlineFlagSetInputText)
            inlineText = ReadInlineString(calc, inputTextOffset, InlineInputTextBytes);
        if (flags & InlineFlagSetCursorPosition)
            inlineCursorPosition = ReadInlineValue<i32>(calc, cursorOffset);
        if (flags & InlineFlagSetUtf8Mode)
            inlineUseUtf8 = ReadInlineValue<u8>(calc, utf8Offset) != 0;

        if (inlineState <= InlineState::Hidden && (flags & InlineFlagUnsetCustomizeDictionary))
            SendInlineReplyLocked(InlineReply::UnsetCustomizeDictionary);
        if (inlineState <= InlineState::Hidden && (flags & InlineFlagUnsetUserWordInfo))
            SendInlineReplyLocked(InlineReply::ReleasedUserWordInfo);

        if ((flags & InlineFlagInitialize) && inlineState == InlineState::Uninitialized) {
            ConfigureInlineKeyboardLocked(calc, newLayout);
            ChangeInlineStateLocked(InlineState::Hidden);
            SendInlineReplyLocked(InlineReply::FinishedInitialize);
        }
        if ((flags & InlineFlagAppear) && inlineState == InlineState::Hidden) {
            ConfigureInlineKeyboardLocked(calc, newLayout);
            ChangeInlineStateLocked(InlineState::Appearing);
            action.type = InlineFrontendActionType::Show;
            action.config = CopyFrontendConfig();
            action.text = inlineText;
            action.cursor = std::clamp(inlineCursorPosition, 0, static_cast<i32>(inlineText.size()));
            return action;
        }
        if ((flags & InlineFlagDisappear) && inlineState == InlineState::Shown) {
            HideInlineKeyboardLocked();
            action.type = InlineFrontendActionType::Hide;
            return action;
        }
        if (inlineState == InlineState::Shown && (flags & (InlineFlagSetInputText | InlineFlagSetCursorPosition))) {
            action.type = InlineFrontendActionType::Update;
            action.text = inlineText;
            action.cursor = std::clamp(inlineCursorPosition, 0, static_cast<i32>(inlineText.size()));
        }
        return action;
    }

    SoftwareKeyboardApplet::InlineFrontendAction SoftwareKeyboardApplet::ProcessInlineRequestLocked(span<u8> data) {
        InlineFrontendAction action;
        if (data.size() < sizeof(InlineRequest)) {
            LOGW("Software keyboard inline request is truncated");
            return action;
        }
        const auto request{ReadInlineValue<InlineRequest>(data, 0)};
        switch (request) {
            case InlineRequest::Finalize:
                inlineStarted = false;
                ChangeInlineStateLocked(InlineState::Uninitialized);
                action.type = InlineFrontendActionType::Close;
                onAppletStateChanged->Signal();
                break;
            case InlineRequest::SetUserWordInfo:
                SendInlineReplyLocked(InlineReply::ReleasedUserWordInfo);
                break;
            case InlineRequest::SetCustomizeDictionary:
            case InlineRequest::SetCustomizedDictionaries:
                break;
            case InlineRequest::Calc:
                action = ProcessInlineCalcLocked(data.subspan(sizeof(InlineRequest)));
                break;
            case InlineRequest::UnsetCustomizedDictionaries:
                SendInlineReplyLocked(InlineReply::UnsetCustomizedDictionaries);
                break;
            case InlineRequest::SetChangedStringV2:
                if (data.size() >= sizeof(InlineRequest) + sizeof(u8))
                    inlineUseChangedStringV2 = ReadInlineValue<u8>(data, sizeof(InlineRequest)) != 0;
                break;
            case InlineRequest::SetMovedCursorV2:
                if (data.size() >= sizeof(InlineRequest) + sizeof(u8))
                    inlineUseMovedCursorV2 = ReadInlineValue<u8>(data, sizeof(InlineRequest)) != 0;
                break;
            default:
                LOGW("Unknown software keyboard inline request: 0x{:X}", static_cast<u32>(request));
                break;
        }
        return action;
    }

    void SoftwareKeyboardApplet::ExecuteInlineFrontendAction(InlineFrontendAction action) {
        if (action.type == InlineFrontendActionType::None)
            return;
        std::optional<FrontendSessionId> sessionId;
        {
            std::scoped_lock lock{inlineMutex};
            sessionId = inlineSessionId;
        }
        if (!sessionId && action.type != InlineFrontendActionType::Close)
            return;

        switch (action.type) {
            case InlineFrontendActionType::Show: {
                LOGI("SWKBD inline: before ShowSoftwareKeyboard, sessionId={}", *sessionId);
                const bool opened{state.jvm->ShowSoftwareKeyboard(*sessionId, action.config, action.text, true)};
                LOGI("SWKBD inline: after ShowSoftwareKeyboard, sessionId={}, opened={}", *sessionId, opened);
                bool update{};
                {
                    std::scoped_lock lock{inlineMutex};
                    if (inlineSessionId != sessionId || inlineState != InlineState::Appearing)
                        return;
                    if (opened) {
                        ChangeInlineStateLocked(InlineState::Shown);
                        update = true;
                    } else {
                        SendInlineReplyLocked(InlineReply::DecidedCancel);
                        ChangeInlineStateLocked(InlineState::Hidden);
                    }
                }
                if (update)
                    state.jvm->UpdateSoftwareKeyboard(*sessionId, action.text, action.cursor);
                return;
            }
            case InlineFrontendActionType::Hide:
                state.jvm->HideSoftwareKeyboard(*sessionId);
                return;
            case InlineFrontendActionType::Update:
                state.jvm->UpdateSoftwareKeyboard(*sessionId, action.text, action.cursor);
                return;
            case InlineFrontendActionType::Close:
                if (sessionId) {
                    state.jvm->CloseSoftwareKeyboardSession(*sessionId);
                    std::scoped_lock lock{inlineMutex};
                    if (inlineSessionId == sessionId)
                        inlineSessionId.reset();
                }
                return;
            case InlineFrontendActionType::None:
                return;
        }
    }

    void SoftwareKeyboardApplet::HandleInlineFrontendEventLocked(FrontendEvent event, InlineFrontendAction &action) {
        if (!inlineStarted || !inlineSessionId || event.sessionId != *inlineSessionId ||
            (inlineState != InlineState::Shown && inlineState != InlineState::Appearing))
            return;
        if (event.type == FrontendEventType::TextChanged || event.type == FrontendEventType::Submit) {
            if (!CanSerializeText(event.text, inlineUseUtf8 ? TextEncoding::Utf8 : TextEncoding::Utf16)) {
                LOGW("Ignoring inline SWKBD frontend text that does not fit the protocol buffer");
                action.type = InlineFrontendActionType::Update;
                action.text = inlineText;
                action.cursor = inlineCursorPosition;
                return;
            }
            inlineText = std::move(event.text);
            inlineCursorPosition = std::clamp(event.cursor, 0, static_cast<i32>(inlineText.size()));
        }
        switch (event.type) {
            case FrontendEventType::TextChanged: {
                const InlineReply reply{inlineUseUtf8
                                            ? (inlineUseChangedStringV2 ? InlineReply::ChangedStringUtf8V2 : InlineReply::ChangedStringUtf8)
                                            : (inlineUseChangedStringV2 ? InlineReply::ChangedStringV2 : InlineReply::ChangedString)};
                SendInlineTextReplyLocked(reply);
                break;
            }
            case FrontendEventType::Submit:
                SendInlineTextReplyLocked(inlineUseUtf8 ? InlineReply::DecidedEnterUtf8 : InlineReply::DecidedEnter);
                HideInlineKeyboardLocked();
                action.type = InlineFrontendActionType::Hide;
                break;
            case FrontendEventType::Cancel:
            case FrontendEventType::FrontendDestroyed:
                SendInlineReplyLocked(InlineReply::DecidedCancel);
                HideInlineKeyboardLocked();
                action.type = InlineFrontendActionType::Hide;
                break;
            case FrontendEventType::TextCheckAccepted:
            case FrontendEventType::TextCheckDismissed:
                break;
        }
    }

    void SoftwareKeyboardApplet::OnSoftwareKeyboardFrontendEvent(FrontendEvent event) {
        if (mode == service::applet::LibraryAppletMode::PartialForeground ||
            mode == service::applet::LibraryAppletMode::PartialForegroundWithIndirectDisplay) {
            InlineFrontendAction action;
            {
                std::scoped_lock lock{inlineMutex};
                HandleInlineFrontendEventLocked(std::move(event), action);
            }
            ExecuteInlineFrontendAction(std::move(action));
            return;
        }

        NormalAction action;
        {
            std::scoped_lock lock{normalMutex};
            if (!normalSessionId || event.sessionId != *normalSessionId || !normalState)
                return;
            switch (event.type) {
                case FrontendEventType::Submit:
                    if (CanSerializeText(event.text, config.commonConfig.isUseUtf8 ? TextEncoding::Utf8 : TextEncoding::Utf16)) {
                        action = normalState->Submit(std::move(event.text));
                    } else {
                        LOGW("Ignoring SWKBD frontend text that does not fit the protocol buffer");
                        action.type = NormalActionType::ResumeEditing;
                    }
                    break;
                case FrontendEventType::Cancel:
                case FrontendEventType::FrontendDestroyed:
                    action = normalState->Cancel();
                    break;
                case FrontendEventType::TextCheckAccepted:
                    action = normalState->ResolveTextCheck(true);
                    break;
                case FrontendEventType::TextCheckDismissed:
                    action = normalState->ResolveTextCheck(false);
                    break;
                case FrontendEventType::TextChanged:
                    break;
            }
        }
        ExecuteNormalAction(std::move(action));
    }

    Result SoftwareKeyboardApplet::GetResult() {
        return {};
    }

    void SoftwareKeyboardApplet::RequestExit() {
        std::optional<FrontendSessionId> sessionId;
        if (mode == service::applet::LibraryAppletMode::PartialForeground ||
            mode == service::applet::LibraryAppletMode::PartialForegroundWithIndirectDisplay) {
            std::scoped_lock lock{inlineMutex};
            inlineStarted = false;
            inlineState = InlineState::Uninitialized;
            sessionId = std::exchange(inlineSessionId, std::nullopt);
        } else {
            std::scoped_lock lock{normalMutex};
            if (normalState)
                normalState->Close();
            sessionId = std::exchange(normalSessionId, std::nullopt);
        }
        if (sessionId)
            state.jvm->CloseSoftwareKeyboardSession(*sessionId);
    }

    bool SoftwareKeyboardApplet::GetIndirectLayerImage(span<u8> image) {
        std::scoped_lock lock{inlineMutex};
        if (mode != service::applet::LibraryAppletMode::PartialForegroundWithIndirectDisplay || !inlineStarted)
            return false;
        std::fill(image.begin(), image.end(), u8{});
        return true;
    }

    void SoftwareKeyboardApplet::PushNormalDataToApplet(std::shared_ptr<service::am::IStorage> data) {
        PushNormalInput(std::move(data));
    }

    void SoftwareKeyboardApplet::PushInteractiveDataToApplet(std::shared_ptr<service::am::IStorage> data) {
        if (mode == service::applet::LibraryAppletMode::PartialForeground ||
            mode == service::applet::LibraryAppletMode::PartialForegroundWithIndirectDisplay) {
            InlineFrontendAction action;
            {
                std::scoped_lock lock{inlineMutex};
                action = ProcessInlineRequestLocked(data->GetSpan());
            }
            ExecuteInlineFrontendAction(std::move(action));
            return;
        }

        const auto dataSpan{data->GetSpan()};
        if (dataSpan.size() < sizeof(ValidationResult)) {
            LOGW("Software keyboard text-check result is truncated");
            return;
        }
        const auto validation{ReadStruct<ValidationResult>(dataSpan, "Software keyboard text-check result is truncated")};
        if (validation.result != TextCheckResult::Success && validation.result != TextCheckResult::ShowFailureDialog &&
            validation.result != TextCheckResult::ShowConfirmDialog && validation.result != TextCheckResult::Silent) {
            LOGW("Unknown software keyboard text-check result: 0x{:X}", static_cast<u32>(validation.result));
            return;
        }
        auto message{ReadNullTerminatedText(validation.chars,
                                            config.commonConfig.isUseUtf8 ? TextEncoding::Utf8 : TextEncoding::Utf16)};
        if (!message) {
            LOGW("Software keyboard text-check message has invalid encoding");
            message.emplace();
        }

        NormalAction action;
        {
            std::scoped_lock lock{normalMutex};
            if (!normalState)
                return;
            action = normalState->ApplyTextCheck(validation.result, std::move(*message));
        }
        ExecuteNormalAction(std::move(action));
    }
}
