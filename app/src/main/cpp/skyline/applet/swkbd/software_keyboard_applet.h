// SPDX-License-Identifier: MPL-2.0
// Copyright © 2022 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <future>
#include <services/am/applet/IApplet.h>
#include <services/applet/common_arguments.h>
#include <jvm.h>
#include "software_keyboard_config.h"

namespace skyline::applet::swkbd {
    static_assert(sizeof(KeyboardConfigVB) == sizeof(JvmManager::KeyboardConfig));

    /**
     * @url https://switchbrew.org/wiki/Software_Keyboard
     * @brief An implementation for the Software Keyboard (swkbd) Applet which handles translating guest applet transactions to the appropriate host behavior
     */
    class SoftwareKeyboardApplet : public service::am::IApplet, service::am::EnableNormalQueue {
      private:
        enum class CloseResult : u32 {
            Enter = 0x0,
            Cancel = 0x1,
        };

        enum class TextCheckResult : u32 {
            Success = 0x0,
            ShowFailureDialog = 0x1,
            ShowConfirmDialog = 0x2,
        };

        enum class InlineState : u32 {
            Uninitialized = 0,
            Initialized = 1,
            Appearing = 2,
            Shown = 3,
            Disappearing = 4,
        };

        enum class InlineRequest : u32 {
            Finalize = 0x4,
            SetUserWordInfo = 0x6,
            SetCustomizeDic = 0x7,
            Calc = 0xA,
            SetCustomizedDictionaries = 0xB,
            UnsetCustomizedDictionaries = 0xC,
            SetChangedStringV2Flag = 0xD,
            SetMovedCursorV2Flag = 0xE,
        };

        enum class InlineReply : u32 {
            FinishedInitialize = 0x0,
            Default = 0x1,
            ChangedString = 0x2,
            MovedCursor = 0x3,
            DecidedEnter = 0x5,
            DecidedCancel = 0x6,
            ChangedStringUtf8 = 0x7,
            MovedCursorUtf8 = 0x8,
            DecidedEnterUtf8 = 0x9,
            UnsetCustomizeDic = 0xA,
            ReleasedUserWordInfo = 0xB,
            UnsetCustomizedDictionaries = 0xC,
            ChangedStringV2 = 0xD,
            MovedCursorV2 = 0xE,
            ChangedStringUtf8V2 = 0xF,
            MovedCursorUtf8V2 = 0x10,
        };

        static constexpr u32 SwkbdTextBytes{0x7D4};
        static constexpr u32 MaxOneLineChars{32};
        static constexpr size_t InlineUtf16TextBytes{0x3EC};
        static constexpr size_t InlineUtf8TextBytes{0x7D4};

        #pragma pack(push, 1)
        struct OutputResult {
            CloseResult closeResult;
            std::array<u8, SwkbdTextBytes> chars{};

            OutputResult(CloseResult closeResult, std::u16string_view text, bool useUtf8Storage);
        };
        static_assert(sizeof(OutputResult) == 0x7D8);

        struct ValidationRequest {
            u64 size;
            std::array<u8, SwkbdTextBytes> chars{};

            ValidationRequest(std::u16string_view text, bool useUtf8Storage);
        };
        static_assert(sizeof(ValidationRequest) == 0x7DC);

        struct ValidationResult {
            TextCheckResult result;
            std::array<char16_t, SwkbdTextBytes / sizeof(char16_t)> chars;
        };
        static_assert(sizeof(ValidationResult) == 0x7D8);
        #pragma pack(pop)

        KeyboardConfigVB config{};
        service::applet::LibraryAppletMode mode{};
        bool validationPending{};
        std::u16string currentText{};
        CloseResult currentResult{};
        jobject dialog{};

        std::mutex inlineMutex;
        std::future<void> inlineInputFuture;
        JvmManager::KeyboardHandle pendingInlineWaitDialog{};
        InlineState inlineState{InlineState::Uninitialized};
        bool inlineStarted{};
        bool inlineUseUtf8{};
        bool inlineUseChangedStringV2{};
        bool inlineUseMovedCursorV2{};
        i32 inlineCursorPosition{};

        void SendResult();
        Result StartInline();
        void ProcessInlineRequest(span<u8> data);
        void ProcessInlineCalc(span<u8> data);
        void ConfigureInlineKeyboard(span<u8> calc, bool extended);
        void ShowInlineKeyboard();
        void HideInlineKeyboard();
        void WaitForInlineKeyboardInput(JvmManager::KeyboardHandle workerDialog);
        void SendInlineReply(InlineReply reply, span<const u8> payload = {});
        void SendInlineTextReply(InlineReply reply, std::u16string_view text, u32 cursor = 0);

      public:
        SoftwareKeyboardApplet(const DeviceState &state, service::ServiceManager &manager, std::shared_ptr<kernel::type::KEvent> onAppletStateChanged, std::shared_ptr<kernel::type::KEvent> onNormalDataPushFromApplet, std::shared_ptr<kernel::type::KEvent> onInteractiveDataPushFromApplet, service::applet::LibraryAppletMode appletMode);
        ~SoftwareKeyboardApplet() override;

        Result Start() override;
        Result GetResult() override;
        bool GetIndirectLayerImage(span<u8> image) override;
        void PushNormalDataToApplet(std::shared_ptr<service::am::IStorage> data) override;
        void PushInteractiveDataToApplet(std::shared_ptr<service::am::IStorage> data) override;
    };
}
