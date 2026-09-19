// SPDX-License-Identifier: MPL-2.0
// Copyright © 2022 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <mutex>
#include <optional>
#include <services/am/applet/IApplet.h>
#include <services/applet/common_arguments.h>
#include "software_keyboard_config.h"
#include "software_keyboard_frontend.h"
#include "software_keyboard_state.h"

namespace skyline::applet::swkbd {
    /**
     * @url https://switchbrew.org/wiki/Software_Keyboard
     * @brief Translates HOS software-keyboard applet transactions to an asynchronous host frontend.
     */
    class SoftwareKeyboardApplet final : public service::am::IApplet,
                                         public service::am::EnableNormalQueue,
                                         public SoftwareKeyboardFrontendCallbacks {
      private:
        enum class InlineState : u32 {
            Uninitialized = 0,
            Hidden = 1,
            Appearing = 2,
            Shown = 3,
            Disappearing = 4,
        };

        enum class InlineRequest : u32 {
            Finalize = 0x4,
            SetUserWordInfo = 0x6,
            SetCustomizeDictionary = 0x7,
            Calc = 0xA,
            SetCustomizedDictionaries = 0xB,
            UnsetCustomizedDictionaries = 0xC,
            SetChangedStringV2 = 0xD,
            SetMovedCursorV2 = 0xE,
        };

        enum class InlineReply : u32 {
            FinishedInitialize = 0x0,
            Default = 0x1,
            ChangedString = 0x2,
            DecidedEnter = 0x5,
            DecidedCancel = 0x6,
            ChangedStringUtf8 = 0x7,
            DecidedEnterUtf8 = 0x9,
            UnsetCustomizeDictionary = 0xA,
            ReleasedUserWordInfo = 0xB,
            UnsetCustomizedDictionaries = 0xC,
            ChangedStringV2 = 0xD,
            ChangedStringUtf8V2 = 0xF,
        };

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
            std::array<u8, SwkbdTextBytes> chars;
        };
        static_assert(sizeof(ValidationResult) == 0x7D8);
        #pragma pack(pop)

        enum class InlineFrontendActionType {
            None,
            Show,
            Hide,
            Update,
            Close,
        };

        struct InlineFrontendAction {
            InlineFrontendActionType type{};
            FrontendKeyboardConfig config{};
            std::u16string text;
            i32 cursor{};
        };

        KeyboardConfigVB config{};
        service::applet::LibraryAppletMode mode{};

        std::mutex normalMutex;
        std::unique_ptr<NormalKeyboardStateMachine> normalState;
        std::optional<FrontendSessionId> normalSessionId;
        bool normalCompleted{};

        std::mutex inlineMutex;
        std::optional<FrontendSessionId> inlineSessionId;
        InlineState inlineState{InlineState::Uninitialized};
        bool inlineStarted{};
        bool inlineUseUtf8{};
        bool inlineUseChangedStringV2{};
        i32 inlineCursorPosition{};
        std::u16string inlineText;

        Result StartNormal();
        Result StartInline();
        void ExecuteNormalAction(NormalAction action);
        void CompleteNormal(CloseResult closeResult, std::u16string text);

        InlineFrontendAction ProcessInlineRequestLocked(span<u8> data);
        InlineFrontendAction ProcessInlineCalcLocked(span<u8> data);
        void ExecuteInlineFrontendAction(InlineFrontendAction action);
        void ConfigureInlineKeyboardLocked(span<u8> calc, bool extendedLayout);
        void ChangeInlineStateLocked(InlineState newState);
        void HideInlineKeyboardLocked();
        void SendInlineReplyLocked(InlineReply reply);
        void SendInlineTextReplyLocked(InlineReply reply);
        void HandleInlineFrontendEventLocked(FrontendEvent event, InlineFrontendAction &action);
        FrontendKeyboardConfig CopyFrontendConfig() const;

      public:
        SoftwareKeyboardApplet(const DeviceState &state, service::ServiceManager &manager,
                               std::shared_ptr<kernel::type::KEvent> onAppletStateChanged,
                               std::shared_ptr<kernel::type::KEvent> onNormalDataPushFromApplet,
                               std::shared_ptr<kernel::type::KEvent> onInteractiveDataPushFromApplet,
                               service::applet::LibraryAppletMode appletMode);
        ~SoftwareKeyboardApplet() override;

        Result Start() override;
        Result GetResult() override;
        void RequestExit() override;
        bool GetIndirectLayerImage(span<u8> image) override;
        void PushNormalDataToApplet(std::shared_ptr<service::am::IStorage> data) override;
        void PushInteractiveDataToApplet(std::shared_ptr<service::am::IStorage> data) override;
        void OnSoftwareKeyboardFrontendEvent(FrontendEvent event) override;
    };
}
