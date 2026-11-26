#include <cstdlib>
#include <cstring>
#include <chrono>
#include <condition_variable>
#include <future>
#include <iostream>
#include <mutex>
#include <vector>

#include "skyline/applet/swkbd/software_keyboard_config.h"
#include "skyline/applet/swkbd/software_keyboard_frontend.h"
#include "skyline/applet/swkbd/frontend_task_queue.h"
#include "skyline/applet/swkbd/software_keyboard_state.h"
#include "skyline/applet/swkbd/software_keyboard_text.h"
#include "skyline/applet/swkbd/software_keyboard_inline.h"
#include "skyline/services/am/applet/indirect_layer_registry.h"
#include "skyline/services/visrv/indirect_layer_layout.h"

using namespace skyline;
using namespace skyline::applet::swkbd;

namespace {
    void Require(bool condition, const char *message);

    void TestFrontendQueue() {
        using namespace std::chrono_literals;
        FrontendTaskQueue queue;
        std::mutex mutex;
        std::condition_variable condition;
        std::vector<int> order;
        std::promise<void> release;
        auto gate{release.get_future().share()};
        Require(queue.Post([gate, &mutex, &condition, &order] {
            gate.wait();
            {
                std::lock_guard lock{mutex};
                order.push_back(1);
            }
            condition.notify_all();
        }), "first frontend request enqueued");
        Require(queue.Post([&mutex, &condition, &order] {
            {
                std::lock_guard lock{mutex};
                order.push_back(2);
            }
            condition.notify_all();
        }), "second frontend request enqueued without waiting for first");
        {
            std::lock_guard lock{mutex};
            Require(order.empty(), "blocked JNI request does not block the guest caller");
        }
        release.set_value();
        {
            std::unique_lock lock{mutex};
            Require(condition.wait_for(lock, 2s, [&] { return order.size() == 2; }), "frontend requests complete");
            Require(order == std::vector<int>({1, 2}), "frontend requests retain order");
        }
        queue.Stop();
        Require(!queue.Post([] {}), "closed frontend rejects requests");
    }
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

        config = {};
        NormalizeInlineConfig(config, 14, -1);
        Require(config.commonConfig.textMaxLength == 14 && config.commonConfig.textMinLength == 0,
                "inline negative minimum is unset");
        Require(config.commonConfig.inputFormMode == InputFormMode::OneLine, "inline short input is one-line");

        config = {};
        NormalizeInlineConfig(config, -1, -1);
        Require(config.commonConfig.textMaxLength == 500 && config.commonConfig.textMinLength == 0,
                "inline negative limits use protocol defaults");
        Require(config.commonConfig.inputFormMode == InputFormMode::MultiLine, "inline default input is multiline");

        config = {};
        NormalizeInlineConfig(config, 10, 20);
        Require(config.commonConfig.textMaxLength == 10 && config.commonConfig.textMinLength == 10,
                "inline minimum is clamped to maximum");
    }

    void TestInlineTextReplies() {
        struct ReplyCase { InlineReply reply; bool utf8; bool changed; bool moved; bool v2; };
        const ReplyCase cases[]{
            {InlineReply::ChangedString, false, true, false, false},
            {InlineReply::ChangedStringV2, false, true, false, true},
            {InlineReply::ChangedStringUtf8, true, true, false, false},
            {InlineReply::ChangedStringUtf8V2, true, true, false, true},
            {InlineReply::MovedCursor, false, false, true, false},
            {InlineReply::MovedCursorV2, false, false, true, true},
            {InlineReply::MovedCursorUtf8, true, false, true, false},
            {InlineReply::MovedCursorUtf8V2, true, false, true, true},
            {InlineReply::DecidedEnter, false, false, false, false},
            {InlineReply::DecidedEnterUtf8, true, false, false, false},
        };
        const std::u16string text{u"Aé\U0001F600"};
        for (const auto test : cases) {
            for (const i32 cursor : {-10, 2, 99}) {
                const auto reply{BuildInlineTextReply(3, test.reply, text, cursor)};
                const size_t textBytes{test.utf8 ? 0x7D4U : 0x3ECU};
                const size_t args{test.changed ? 16U : test.moved ? 8U : 4U};
                Require(reply.size() == 8 + textBytes + args + (test.v2 ? 1 : 0), "inline reply exact wire size");
                auto read = [&](size_t offset) { u32 value; std::memcpy(&value, reply.data() + offset, sizeof(value)); return value; };
                Require(read(0) == 3 && read(4) == static_cast<u32>(test.reply), "inline state and reply ID");
                const auto decoded{ReadNullTerminatedText({reply.data() + 8, textBytes}, test.utf8 ? TextEncoding::Utf8 : TextEncoding::Utf16)};
                Require(decoded && *decoded == text, "inline unicode roundtrip");
                Require(read(8 + textBytes) == text.size(), "inline length is UTF16 units even for UTF8");
                if (test.changed)
                    Require(read(12 + textBytes) == 0xFFFFFFFFU && read(16 + textBytes) == 0xFFFFFFFFU, "changed dictionary cursor sentinels");
                if (test.changed || test.moved)
                    Require(read(8 + textBytes + args - 4) == static_cast<u32>(std::clamp(cursor, 0, static_cast<i32>(text.size()))), "inline cursor clamp and wire offset");
                if (test.v2)
                    Require(reply.back() == 0, "inline V2 final flag");
            }
        }
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
        Require(registry.IsRegistered(first), "queued frontend request sees active session");
        Require(registry.Dispatch({.sessionId = first, .type = FrontendEventType::Cancel, .text = {}, .cursor = 0}), "dispatch active session");
        Require(!registry.IsRegistered(first), "queued frontend request skips closed session");
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

    // Ryujinx's published formula (pitch/height 64-aligned, then 0x1000, then 0x20000). Kept here only to document the divergence.
    std::uint64_t RyujinxReferenceRequiredSize(std::uint64_t width, std::uint64_t height) {
        using service::visrv::AlignIndirectLayerSize;
        const auto pitch{AlignIndirectLayerSize(width * 4, 64)};
        const auto memorySize{pitch * AlignIndirectLayerSize(height, 64)};
        return AlignIndirectLayerSize(AlignIndirectLayerSize(memorySize, 0x1000), 0x20000);
    }

    void TestIndirectLayerLayout() {
        using namespace service::visrv;
        IndirectLayerLayout layout;
        Require(CalculateIndirectLayerLayout(1280, 720, layout), "indirect layout dimensions");
        Require(layout.stride == 0x1400, "indirect image stride");
        Require(layout.imageSize == 0x384000, "indirect image size");
        Require(layout.requiredSize == 0x3A0000, "indirect required memory size");
        Require(CalculateIndirectLayerLayout(17, 3, layout), "unaligned indirect dimensions");
        Require(layout.stride == 68 && layout.imageSize == 204 && layout.requiredSize == 0x20000,
                "indirect image layout remains linear");
        Require(!CalculateIndirectLayerLayout(0, 720, layout), "reject invalid indirect width");

        // Documented divergence: for 1280x720 Ryujinx's formula yields 0x3C0000, Strato (yuzu/Eden formula) 0x3A0000.
        Require(RyujinxReferenceRequiredSize(1280, 720) == 0x3C0000, "reference formula value for 1280x720");
        Require(RyujinxReferenceRequiredSize(1280, 720) - 0x3A0000 == 0x20000, "reference differs by exactly one 0x20000 granule");

        // Invariants every guest relies on, for many dimensions: the advertised buffer must hold the image Strato writes,
        // be granular, and not exceed the image by more than one granule. Also compare against the reference formula.
        unsigned differing{};
        for (const std::int64_t width : {1, 2, 15, 16, 17, 63, 64, 65, 127, 640, 854, 1024, 1280, 1366, 1600, 1920, 2560, 3840, 4096})
            for (const std::int64_t height : {1, 2, 3, 15, 16, 17, 63, 64, 65, 359, 360, 480, 540, 720, 721, 768, 1080, 1440, 2160, 4096}) {
                Require(CalculateIndirectLayerLayout(width, height, layout), "valid dimensions");
                Require(layout.stride == static_cast<std::uint64_t>(width) * 4, "stride is width*4");
                Require(layout.imageSize == layout.stride * static_cast<std::uint64_t>(height), "image size is stride*height");
                Require(layout.requiredSize >= layout.imageSize, "advertised size holds the image");
                Require(layout.requiredSize % IndirectLayerRequiredSizeAlignment == 0, "advertised size is granular");
                Require(layout.requiredSize - layout.imageSize < IndirectLayerRequiredSizeAlignment, "no more than one granule of slack");
                Require(IndirectLayerAlignment == 0x1000 && layout.requiredSize % IndirectLayerAlignment == 0, "advertised size is a multiple of the advertised alignment");
                const auto reference{RyujinxReferenceRequiredSize(static_cast<std::uint64_t>(width), static_cast<std::uint64_t>(height))};
                Require(reference >= layout.imageSize, "reference also holds the linear image");
                differing += reference != layout.requiredSize;
            }
        Require(differing != 0, "the two formulas are genuinely different for some sizes");

        // Overflow and range handling.
        constexpr auto Max{std::numeric_limits<std::int64_t>::max()};
        Require(!CalculateIndirectLayerLayout(-1, 720, layout) && !CalculateIndirectLayerLayout(1280, -1, layout), "negative dimensions");
        Require(!CalculateIndirectLayerLayout(1280, 0, layout), "zero height");
        Require(!CalculateIndirectLayerLayout(Max, 1, layout), "width*4 overflow");
        Require(!CalculateIndirectLayerLayout(Max / 4, 2, layout), "stride*height overflow");
        Require(!CalculateIndirectLayerLayout(1, Max, layout), "height*stride overflow");
        Require(!CalculateIndirectLayerLayout(Max / 4, 1, layout), "alignment round-up overflow");
        Require(!CalculateIndirectLayerLayout(std::numeric_limits<std::int64_t>::min(), 1, layout), "most negative width");
        Require(CalculateIndirectLayerLayout(1 << 15, 1 << 15, layout) && layout.imageSize == (1ULL << 32), "large but valid dimensions");
    }


}

int main() {
    TestFrontendQueue();
    TestText();
    TestConfig();
    TestState();
    TestInlineTextReplies();
    TestSessions();
    TestIndirectLayers();
    TestIndirectLayerLayout();
    std::cout << "PASS SWKBD serialization, normal state, frontend sessions, and indirect handles, layout invariants\n";
}
