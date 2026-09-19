#include <cstdlib>
#include <iostream>
#include <vector>

#include "skyline/applet/swkbd/software_keyboard_config.h"
#include "skyline/applet/swkbd/software_keyboard_frontend.h"
#include "skyline/applet/swkbd/software_keyboard_state.h"
#include "skyline/applet/swkbd/software_keyboard_text.h"

using namespace skyline;
using namespace skyline::applet::swkbd;

namespace {
    void Require(bool condition, const char *message) {
        if (!condition) {
            std::cerr << "FAIL " << message << '\n';
            std::exit(1);
        }
    }

    void TestText() {
        std::array<u8, 10> guarded{};
        guarded.front() = guarded.back() = 0xA5;
        auto result{WriteText(span<u8>{guarded}.subspan(1, 8), u"A\U0001F642B", TextEncoding::Utf8)};
        Require(result.valid && !result.truncated && result.bytesWritten == 6, "UTF-8 scalar write");
        Require(guarded.front() == 0xA5 && guarded.back() == 0xA5, "UTF-8 canaries");

        std::array<u8, 4> shortUtf16{};
        result = WriteText(shortUtf16, u"A\U0001F642", TextEncoding::Utf16);
        Require(result.truncated && result.codeUnitsConsumed == 1 && result.bytesWritten == 2, "UTF-16 surrogate is not split");

        std::array<u8, 8> source{0, 0, 'A', 0, 'B', 0, 0, 0};
        Require(ReadUtf16Text(source, 2, 2) == std::optional<std::u16string>{u"AB"}, "bounded initial text");
        Require(!ReadUtf16Text(source, 3, 1), "reject unaligned initial text");

        std::array<u8, 7> utf8{'A', 0xF0, 0x9F, 0x99, 0x82, 0, 0};
        Require(ReadNullTerminatedText(utf8, TextEncoding::Utf8) == std::optional<std::u16string>{u"A\U0001F642"},
                "bounded UTF-8 read");
        utf8[2] = 0;
        Require(!ReadNullTerminatedText(utf8, TextEncoding::Utf8), "reject truncated UTF-8 sequence");
    }

    void TestConfig() {
        KeyboardConfigVB config{};
        config.commonConfig.textMinLength = 3000;
        NormalizeNormalConfig(config);
        Require(config.commonConfig.textMaxLength == 1002 && config.commonConfig.textMinLength == 1002, "UTF-16 bounds");
        Require(config.commonConfig.inputFormMode == InputFormMode::MultiLine, "normal multiline normalization");

        config = {};
        config.commonConfig.isUseUtf8 = true;
        NormalizeNormalConfig(config);
        Require(config.commonConfig.textMaxLength == 2004, "UTF-8 bounds");
    }

    void TestState() {
        NormalKeyboardStateMachine plain{false};
        plain.Open(u"initial");
        auto action{plain.Submit(u"typed")};
        Require(action.type == NormalActionType::Complete && action.closeResult == CloseResult::Enter && action.text == u"typed", "normal submit");

        NormalKeyboardStateMachine checked{true};
        checked.Open({});
        action = checked.Submit(u"checked");
        Require(action.type == NormalActionType::RequestTextCheck, "text-check request");
        action = checked.ApplyTextCheck(TextCheckResult::ShowConfirmDialog, u"confirm");
        Require(action.type == NormalActionType::ShowTextCheck, "text-check dialog");
        action = checked.ResolveTextCheck(false);
        Require(action.type == NormalActionType::ResumeEditing && checked.GetState() == NormalKeyboardState::Editing, "text-check retry");
        action = checked.Cancel();
        Require(action.type == NormalActionType::Complete && action.closeResult == CloseResult::Cancel && action.text.empty(), "normal cancel differs from submit");
    }

    class Callbacks final : public SoftwareKeyboardFrontendCallbacks {
      public:
        FrontendSessionRegistry *registry{};
        unsigned calls{};
        void OnSoftwareKeyboardFrontendEvent(FrontendEvent event) override {
            ++calls;
            if (registry)
                registry->Unregister(event.sessionId);
        }
    };

    void TestSessions() {
        FrontendSessionRegistry registry;
        auto callbacks{std::make_shared<Callbacks>()};
        callbacks->registry = &registry;
        const auto first{registry.Register(callbacks)};
        const auto second{registry.Register(callbacks)};
        Require(first != second && second > first, "monotonic session IDs");
        Require(registry.Dispatch({.sessionId = first, .type = FrontendEventType::Cancel, .text = {}, .cursor = 0}), "dispatch active session");
        Require(callbacks->calls == 1 && !registry.Dispatch({.sessionId = first, .type = {}, .text = {}, .cursor = 0}), "reentrant unregister and stale rejection");
        callbacks.reset();
        Require(!registry.Dispatch({.sessionId = second, .type = {}, .text = {}, .cursor = 0}), "expired callback rejection");
    }
}

int main() {
    TestText();
    TestConfig();
    TestState();
    TestSessions();
    std::cout << "PASS SWKBD serialization, normal state, and frontend sessions\n";
}
