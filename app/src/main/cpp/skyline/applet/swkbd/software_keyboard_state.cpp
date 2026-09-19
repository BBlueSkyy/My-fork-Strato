// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Strato Team and Contributors

#include "software_keyboard_state.h"

namespace skyline::applet::swkbd {
    NormalAction NormalKeyboardStateMachine::Open(std::u16string initialText) {
        if (state != NormalKeyboardState::Idle)
            return {};
        text = std::move(initialText);
        state = NormalKeyboardState::Editing;
        return {};
    }

    NormalAction NormalKeyboardStateMachine::Submit(std::u16string submittedText) {
        if (state != NormalKeyboardState::Editing)
            return {};
        text = std::move(submittedText);
        if (useTextCheck) {
            state = NormalKeyboardState::CheckingText;
            return {.type = NormalActionType::RequestTextCheck, .textCheckResult = {}, .closeResult = {}, .text = text, .message = {}};
        }
        state = NormalKeyboardState::Complete;
        return {.type = NormalActionType::Complete, .textCheckResult = {}, .closeResult = CloseResult::Enter, .text = text, .message = {}};
    }

    NormalAction NormalKeyboardStateMachine::Cancel() {
        if (state == NormalKeyboardState::Complete || state == NormalKeyboardState::Closed)
            return {};
        state = NormalKeyboardState::Complete;
        return {.type = NormalActionType::Complete, .textCheckResult = {}, .closeResult = CloseResult::Cancel, .text = {}, .message = {}};
    }

    NormalAction NormalKeyboardStateMachine::ApplyTextCheck(TextCheckResult result, std::u16string message) {
        if (state != NormalKeyboardState::CheckingText)
            return {};
        if (result == TextCheckResult::Success) {
            state = NormalKeyboardState::Complete;
            return {.type = NormalActionType::Complete, .textCheckResult = {}, .closeResult = CloseResult::Enter, .text = text, .message = {}};
        }
        if (result == TextCheckResult::Silent) {
            state = NormalKeyboardState::Editing;
            return {.type = NormalActionType::ResumeEditing, .textCheckResult = {}, .closeResult = {}, .text = {}, .message = {}};
        }
        if (result != TextCheckResult::ShowFailureDialog && result != TextCheckResult::ShowConfirmDialog)
            return {};
        state = NormalKeyboardState::ShowingTextCheck;
        return {.type = NormalActionType::ShowTextCheck, .textCheckResult = result, .closeResult = {}, .text = {}, .message = std::move(message)};
    }

    NormalAction NormalKeyboardStateMachine::ResolveTextCheck(bool accepted) {
        if (state != NormalKeyboardState::ShowingTextCheck)
            return {};
        if (accepted) {
            state = NormalKeyboardState::Complete;
            return {.type = NormalActionType::Complete, .textCheckResult = {}, .closeResult = CloseResult::Enter, .text = text, .message = {}};
        }
        state = NormalKeyboardState::Editing;
        return {.type = NormalActionType::ResumeEditing, .textCheckResult = {}, .closeResult = {}, .text = {}, .message = {}};
    }

    void NormalKeyboardStateMachine::Close() {
        state = NormalKeyboardState::Closed;
    }
}