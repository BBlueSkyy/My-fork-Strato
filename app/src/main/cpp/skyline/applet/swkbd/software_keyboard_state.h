// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Strato Team and Contributors

#pragma once

#include <string>
#include <common.h>

namespace skyline::applet::swkbd {
    enum class CloseResult : u32 {
        Enter = 0,
        Cancel = 1,
    };

    enum class TextCheckResult : u32 {
        Success = 0,
        ShowFailureDialog = 1,
        ShowConfirmDialog = 2,
        Silent = 3,
    };

    enum class NormalKeyboardState {
        Idle,
        Editing,
        CheckingText,
        ShowingTextCheck,
        Complete,
        Closed,
    };

    enum class NormalActionType {
        None,
        RequestTextCheck,
        ShowTextCheck,
        ResumeEditing,
        Complete,
    };

    struct NormalAction {
        NormalActionType type{};
        TextCheckResult textCheckResult{};
        CloseResult closeResult{};
        std::u16string text;
        std::u16string message;
    };

    class NormalKeyboardStateMachine {
      private:
        bool useTextCheck;
        NormalKeyboardState state{NormalKeyboardState::Idle};
        std::u16string text;

      public:
        explicit NormalKeyboardStateMachine(bool useTextCheck) : useTextCheck{useTextCheck} {}

        NormalKeyboardState GetState() const { return state; }
        NormalAction Open(std::u16string initialText);
        NormalAction Submit(std::u16string submittedText);
        NormalAction Cancel();
        NormalAction ApplyTextCheck(TextCheckResult result, std::u16string message);
        NormalAction ResolveTextCheck(bool accepted);
        void Close();
    };
}
