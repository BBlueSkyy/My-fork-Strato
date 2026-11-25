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
#include <skyline/services/visrv/indirect_layer_ipc.h>
#include <cstdlib>
#include <cstdint>
#include <set>
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

// ---- AM -> VI -> SWKBD indirect-image flow (production accessor, registry, applet and VI helpers) ----
struct AlignedBuffer {
    u8 *base{};
    size_t size{};
    AlignedBuffer(size_t bytes, size_t alignment, u8 fill) : size{bytes} {
        base = static_cast<u8 *>(std::aligned_alloc(alignment, (bytes + alignment - 1) / alignment * alignment));
        Require(base != nullptr, "aligned allocation");
        std::memset(base, fill, bytes);
    }
    ~AlignedBuffer() { std::free(base); }
    AlignedBuffer(const AlignedBuffer &) = delete;
};
struct IndirectFlow {
    JvmManager jvm;
    kernel::type::KProcess process;
    DeviceState state{&jvm, &process};
    service::ServiceManager manager;
    kernel::type::KSession session;
    std::unique_ptr<service::am::ILibraryAppletAccessor> accessor;
    static constexpr u64 Aruid{0x99};
    explicit IndirectFlow(Mode mode, bool start = true) {
        manager.indirectLayers = std::make_shared<service::am::IndirectLayerRegistry>();
        accessor = std::make_unique<service::am::ILibraryAppletAccessor>(state, manager, skyline::applet::AppletId::LibraryAppletSwkbd, mode, Aruid);
        if (!start) return;
        Push(Bytes(service::applet::CommonArguments{1, 0x20, 0x8000D, 0, 0, 0}), false);
        Push(Bytes<u64>(0x0000010000000000ULL), false);
        Require(accessor->StartApplet().raw == 0, "start");
    }
    void Push(std::vector<u8> bytes, bool interactive) {
        ipc::IpcRequest request; ipc::IpcResponse response;
        request.storage = std::make_shared<service::am::VectorIStorage>(state, manager, std::move(bytes));
        Require((interactive ? accessor->PushInteractiveInData(session, request, response) : accessor->PushInData(session, request, response)).raw == 0, "push");
    }
    void Command(u32 id, std::vector<u8> argument = {}) {
        std::vector<u8> bytes(4 + argument.size()); Write(bytes, 0, id);
        if (!argument.empty()) std::memcpy(bytes.data() + 4, argument.data(), argument.size());
        Push(std::move(bytes), true);
    }
    // AM 160: raw data = u64 AppletResourceUserId (PID descriptor supplies request.pid); reply = u64 handle.
    Result Handle(u64 aruid, u64 &handle, u64 pid = 0x1234) {
        ipc::IpcRequest request; ipc::IpcResponse response;
        request.pid = pid; request.data = Bytes(aruid);
        const auto result{accessor->GetIndirectLayerConsumerHandle(session, request, response)};
        if (!result.raw) {
            Require(response.data.size() == sizeof(u64), "AM 160 reply is exactly one u64");
            std::memcpy(&handle, response.data.data(), sizeof(u64));
        }
        return result;
    }
    // VI 2460 through the shared production helpers; returns the serialised reply bytes.
    static Result MemoryInfo(std::int64_t width, std::int64_t height, std::vector<u8> &reply) {
        service::visrv::IndirectLayerLayout layout;
        ipc::IpcRequest request; request.data = Bytes(std::array<std::int64_t, 2>{width, height});
        const auto args{request.Pop<std::int64_t>()}, args2{request.Pop<std::int64_t>()};
        if (!service::visrv::CalculateIndirectLayerLayout(args, args2, layout))
            return service::visrv::result::InvalidDimensions;
        ipc::IpcResponse response;
        service::visrv::WriteIndirectLayerMemoryInfoReply(response, layout);
        reply = response.data;
        return {};
    }
    Result Map(u64 pid, std::int64_t width, std::int64_t height, u64 handle, u64 aruid, span<u8> buffer, ipc::IpcResponse &response) {
        service::visrv::IndirectLayerLayout layout;
        const auto result{service::visrv::MapIndirectLayerImage(*manager.indirectLayers, pid, width, height, handle, aruid, buffer, layout)};
        if (!result.raw) service::visrv::WriteIndirectLayerImageMapReply(response, layout);
        return result;
    }
};
std::int64_t ReplyField(const std::vector<u8> &reply, size_t index) { std::int64_t value{}; std::memcpy(&value, reply.data() + index * 8, 8); return value; }
void TestIndirectWireFormats() {
    // 2460: two s64 values, size first. Offsets are fixed by the IPC ABI, not by sizeof of any struct.
    std::vector<u8> reply;
    Require(IndirectFlow::MemoryInfo(1280, 720, reply).raw == 0 && reply.size() == 16, "2460 reply is two s64 fields");
    Require(ReplyField(reply, 0) == 0x3A0000 && ReplyField(reply, 1) == 0x1000, "2460 reply order: size then alignment");
    Require(IndirectFlow::MemoryInfo(0, 720, reply).raw == service::visrv::result::InvalidDimensions.raw, "2460 rejects zero width");
    Require(IndirectFlow::MemoryInfo(1280, -1, reply).raw == service::visrv::result::InvalidDimensions.raw, "2460 rejects negative height");
    // 2450 raw data: s64 width, s64 height, u64 handle, u64 ARUID.
    {
        ipc::IpcRequest request; request.data = Bytes(std::array<u64, 4>{1280, 720, 0xAABB, 0xCCDD});
        const auto args{service::visrv::ReadIndirectLayerImageMapArguments(request)};
        Require(args.width == 1280 && args.height == 720 && args.handle == 0xAABB && args.appletResourceUserId == 0xCCDD, "2450 argument order");
        Require(request.offset == 32, "2450 consumes exactly 32 bytes");
    }
    // 2451 raw data: four f32, then width, height, handle, ARUID (switchbrew: the last u64 is the ARUID).
    {
        std::vector<u8> data(16 + 32);
        const float crop[4]{0.0f, 0.0f, 1.0f, 1.0f}; std::memcpy(data.data(), crop, 16);
        const std::array<u64, 4> fields{1280, 720, 0xAABB, 0xCCDD}; std::memcpy(data.data() + 16, fields.data(), 32);
        ipc::IpcRequest request; request.data = data;
        const auto args{service::visrv::ReadIndirectLayerImageCropMapArguments(request)};
        Require(args.width == 1280 && args.height == 720 && args.handle == 0xAABB && args.appletResourceUserId == 0xCCDD, "2451 argument order");
        Require(request.offset == 48, "2451 consumes exactly 48 bytes");
    }
    // ImageMap reply: s64 image size, then s64 stride.
    {
        service::visrv::IndirectLayerLayout layout; Require(service::visrv::CalculateIndirectLayerLayout(1280, 720, layout), "layout");
        ipc::IpcResponse response; service::visrv::WriteIndirectLayerImageMapReply(response, layout);
        Require(response.data.size() == 16 && ReplyField(response.data, 0) == 0x384000 && ReplyField(response.data, 1) == 0x1400, "2450 reply order: size then stride");
    }
}
void TestIndirectFullFlow(std::int64_t width, std::int64_t height) {
    IndirectFlow f{Mode::PartialForegroundWithIndirectDisplay};
    // Observed Hitman order: V2 flags, AM 160, then the VI memory query, all before the first Calc.
    f.Command(0xD, {0}); f.Command(0xE, {0});
    u64 handle{}; Require(f.Handle(IndirectFlow::Aruid, handle).raw == 0 && handle != 0, "AM 160 yields a handle");
    u64 again{}; Require(f.Handle(IndirectFlow::Aruid, again).raw == 0 && again == handle, "AM 160 is stable while the layer lives");
    std::vector<u8> reply; Require(IndirectFlow::MemoryInfo(width, height, reply).raw == 0, "VI memory info");
    const auto required{static_cast<size_t>(ReplyField(reply, 0))}, alignment{static_cast<size_t>(ReplyField(reply, 1))};
    const size_t image{static_cast<size_t>(width) * 4 * static_cast<size_t>(height)};
    Require(alignment == 0x1000 && required >= image && required % 0x20000 == 0, "advertised memory covers the image and is granular");
    // The guest allocates exactly the advertised size at the advertised alignment, with a guard region around the image.
    AlignedBuffer buffer{required, alignment, 0xCC};
    ipc::IpcResponse response;
    Require(f.Map(0x1234, width, height, handle, IndirectFlow::Aruid, {buffer.base, buffer.size}, response).raw == 0, "ImageMap succeeds in advertised memory");
    Require(response.data.size() == 16 && static_cast<size_t>(ReplyField(response.data, 0)) == image && ReplyField(response.data, 1) == width * 4, "ImageMap reply matches layout");
    for (size_t i{}; i < image; i += (i < 64 || image - i < 64) ? 1 : 4093) Require(buffer.base[i] == 0, "image is fully written (blank)");
    for (size_t i{image}; i < required; i += (i < image + 64) ? 1 : 4093) Require(buffer.base[i] == 0xCC, "bytes past the image are untouched");
    // After the VI sequence, the inline protocol continues to work: Calc Initialize+Appear opens the frontend, Finalize completes.
    f.Command(0xA, Calc(0x4E8, 1 | 4));
    Require(f.jvm.opens == 1, "Calc/Appear still reaches the frontend after VI mapping");
    f.jvm.Event(FrontendEventType::FrontendOpened);
    f.Command(4);
    Require(f.accessor->IsAppletCompleted(), "Finalize completes after VI mapping");
}
void TestIndirectRejections() {
    IndirectFlow f{Mode::PartialForegroundWithIndirectDisplay};
    u64 handle{}; Require(f.Handle(IndirectFlow::Aruid, handle).raw == 0, "handle");
    const std::int64_t w{1280}, h{720}; const size_t image{1280 * 4 * 720}, required{0x3A0000};
    AlignedBuffer good{required, 0x1000, 0xCC};
    auto map = [&](u64 pid, std::int64_t width, std::int64_t height, u64 hdl, u64 aruid, u8 *data, size_t size) {
        ipc::IpcResponse response;
        const auto result{f.Map(pid, width, height, hdl, aruid, {data, size}, response)};
        Require(result.raw != 0 || response.data.size() == 16, "success serialises, failure does not");
        Require(result.raw == 0 || response.data.empty(), "failed requests never write a reply payload");
        return result.raw;
    };
    using namespace service::visrv;
    Require(map(0x1234, w, h, handle, IndirectFlow::Aruid, good.base, image - 1) == result::InvalidArgument.raw, "buffer one byte short");
    Require(map(0x1234, w, h, handle, IndirectFlow::Aruid, good.base, image) == 0, "buffer exactly the image size is enough for the image");
    Require(map(0x1234, w, h, handle, IndirectFlow::Aruid, good.base + 8, image) == result::InvalidArgument.raw, "misaligned buffer");
    Require(map(0x1234, w, h, handle, IndirectFlow::Aruid, nullptr, 0) == result::InvalidArgument.raw, "empty buffer");
    Require(map(0x1234, 0, h, handle, IndirectFlow::Aruid, good.base, required) == result::InvalidDimensions.raw, "zero width");
    Require(map(0x1234, w, -5, handle, IndirectFlow::Aruid, good.base, required) == result::InvalidDimensions.raw, "negative height");
    Require(map(0x1234, INT64_MAX, 2, handle, IndirectFlow::Aruid, good.base, required) == result::InvalidDimensions.raw, "width overflow");
    Require(map(0x1234, 1 << 20, INT64_MAX / 4, handle, IndirectFlow::Aruid, good.base, required) == result::InvalidDimensions.raw, "size overflow");
    Require(map(0x1235, w, h, handle, IndirectFlow::Aruid, good.base, required) == result::InvalidValue.raw, "foreign PID");
    Require(map(0x1234, w, h, handle, IndirectFlow::Aruid + 1, good.base, required) == result::InvalidValue.raw, "foreign ARUID");
    Require(map(0x1234, w, h, handle + 1, IndirectFlow::Aruid, good.base, required) == result::InvalidValue.raw, "unknown handle");
    // AM validation: wrong ARUID or missing PID are rejected, closing the layer invalidates the handle.
    u64 unused{};
    Require(f.Handle(IndirectFlow::Aruid + 1, unused).raw == service::am::result::ObjectInvalid.raw, "AM 160 foreign ARUID");
    Require(f.Handle(IndirectFlow::Aruid, unused, 0).raw == service::am::result::ObjectInvalid.raw, "AM 160 without PID");
    { ipc::IpcRequest request; ipc::IpcResponse response; f.accessor->RequestExit(f.session, request, response); }
    Require(map(0x1234, w, h, handle, IndirectFlow::Aruid, good.base, required) == result::InvalidValue.raw, "closed layer handle");
    Require(f.Handle(IndirectFlow::Aruid, unused).raw == service::am::result::ObjectInvalid.raw, "AM 160 after exit");
    // Before Start the applet has no image; the handle exists but the map reports NoData and writes nothing.
    IndirectFlow idle{Mode::PartialForegroundWithIndirectDisplay, false};
    u64 idleHandle{}; Require(idle.Handle(IndirectFlow::Aruid, idleHandle).raw == 0, "handle before start");
    AlignedBuffer untouched{required, 0x1000, 0xCC};
    ipc::IpcResponse response;
    Require(idle.Map(0x1234, w, h, idleHandle, IndirectFlow::Aruid, {untouched.base, untouched.size}, response).raw == result::NoData.raw && untouched.base[0] == 0xCC, "NoData before Start");
    // Other modes never expose an indirect layer.
    IndirectFlow normal{Mode::PartialForeground};
    Require(normal.Handle(IndirectFlow::Aruid, unused).raw == service::am::result::ObjectInvalid.raw, "AM 160 requires indirect display mode");
}
int main() {
    TestIndirectWireFormats();
    for (const auto &[w, h] : {std::pair<std::int64_t, std::int64_t>{1280, 720}, {1920, 1080}, {1, 1}, {17, 3}, {3840, 2160}, {1280, 721}, {1366, 768}, {64, 64}, {4096, 4096}})
        TestIndirectFullFlow(w, h);
    TestIndirectRejections();
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
    std::cout << "PASS indirect-layer AM/VI/SWKBD flow: wire formats, 9 dimension flows, rejection matrix\n";
    std::cout << "PASS production SWKBD applet: 24 Hitman V2 prefixes, 4 AM boundary prefixes, 6 inline layouts/modes, 24 guest-text reply sequences, malformed Calc and 3 normal flows\n";
}
