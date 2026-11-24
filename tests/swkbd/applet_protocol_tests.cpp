// SPDX-License-Identifier: MPL-2.0
// Runs the real applet and real IApplet queue/event paths with host-only boundary stubs.
#include <cstdlib>
#include <iostream>
#include <future>
#include <chrono>
#include <jvm.h>
#include <skyline/applet/swkbd/software_keyboard_applet.h>
#include <skyline/services/am/applet/ILibraryAppletAccessor.h>
#include <skyline/services/am/applet/indirect_layer_registry.h>
using namespace skyline;
using namespace skyline::applet::swkbd;
using Mode = service::applet::LibraryAppletMode;
std::shared_ptr<service::am::IApplet> skyline::applet::CreateApplet(const DeviceState &state, service::ServiceManager &manager,
        AppletId, std::shared_ptr<kernel::type::KEvent> completed, std::shared_ptr<kernel::type::KEvent> normal,
        std::shared_ptr<kernel::type::KEvent> interactive, Mode mode) {
    return std::make_shared<SoftwareKeyboardApplet>(state, manager, std::move(completed), std::move(normal), std::move(interactive), mode);
}
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
template<typename Function> void Responsive(Function function) {
    auto future{std::async(std::launch::async, std::move(function))};
    if (future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        std::cerr << "FAIL applet request/state-lock deadline exceeded\n";
        std::_Exit(EXIT_FAILURE); // Do not wait on a deadlocked async future during unwinding.
    }
    future.get();
}
void TestHitmanV2Prefix(bool changedV2, bool movedV2, size_t size, bool combinedInitializeAppear) {
    // Values confirmed by emulation.log. The flag bytes were not logged, so replay all 0/1 pairs.
    Fixture f{Mode::PartialForegroundWithIndirectDisplay}; // common version=1, size=0x20, API=0x8000D
    f.Normal(Bytes<u64>(0x0000010000000000ULL));
    f.applet->Start();
    auto images{std::async(std::launch::async, [&] {
        std::array<u8, 0x1000> image{};
        for (unsigned index{}; index < 32; ++index)
            Require(f.applet->GetIndirectLayerImage(image), "indirect-image callback remains responsive during negotiation");
    })};
    Responsive([&] {
        f.Request(0xD, {static_cast<u8>(changedV2)});
        f.Request(0xE, {static_cast<u8>(movedV2)});
    });
    if (images.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        std::cerr << "FAIL concurrent indirect image/state-lock deadline exceeded\n";
        std::_Exit(EXIT_FAILURE);
    }
    images.get();
    Require(f.jvm.opens == 0 && !f.interactive->signalled && !f.completed->signalled,
            "V2 negotiation must return without opening, initializing or fabricating a reply");
    Responsive([&] {
        f.Request(0xA, Calc(size, combinedInitializeAppear ? 1 | 4 : 1));
        f.Reply(1, 1); f.Reply(0, 1, 9);
        if (!combinedInitializeAppear) {
            Require(!f.interactive->signalled, "initialize replies drain normally after V2 prefix");
            f.Request(0xA, Calc(size, 4));
        }
        f.Reply(1, 2);
        Require(f.jvm.opens == 1, "Calc/Appear remains reachable after the exact observed prefix");
        f.jvm.Event(FrontendEventType::FrontendOpened); f.Reply(1, 3);
        f.jvm.Event(FrontendEventType::TextChanged, u"Name", 4);
        f.Reply(changedV2 ? 0xD : 2, 3, 8 + 0x3EC + 16 + (changedV2 ? 1 : 0));
        f.jvm.Event(FrontendEventType::CursorMoved, u"Name", 1);
        f.Reply(movedV2 ? 0xE : 3, 3, 8 + 0x3EC + 8 + (movedV2 ? 1 : 0));
        f.Request(0xD, {static_cast<u8>(!changedV2)});
        f.Request(0xE, {static_cast<u8>(!movedV2)});
        f.Request(0xD); f.Request(0xE); // Missing byte must not overwrite the negotiated flags.
        Require(!f.interactive->signalled, "re-negotiation/truncated flags do not fabricate acknowledgements");
        f.jvm.Event(FrontendEventType::TextChanged, u"Name", 4);
        f.Reply(!changedV2 ? 0xD : 2, 3, 8 + 0x3EC + 16 + (!changedV2 ? 1 : 0));
        f.jvm.Event(FrontendEventType::CursorMoved, u"Name", 1);
        f.Reply(!movedV2 ? 0xE : 3, 3, 8 + 0x3EC + 8 + (!movedV2 ? 1 : 0));
        f.Request(4); f.Reply(1, 0);
        Require(f.completed->signalled, "Finalize remains responsive after V2 prefix");
    });
}
void TestHitmanAccessorPrefix(bool changedV2, bool movedV2) {
    JvmManager jvm;
    kernel::type::KProcess process;
    DeviceState state{&jvm, &process};
    service::ServiceManager manager;
    manager.indirectLayers = std::make_shared<service::am::IndirectLayerRegistry>();
    service::am::ILibraryAppletAccessor accessor{state, manager, skyline::applet::AppletId::LibraryAppletSwkbd,
        Mode::PartialForegroundWithIndirectDisplay, 0x99};
    kernel::type::KSession session;
    auto input = [&](std::vector<u8> bytes, bool interactive) {
        ipc::IpcRequest request; ipc::IpcResponse response;
        request.storage = std::make_shared<service::am::VectorIStorage>(state, manager, std::move(bytes));
        const auto result{interactive ? accessor.PushInteractiveInData(session, request, response) : accessor.PushInData(session, request, response)};
        Require(result.raw == 0, "AM input handler must return success");
    };
    auto command = [&](u32 id, std::vector<u8> argument = {}) {
        std::vector<u8> bytes(4 + argument.size()); Write(bytes, 0, id);
        if (!argument.empty()) std::memcpy(bytes.data() + 4, argument.data(), argument.size());
        input(std::move(bytes), true);
    };
    auto pop = [&](u32 expectedCode, u32 expectedState) {
        ipc::IpcRequest request; ipc::IpcResponse response;
        Require(accessor.PopInteractiveOutData(session, request, response).raw == 0, "AM reply available");
        auto storage{std::static_pointer_cast<service::am::IStorage>(response.object)};
        Require(storage && Read(storage->GetSpan(), 0) == expectedState && Read(storage->GetSpan(), 4) == expectedCode,
                "AM returns the actual applet reply");
    };
    service::applet::CommonArguments common{1, 0x20, 0x8000D, 0, 0, 0};
    input(Bytes(common), false); input(Bytes<u64>(0x0000010000000000ULL), false);
    Require(accessor.StartApplet().raw == 0, "AM starts observed mode/version");
    Responsive([&] { command(0xD, {static_cast<u8>(changedV2)}); command(0xE, {static_cast<u8>(movedV2)}); });
    ipc::IpcRequest request; ipc::IpcResponse response;
    Require(accessor.PopInteractiveOutData(session, request, response).raw == service::am::result::NotAvailable.raw,
            "no fabricated reply for V2 flag commands at AM boundary");
    Require(jvm.opens == 0 && !accessor.IsAppletCompleted(), "V2 commands do not show or finish");
    // The referenced Hitman SDK accumulates Initialize and Appear before its first Calc.
    Responsive([&] { command(0xA, Calc(0x4E8, 1 | 4)); }); pop(1, 1); pop(0, 1); pop(1, 2);
    Require(jvm.opens == 1, "AM does not lose subsequent Calc/Appear");
    jvm.Event(FrontendEventType::FrontendOpened); pop(1, 3);
    Responsive([&] { command(4); }); pop(1, 0);
    Require(accessor.IsAppletCompleted(), "AM sees Finalize completion");
}
int main() {
    for (const bool changed : {false, true})
        for (const bool moved : {false, true})
            for (const size_t size : {0x4A0U, 0x4C8U, 0x4E8U})
                for (const bool combined : {false, true}) TestHitmanV2Prefix(changed, moved, size, combined);
    for (const bool changed : {false, true})
        for (const bool moved : {false, true}) TestHitmanAccessorPrefix(changed, moved);
    for (const auto mode : {Mode::PartialForeground, Mode::PartialForegroundWithIndirectDisplay})
        for (const size_t size : {0x4A0U, 0x4C8U, 0x4E8U}) {
            TestInline(mode, size);
            for (const bool utf8 : {false, true})
                for (const bool v2 : {false, true}) TestGuestTextReply(mode, size, utf8, v2);
        }
    TestMalformedCalc(); TestNormal(false, false); TestNormal(false, true); TestNormal(true, false);
    std::cout << "PASS production SWKBD applet: 24 Hitman V2 prefixes, 4 AM boundary prefixes, 6 inline layouts/modes, 24 guest-text reply sequences, malformed Calc and 3 normal flows\n";
}
