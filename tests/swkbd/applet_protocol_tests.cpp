// SPDX-License-Identifier: MPL-2.0
// Runs the real applet and real IApplet queue/event paths with host-only boundary stubs.
#include <cstdlib>
#include <iostream>
#include <jvm.h>
#include <skyline/applet/swkbd/software_keyboard_applet.h>
using namespace skyline;
using namespace skyline::applet::swkbd;
using Mode = service::applet::LibraryAppletMode;
void Require(bool value, const char *message) { if (!value) throw exception(message); }
template<typename T> void Write(std::vector<u8> &bytes, size_t offset, T value) { std::memcpy(bytes.data() + offset, &value, sizeof(value)); }
template<typename T> std::vector<u8> Bytes(const T &value) { std::vector<u8> bytes(sizeof(T)); std::memcpy(bytes.data(), &value, sizeof(T)); return bytes; }
u32 Read(span<u8> bytes, size_t offset) { u32 value{}; std::memcpy(&value, bytes.data() + offset, sizeof(value)); return value; }
struct Fixture {
    JvmManager jvm;
    DeviceState state{&jvm};
    service::ServiceManager manager;
    std::shared_ptr<kernel::type::KEvent> completed{std::make_shared<kernel::type::KEvent>()};
    std::shared_ptr<kernel::type::KEvent> normal{std::make_shared<kernel::type::KEvent>()};
    std::shared_ptr<kernel::type::KEvent> interactive{std::make_shared<kernel::type::KEvent>()};
    std::shared_ptr<SoftwareKeyboardApplet> applet;
    explicit Fixture(Mode mode) : applet{std::make_shared<SoftwareKeyboardApplet>(state, manager, completed, normal, interactive, mode)} {
        service::applet::CommonArguments common{};
        common.version = 1; common.size = sizeof(common); common.apiVersion = 0x8000D;
        Normal(Bytes(common));
    }
    void Normal(std::vector<u8> bytes) { applet->PushNormalDataToApplet(std::make_shared<service::am::VectorIStorage>(state, manager, std::move(bytes))); }
    void Request(u32 command, std::vector<u8> argument = {}) {
        std::vector<u8> bytes(4 + argument.size()); Write(bytes, 0, command);
        if (!argument.empty()) std::memcpy(bytes.data() + 4, argument.data(), argument.size());
        applet->PushInteractiveDataToApplet(std::make_shared<service::am::VectorIStorage>(state, manager, std::move(bytes)));
    }
    std::shared_ptr<service::am::IStorage> Reply(u32 code, u32 expectedState, size_t size = 8) {
        Require(interactive->signalled, "reply must signal interactive event");
        auto storage{applet->PopInteractiveAndClear()};
        Require(storage != nullptr && storage->GetSpan().size() == size, "reply storage size");
        Require(Read(storage->GetSpan(), 0) == expectedState && Read(storage->GetSpan(), 4) == code, "reply state/code");
        return storage;
    }
};
std::vector<u8> Calc(size_t size, u64 flags) {
    std::vector<u8> bytes(size);
    Write<u16>(bytes, 4, static_cast<u16>(size)); Write(bytes, 8, flags);
    const size_t appear{size == 0x4E8 ? 0x18U : 0x20U};
    Write<u32>(bytes, appear + 0x20, 14);
    return bytes;
}
void TestInline(Mode mode, size_t size) {
    Fixture f{mode};
    f.Normal(Bytes<u64>(mode == Mode::PartialForeground ? 0x10100000000ULL : 0x10000000000ULL));
    f.applet->Start();
    Require(f.jvm.opens == 0 && !f.interactive->signalled, "InitializeArg alone must not show editor");
    f.Request(0xA, Calc(size, 1 | 8)); // Initial text must not add an extra ChangedString to initialization.
    f.Reply(1, 1); Require(f.interactive->signalled, "first pop must retain event while another reply is queued");
    f.Reply(0, 1, 9); Require(!f.interactive->signalled, "last pop clears interactive event");
    f.Request(0xE, {1});
    f.Request(0xA, Calc(size, 4));
    f.Reply(1, 2);
    Require(f.jvm.opens == 1 && f.jvm.inlineMode, "Appear must request inline Android editor");
    f.jvm.Event(FrontendEventType::FrontendOpened);
    f.Reply(1, 3); Require(f.jvm.updates == 1, "opened acknowledgement updates cursor/text");
    f.jvm.Event(FrontendEventType::CursorMoved, u"AB", 1);
    f.Reply(0xE, 3, 8 + 0x3EC + 8 + 1);
    f.Request(0xA, Calc(size, 0x80));
    f.Reply(1, 4); f.Reply(1, 1);
    Require(f.jvm.hides == 1, "Disappear requests Android hide");
    f.Request(0xA, Calc(size, 4)); f.Reply(1, 2);
    Require(f.jvm.opens == 2, "consecutive Appear can reopen");
    f.jvm.Event(FrontendEventType::FrontendOpened); f.Reply(1, 3);
    f.jvm.Event(FrontendEventType::Submit, u"Name", 4);
    f.Reply(5, 3, 8 + 0x3EC + 4); f.Reply(1, 4); f.Reply(1, 1);
    f.Request(0xA, Calc(size, 4)); f.Reply(1, 2);
    f.jvm.Event(FrontendEventType::FrontendOpened); f.Reply(1, 3);
    f.jvm.Event(FrontendEventType::Cancel);
    f.Reply(6, 3); f.Reply(1, 4); f.Reply(1, 1);
    Require(f.jvm.hides == 3 && !f.completed->signalled, "submit/cancel hide inline UI without finalizing applet");
    f.Request(4); f.Reply(1, 0);
    Require(f.completed->signalled && f.jvm.closes == 1, "Finalize signals completion and closes session");
}
void TestMalformedCalc() {
    Fixture f{Mode::PartialForegroundWithIndirectDisplay};
    f.Normal(Bytes<u64>(0)); f.applet->Start();
    f.Request(0xA, std::vector<u8>(2));
    auto mismatch{Calc(0x4E8, 1)}; Write<u16>(mismatch, 4, 0x4C8);
    f.Request(0xA, mismatch); f.Request(0xA, Calc(0x4B0, 1));
    Require(f.jvm.opens == 0 && !f.interactive->signalled, "rejected Calc must not fabricate opens or replies");
    f.Request(0xA, Calc(0x4E8, 1)); f.Reply(1, 1); f.Reply(0, 1, 9);
}
void TestGuestTextReply(Mode mode, size_t size, bool utf8, bool v2) {
    Fixture f{mode};
    f.Normal(Bytes<u64>(mode == Mode::PartialForeground ? 0x10100000000ULL : 0x10000000000ULL));
    f.applet->Start();
    f.Request(0xA, Calc(size, 1)); f.Reply(1, 1); f.Reply(0, 1, 9);
    f.Request(0xD, {static_cast<u8>(v2)});
    auto input{Calc(size, 0x8 | 0x10 | 0x20)};
    const size_t inputOffset{size == 0x4A0 ? 0x68U : 0x90U};
    const std::u16string text{u"é😀"};
    std::memcpy(input.data() + inputOffset, text.data(), text.size() * sizeof(char16_t));
    Write<i32>(input, size == 0x4E8 ? 0x8C : 0x1C, 1);
    Write<u8>(input, size == 0x4A0 ? 0x45C : 0x484, utf8);
    const u32 expectedReply{utf8 ? (v2 ? 0xFU : 7U) : (v2 ? 0xDU : 2U)};
    const size_t replyBytes{8 + (utf8 ? 0x7D4U : 0x3ECU) + 16 + (v2 ? 1 : 0)};
    f.Request(0xA, input);
    const auto reply{f.Reply(expectedReply, 1, replyBytes)};
    const size_t textBytes{utf8 ? 0x7D4U : 0x3ECU};
    const auto decoded{ReadNullTerminatedText({reply->GetSpan().data() + 8, textBytes}, utf8 ? TextEncoding::Utf8 : TextEncoding::Utf16)};
    Require(decoded && *decoded == text, "guest text reply must contain the committed Unicode string");
    Require(Read(reply->GetSpan(), 8 + textBytes) == text.size() && Read(reply->GetSpan(), 8 + textBytes + 12) == 1,
            "guest text reply length and cursor remain UTF16 units");
    Require(!f.interactive->signalled && f.jvm.opens == 0, "hidden guest text gets exactly one reply without forcing an editor open");
    auto appear{input}; Write<u64>(appear, 8, 0x8 | 0x10 | 0x20 | 0x4);
    f.Request(0xA, appear); f.Reply(expectedReply, 1, replyBytes); f.Reply(1, 2);
    f.jvm.Event(FrontendEventType::FrontendOpened); f.Reply(1, 3);
    f.Request(0xA, input); f.Reply(expectedReply, 3, replyBytes);
    Require(!f.interactive->signalled && f.jvm.updates == 2, "shown guest text replies once and updates Android independently");
}
void TestNormal(bool check, bool cancel) {
    Fixture f{Mode::AllForeground};
    KeyboardConfigVB config{}; config.commonConfig.textMaxLength = 14; config.commonConfig.isUseTextCheck = check;
    f.Normal(Bytes(config)); f.applet->Start();
    Require(f.jvm.opens == 1 && !f.jvm.inlineMode, "normal Start requests Android editor without Calc");
    f.jvm.Event(FrontendEventType::FrontendOpened);
    f.jvm.Event(cancel ? FrontendEventType::Cancel : FrontendEventType::Submit, u"Name", 4);
    if (check && !cancel) {
        Require(!f.completed->signalled && f.interactive->signalled, "text-check waits without completing");
        auto validation{f.applet->PopInteractiveAndClear()};
        Require(validation && validation->GetSpan().size() == 0x7DC, "normal validation request size");
        std::vector<u8> response(0x7D8); // Success result and empty message.
        f.applet->PushInteractiveDataToApplet(std::make_shared<service::am::VectorIStorage>(f.state, f.manager, std::move(response)));
    }
    Require(f.completed->signalled && f.normal->signalled && f.jvm.closes == 1, "normal completes and signals both events");
    auto output{f.applet->PopNormalAndClear()};
    Require(output && output->GetSpan().size() == 0x7D8 && !f.normal->signalled, "normal output layout and queue reset");
    Require(Read(output->GetSpan(), 0) == static_cast<u32>(cancel ? CloseResult::Cancel : CloseResult::Enter), "normal submit/cancel result");
}
int main() {
    for (const auto mode : {Mode::PartialForeground, Mode::PartialForegroundWithIndirectDisplay})
        for (const size_t size : {0x4A0U, 0x4C8U, 0x4E8U}) {
            TestInline(mode, size);
            for (const bool utf8 : {false, true})
                for (const bool v2 : {false, true}) TestGuestTextReply(mode, size, utf8, v2);
        }
    TestMalformedCalc(); TestNormal(false, false); TestNormal(false, true); TestNormal(true, false);
    std::cout << "PASS production SWKBD applet: 6 inline layouts/modes, 24 guest-text reply sequences, malformed Calc and 3 normal flows\n";
}
