#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace skyline {
using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
struct exception : std::runtime_error {
    template<class... T> explicit exception(const char *message, T &&...) : std::runtime_error(message) {}
};
template<class T> struct span : std::span<T> {
    using std::span<T>::span;
    void copy_from(span other) { std::copy(other.begin(), other.end(), this->begin()); }
};
}
#include "dirty.inc"
using namespace skyline;

namespace vk {
using DeviceSize = u64;
using Buffer = u64;
using PipelineStageFlags = u32;
namespace PipelineStageFlagBits { constexpr u32 eVertexInput = 1; }
enum class IndexType { eUint8EXT, eUint16, eUint32 };
}
#include "quads.inc"
namespace conversion = skyline::gpu::interconnect::conversion;
namespace engine {
struct IndexBuffer {
    enum class IndexSize : u32 { OneByte, TwoBytes, FourBytes };
    IndexSize indexSize{IndexSize::TwoBytes};
    u32 padding{};
    u64 address{0x1000};
    u64 limit{0x1FFF};
};
}
struct Registers { engine::IndexBuffer indexBuffer; u32 padding{}; };
using DirtyManager = dirty::Manager<sizeof(Registers), 4>;
struct BufferBinding {
    vk::Buffer buffer{};
    vk::DeviceSize offset{}, size{};
    explicit operator bool() const { return buffer != 0; }
    bool operator==(const BufferBinding &) const = default;
};
struct FenceCycle {};
struct MegaBufferAllocator {
    struct Allocation { u64 buffer{80}, offset{}; std::span<u8> region; };
    std::vector<std::unique_ptr<std::vector<u8>>> allocations;
    Allocation Allocate(const std::shared_ptr<FenceCycle> &, size_t size) {
        allocations.push_back(std::make_unique<std::vector<u8>>(size));
        return {80, 0, *allocations.back()};
    }
};
struct Buffer {
    BufferBinding mega{10, 16, 12};
    std::array<u8, 4096> bytes{};
    void PopulateReadBarrier(u32, u32 &, u32 &) {}
};
struct BufferView {
    Buffer *buffer{};
    size_t size{};
    explicit operator bool() const { return buffer != nullptr; }
    Buffer *GetBuffer() const { return buffer; }
    BufferBinding TryMegaBuffer(const std::shared_ptr<FenceCycle> &, MegaBufferAllocator &, u64) const {
        auto binding = buffer->mega;
        binding.size = size;
        return binding;
    }
    std::span<u8> GetReadOnlyBackingSpan(bool, const std::function<void()> &) const { return buffer->bytes; }
};
struct GPU { MegaBufferAllocator megaBufferAllocator; Buffer source; bool mapped{true}; };
struct Executor {
    std::shared_ptr<FenceCycle> cycle{std::make_shared<FenceCycle>()};
    u64 executionTag{1};
    unsigned attaches{};
    void AttachBuffer(BufferView &) { ++attaches; }
};
struct InterconnectContext { Executor &executor; GPU &gpu; };
struct CachedMappedBufferView {
    BufferView view;
    static inline unsigned purges{};
    void Update(InterconnectContext &ctx, u64, u64 size, bool) {
        view = {ctx.gpu.mapped ? &ctx.gpu.source : nullptr, size};
    }
    void PurgeCaches() { view = {}; ++purges; }
    BufferView &operator*() { return view; }
    BufferView *operator->() { return &view; }
};
// Recording is represented by a binding sink. Buffer mapping, allocation and
// command scheduling are facades; all cache decisions below are production.
struct BindingSink {
    BufferBinding bound;
    vk::IndexType type{};
    unsigned binds{};
};
struct StateUpdateBuilder {
    BindingSink &sink;
    void SetIndexBuffer(BufferBinding binding, vk::IndexType type) {
        sink.bound = binding; sink.type = type; ++sink.binds;
    }
    void SetIndexBuffer(BufferView view, vk::IndexType type) {
        SetIndexBuffer({20, 0, view.size}, type);
    }
};
#define LOGD(...)
#define LOGE(...)
#include "index_class.inc"
#include "index_methods.inc"
struct ActiveState {
    dirty::ManualDirtyState<IndexBufferState> indexBuffer;
    explicit ActiveState(DirtyManager &manager, Registers &registers)
        : indexBuffer{manager, IndexBufferState::EngineRegisters{registers.indexBuffer}} {}
    void InvalidateIndexBuffer();
};
#include "invalidate.inc"

// Only command-header identities are facades here; the linked-list observer
// body is extracted unchanged from production StateUpdateBuilder.
struct StateUpdateCmdHeader {
    StateUpdateCmdHeader *next{};
    void (*record)(){};
};
struct SetIndexBufferCmd { static void Record() {} };
struct SetIndexBufferDynamicCmd { static void Record() {} };
struct UnrelatedCmd { static void Record() {} };
struct IndexCommandObserver {
    StateUpdateCmdHeader *head{};
    #include "observer.inc"
};

static unsigned failures{};
static void check(bool condition, const char *message) {
    std::cout << (condition ? "PASS " : "FAIL ") << message << '\n';
    if (!condition) ++failures;
}
struct Fixture {
    Registers registers;
    DirtyManager manager{registers};
    ActiveState state{manager, registers};
    GPU gpu;
    Executor executor;
    InterconnectContext ctx{executor, gpu};
    BindingSink sink;
    StateUpdateBuilder builder{sink};
    void draw(u32 count = 6) {
        u32 src{}, dst{};
        state.indexBuffer.Update(ctx, builder, src, dst, false, false, 0, count);
    }
};

static void overrideThenGuest(bool inlineOverride, bool directBacking) {
    Fixture fixture;
    if (directBacking) fixture.gpu.source.mega = {};
    fixture.draw();
    const auto correct = fixture.sink.bound;
    const auto correctType = fixture.sink.type;
    const BufferBinding synthetic{inlineOverride ? 91U : 90U, 0, 24};
    fixture.builder.SetIndexBuffer(synthetic, vk::IndexType::eUint32);
    const auto afterOverride = fixture.sink.binds;
    fixture.draw();
    check(fixture.sink.binds == afterOverride && fixture.sink.bound == synthetic,
          inlineOverride ? "negative control: cached guest draw retains inline override" :
                           "negative control: cached guest draw retains quad override");
    const auto purgesBeforeInvalidation = CachedMappedBufferView::purges;
    fixture.state.InvalidateIndexBuffer();
    check(CachedMappedBufferView::purges == purgesBeforeInvalidation,
          "index invalidation preserves mapped-view caches");
    fixture.draw();
    check(fixture.sink.bound == correct && fixture.sink.type == correctType && fixture.sink.binds == afterOverride + 1,
          inlineOverride ? "index invalidation restores guest binding after inline override" :
                           "index invalidation restores guest binding after quad override");
}

static void unchangedAndChangedGuest() {
    Fixture fixture;
    fixture.draw();
    const auto first = fixture.sink.binds;
    fixture.draw();
    check(fixture.sink.binds == first, "repeated unchanged guest draw adds no binding churn");
    fixture.gpu.source.mega = {11, 32, 12};
    fixture.draw();
    check(fixture.sink.binds == first + 1 && fixture.sink.bound.buffer == 11,
          "production Refresh rebinds a changed megabuffer allocation");
    fixture.registers.indexBuffer.indexSize = engine::IndexBuffer::IndexSize::FourBytes;
    fixture.manager.MarkDirty(offsetof(Registers, indexBuffer) / 4 + offsetof(engine::IndexBuffer, indexSize) / 4);
    fixture.draw();
    check(fixture.sink.type == vk::IndexType::eUint32 && fixture.sink.binds == first + 2,
          "production dirty-register tracking rebinds a changed guest index type");
    const auto purges = CachedMappedBufferView::purges;
    fixture.state.indexBuffer.MarkDirty(true);
    check(CachedMappedBufferView::purges == purges + 1,
          "production PurgeCaches delegates mapping purge through the dirty wrapper");
    fixture.draw();
    check(fixture.sink.binds == first + 3,
          "cache purge requires the next guest draw to bind again");
}

static void preparedBindingObserver() {
    #if INDEX_OBSERVER_AVAILABLE
    IndexCommandObserver observer;
    check(!observer.HasIndexBufferBinding(), "actual command observer rejects an empty update list");
    StateUpdateCmdHeader unrelated{nullptr, &UnrelatedCmd::Record};
    observer.head = &unrelated;
    check(!observer.HasIndexBufferBinding(), "actual command observer rejects unrelated state commands");
    StateUpdateCmdHeader direct{nullptr, &SetIndexBufferCmd::Record};
    unrelated.next = &direct;
    check(observer.HasIndexBufferBinding(), "actual command observer finds a static index bind after unrelated state");
    StateUpdateCmdHeader dynamic{nullptr, &SetIndexBufferDynamicCmd::Record};
    unrelated.next = &dynamic;
    check(observer.HasIndexBufferBinding(), "actual command observer finds a dynamic index bind after unrelated state");
    #endif
}

int main() {
    overrideThenGuest(false, false);
    overrideThenGuest(true, false);
    overrideThenGuest(false, true);
    overrideThenGuest(true, true);
    unchangedAndChangedGuest();
    preparedBindingObserver();
    return failures ? 1 : 0;
}
