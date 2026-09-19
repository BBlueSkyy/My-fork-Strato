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
        LOGD("Inline swkbd start: mode=0x{:X}, modeFlag={}", static_cast<u32>(mode), partialForeground);
        if (mode != expectedMode)
            LOGW("Inline keyboard InitializeArg mode does not match LibraryAppletMode (expected=0x{:X}, actual=0x{:X})",
                 static_cast<u32>(expectedMode), static_cast<u32>(mode));

        normalInputData.pop();
        std::scoped_lock inlineLock{inlineMutex};
        inlineState = InlineState::Uninitialized;
        inlineStarted = true;
        return {};
    }

    void SoftwareKeyboardApplet::ChangeInlineState(InlineState state) {
        if (inlineState == state)
            return;

        inlineState = state;
        LOGD("Inline swkbd state -> 0x{:X}", static_cast<u32>(inlineState));
        SendInlineReply(InlineReply::Default);
    }

    void SoftwareKeyboardApplet::SendInlineReply(InlineReply reply) {
        const size_t size{InlineReplyHeaderSize + (reply == InlineReply::FinishedInitialize ? 1 : 0)};
        std::vector<u8> response(size);
        WriteInlineValue(response, 0, inlineState);
        WriteInlineValue(response, sizeof(u32), reply);
        LOGD("Inline swkbd reply: type=0x{:X}, state=0x{:X}, size=0x{:X}",
             static_cast<u32>(reply), static_cast<u32>(inlineState), response.size());

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

        LOGD("Inline swkbd text reply: type=0x{:X}, state=0x{:X}, chars={}",
             static_cast<u32>(reply), static_cast<u32>(inlineState), currentText.size());
        PushInteractiveDataAndSignal(std::make_shared<service::am::VectorIStorage>(state, manager, std::move(response)));
    }

    void SoftwareKeyboardApplet::ConfigureInlineKeyboard(span<u8> calc, bool extendedLayout) {
        const size_t appearOffset{static_cast<size_t>(extendedLayout ? 0x18 : 0x20)};

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

        if (!dialog) {
            LOGW("Couldn't show inline keyboard dialog");
            SendInlineReply(InlineReply::DecidedCancel);
            ChangeInlineState(InlineState::Hidden);
            return;
        }

        ChangeInlineState(InlineState::Shown);
        pendingInlineWaitDialog = state.jvm->CloneKeyboardHandle(dialog);
    }

    void SoftwareKeyboardApplet::HideInlineKeyboard() {
        if (inlineState != InlineState::Shown && inlineState != InlineState::Appearing)
            return;

        ChangeInlineState(InlineState::Disappearing);
        if (dialog) {
            state.jvm->CloseKeyboard(dialog);
            dialog = {};
        }
        ChangeInlineState(InlineState::Hidden);
    }

    void SoftwareKeyboardApplet::WaitForInlineKeyboardInput(JvmManager::KeyboardHandle waitDialog) {
        while (true) {
            auto update{state.jvm->WaitForInlineKeyboardUpdate(waitDialog)};
            JvmManager::KeyboardHandle dismissedDialog{};
            bool finished{};

            {
                std::scoped_lock lock{inlineMutex};

                if (update.type == JvmManager::KeyboardUpdate::Type::Closed &&
                    (inlineState == InlineState::Shown || inlineState == InlineState::Appearing)) {
                    currentResult = CloseResult::Cancel;
                    SendInlineReply(InlineReply::DecidedCancel);
                    ChangeInlineState(InlineState::Disappearing);
                    dismissedDialog = dialog;
                    dialog = {};
                    ChangeInlineState(InlineState::Hidden);
                    finished = true;
                } else if (update.type == JvmManager::KeyboardUpdate::Type::Closed ||
                           inlineState == InlineState::Uninitialized ||
                    (inlineState != InlineState::Shown && inlineState != InlineState::Appearing)) {
                    finished = true;
                } else {
                    currentText = std::move(update.text);
                    inlineCursorPosition = std::clamp(update.cursor, 0, static_cast<i32>(currentText.size()));

                    if (update.type == JvmManager::KeyboardUpdate::Type::Changed) {
                        const InlineReply changedReply{inlineUseUtf8
                                                           ? (inlineUseChangedStringV2 ? InlineReply::ChangedStringUtf8V2 : InlineReply::ChangedStringUtf8)
                                                           : (inlineUseChangedStringV2 ? InlineReply::ChangedStringV2 : InlineReply::ChangedString)};
                        SendInlineTextReply(changedReply);
                    } else {
                        if (update.type == JvmManager::KeyboardUpdate::Type::Enter) {
                            currentResult = CloseResult::Enter;
                            SendInlineTextReply(inlineUseUtf8 ? InlineReply::DecidedEnterUtf8 : InlineReply::DecidedEnter);
                        } else {
                            currentResult = CloseResult::Cancel;
                            SendInlineReply(InlineReply::DecidedCancel);
                        }
                        HideInlineKeyboard();
                        finished = true;
                    }
                }
            }

            if (dismissedDialog)
                state.jvm->ReleaseKeyboardHandle(dismissedDialog);
            if (finished)
                break;
        }

        state.jvm->ReleaseKeyboardHandle(waitDialog);
    }

    void SoftwareKeyboardApplet::ProcessInlineCalc(span<u8> calc) {
        if (calc.size() < 0x18) {
            LOGW("Software keyboard inline Calc is truncated: 0x{:X}", calc.size());
            return;
        }

        const auto calcArgSize{ReadInlineValue<u16>(calc, 0x4)};
        if (calcArgSize != calc.size()) {
            LOGW("Software keyboard inline Calc size mismatch: header=0x{:X}, storage=0x{:X}", calcArgSize, calc.size());
            return;
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
                return;
        }

        const u64 flags{ReadInlineValue<u64>(calc, 0x8)};
        const size_t cursorOffset{static_cast<size_t>(newLayout ? 0x8C : 0x1C)};
        const size_t inputTextOffset{static_cast<size_t>(extendedInputLayout ? 0x90 : 0x68)};
        const size_t utf8Offset{static_cast<size_t>(extendedInputLayout ? 0x484 : 0x45C)};
        LOGD("Inline swkbd Calc: size=0x{:X}, layout={}, flags=0x{:X}, state=0x{:X}",
             calcArgSize, newLayout ? "new" : (extendedInputLayout ? "legacy-extended" : "old"),
             flags, static_cast<u32>(inlineState));

        if (flags & InlineFlagSetInputText)
            currentText = ReadInlineString(calc, inputTextOffset, InlineInputTextBytes);
        if (flags & InlineFlagSetCursorPosition)
            inlineCursorPosition = ReadInlineValue<i32>(calc, cursorOffset);
        if (flags & InlineFlagSetUtf8Mode)
            inlineUseUtf8 = ReadInlineValue<u8>(calc, utf8Offset) != 0;

        if (inlineState <= InlineState::Hidden && (flags & InlineFlagUnsetCustomizeDictionary))
            SendInlineReply(InlineReply::UnsetCustomizeDictionary);
        if (inlineState <= InlineState::Hidden && (flags & InlineFlagUnsetUserWordInfo))
            SendInlineReply(InlineReply::ReleasedUserWordInfo);

        const bool initialize{(flags & InlineFlagInitialize) != 0};
        if (initialize && inlineState == InlineState::Uninitialized) {
            LOGD("Inline swkbd initializing from Calc: size=0x{:X}, flags=0x{:X}", calcArgSize, flags);
            ConfigureInlineKeyboard(calc, newLayout);
            ChangeInlineState(InlineState::Hidden);
            SendInlineReply(InlineReply::FinishedInitialize);
        }

        if ((flags & InlineFlagAppear) && inlineState == InlineState::Hidden) {
            ConfigureInlineKeyboard(calc, newLayout);
            ShowInlineKeyboard();
            return;
        }

        if ((flags & InlineFlagDisappear) && inlineState == InlineState::Shown) {
            HideInlineKeyboard();
            return;
        }
    }

    void SoftwareKeyboardApplet::ProcessInlineRequest(span<u8> data) {
        if (data.size() < sizeof(InlineRequest)) {
            LOGW("Software keyboard inline request is truncated");
            return;
        }

        const auto request{ReadInlineValue<InlineRequest>(data, 0)};
        LOGD("Inline swkbd request: type=0x{:X}, size=0x{:X}, state=0x{:X}",
             static_cast<u32>(request), data.size(), static_cast<u32>(inlineState));
        switch (request) {
            case InlineRequest::Finalize:
                inlineStarted = false;
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

    SoftwareKeyboardApplet::~SoftwareKeyboardApplet() {
        if (mode != service::applet::LibraryAppletMode::PartialForeground &&
            mode != service::applet::LibraryAppletMode::PartialForegroundWithIndirectDisplay)
            return;

        RequestExit();

        if (inlineInputFuture.valid())
            inlineInputFuture.wait();
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

    void SoftwareKeyboardApplet::RequestExit() {
        if (mode != service::applet::LibraryAppletMode::PartialForeground &&
            mode != service::applet::LibraryAppletMode::PartialForegroundWithIndirectDisplay)
            return;

        std::scoped_lock lock{inlineMutex};
        inlineStarted = false;
        inlineState = InlineState::Uninitialized;
        if (dialog) {
            state.jvm->CloseKeyboard(dialog);
            dialog = {};
        }
        if (pendingInlineWaitDialog) {
            state.jvm->ReleaseKeyboardHandle(pendingInlineWaitDialog);
            pendingInlineWaitDialog = {};
        }
    }

    bool SoftwareKeyboardApplet::GetIndirectLayerImage(span<u8> image) {
        std::scoped_lock lock{inlineMutex};
        if (mode != service::applet::LibraryAppletMode::PartialForegroundWithIndirectDisplay || !inlineStarted)
            return false;

        // The Android frontend draws the keyboard above the game's surface. Its guest layer
        // is transparent, including pitch/height padding, so it does not obscure the game.
        std::fill(image.begin(), image.end(), u8{});
        return true;
    }

    void SoftwareKeyboardApplet::PushNormalDataToApplet(std::shared_ptr<service::am::IStorage> data) {
        PushNormalInput(data);
    }

    void SoftwareKeyboardApplet::PushInteractiveDataToApplet(std::shared_ptr<service::am::IStorage> data) {
        if (mode == service::applet::LibraryAppletMode::PartialForeground ||
            mode == service::applet::LibraryAppletMode::PartialForegroundWithIndirectDisplay) {
            JvmManager::KeyboardHandle waitDialog{};
            {
                std::scoped_lock lock{inlineMutex};
                ProcessInlineRequest(data->GetSpan());
                waitDialog = pendingInlineWaitDialog;
                pendingInlineWaitDialog = {};
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