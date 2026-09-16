// SPDX-License-Identifier: MPL-2.0
// Copyright © 2022 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <future>
#include <services/am/applet/IApplet.h>
#include <services/applet/common_arguments.h>
#include <jvm.h>
#include "software_keyboard_config.h"
#include "software_keyboard_inline.h"

namespace skyline::applet::swkbd {
    static_assert(sizeof(KeyboardConfigVB) == sizeof(JvmManager::KeyboardConfig));

    /**
     * @url https://switchbrew.org/wiki/Software_Keyboard
     * @brief Translates Software Keyboard applet transactions to the Android frontend.
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

        static constexpr u32 SwkbdTextBytes{0x7D4};
        static constexpr u32 MaxOneLineChars{32};

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
        inline_protocol::InitializeArg inlineInitializeArg{};
        inline_protocol::CalcArgCommon inlineCalcCommon{};
        inline_protocol::CalcArgOldBody inlineCalcOld{};
        inline_protocol::CalcArgNewBody inlineCalcNew{};
        inline_protocol::State inlineState{inline_protocol::State::NotInitialized};
        bool inlineStarted{};
        bool inlineUseUtf8{};
        bool inlineUseChangedStringV2{};
        bool inlineUseMovedCursorV2{};
        bool inlineUsesNewLayout{};
        i32 inlineCursorPosition{};
        std::shared_ptr<service::am::IStorage> inlineDictionaryStorage{};

        void SendResult();

        Result StartInline();
        void ProcessInlineStorage(std::shared_ptr<service::am::IStorage> data);
        void ProcessInlineRequest(span<u8> data);
        void ProcessInlineCalc(span<u8> data);
        void ProcessInlineCalcOld();
        void ProcessInlineCalcNew();
        void ConfigureInlineKeyboardOld();
        void ConfigureInlineKeyboardNew();

        void ChangeInlineState(inline_protocol::State state);
        void ShowInlineKeyboard();
        void HideInlineKeyboard();
        void WaitForInlineKeyboardInput(JvmManager::KeyboardHandle workerDialog);

        void SendInlineReply(inline_protocol::Reply reply, span<const u8> payload = {});
        void SendInlineTextReply(inline_protocol::Reply reply, std::u16string_view text, i32 cursor = 0);

      public:
        SoftwareKeyboardApplet(const DeviceState &state, service::ServiceManager &manager,
                               std::shared_ptr<kernel::type::KEvent> onAppletStateChanged,
                               std::shared_ptr<kernel::type::KEvent> onNormalDataPushFromApplet,
                               std::shared_ptr<kernel::type::KEvent> onInteractiveDataPushFromApplet,
                               service::applet::LibraryAppletMode appletMode);
        ~SoftwareKeyboardApplet() override;

        Result Start() override;
        Result GetResult() override;
        void PushNormalDataToApplet(std::shared_ptr<service::am::IStorage> data) override;
        void PushInteractiveDataToApplet(std::shared_ptr<service::am::IStorage> data) override;
    };
}
