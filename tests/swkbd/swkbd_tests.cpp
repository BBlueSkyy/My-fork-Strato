#include <cstdlib>
#include <iostream>
#include <vector>

#include "skyline/applet/swkbd/software_keyboard_config.h"
#include "skyline/applet/swkbd/software_keyboard_frontend.h"
#include "skyline/applet/swkbd/software_keyboard_state.h"
#include "skyline/applet/swkbd/software_keyboard_text.h"
#include "skyline/services/am/applet/indirect_layer_registry.h"
#include "skyline/services/visrv/indirect_layer_layout.h"

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
        std::array<u8, 2> unpairedSurrogate{0x00, 0xD8};
        Require(!ReadUtf16Text(unpairedSurrogate, 0, 1), "reject unpaired UTF-16 surrogate");

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

    void TestIndirectLayers() {
        service::am::IndirectLayerRegistry registry;
        auto owner{std::make_shared<int>(1)};
        auto applet{std::shared_ptr<service::am::IApplet>(owner, reinterpret_cast<service::am::IApplet *>(owner.get()))};
        const auto handle{registry.Register(applet, 0x11, 0x22)};
        Require(handle != 0 && registry.Get(handle, 0x11, 0x22) == applet, "indirect handle identity");
        Require(!registry.Get(handle, 0x12, 0x22) && !registry.Get(handle, 0x11, 0x23), "PID and ARUID validation");
        registry.Unregister(handle);
        Require(!registry.Get(handle, 0x11, 0x22), "indirect handle lifetime");

        const auto expiredHandle{registry.Register(applet, 0x11, 0x22)};
        applet.reset();
        owner.reset();
        Require(!registry.Get(expiredHandle, 0x11, 0x22), "expired applet rejection");
    }

    void TestIndirectLayerLayout() {
        service::visrv::IndirectLayerLayout layout;
        Require(service::visrv::CalculateIndirectLayerLayout(1280, 720, layout), "indirect layout dimensions");
        Require(layout.stride == 0x1400, "indirect image stride");
        Require(layout.imageSize == 0x384000, "indirect image size");
        Require(layout.requiredSize == 0x3A0000, "indirect required memory size");
        Require(service::visrv::CalculateIndirectLayerLayout(17, 3, layout), "unaligned indirect dimensions");
        Require(layout.stride == 68 && layout.imageSize == 204 && layout.requiredSize == 0x20000,
                "indirect image layout remains linear");
        Require(!service::visrv::CalculateIndirectLayerLayout(0, 720, layout), "reject invalid indirect width");
    }
}

int main() {
    TestText();
    TestConfig();
    TestState();
    TestSessions();
    TestIndirectLayers();
    TestIndirectLayerLayout();
    std::cout << "PASS SWKBD serialization, normal state, frontend sessions, and indirect handles\n";
}
