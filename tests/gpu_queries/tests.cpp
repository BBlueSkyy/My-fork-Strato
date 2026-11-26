#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

using u8 = uint8_t;
using u32 = uint32_t;
using u64 = uint64_t;
using ContextTag = u64;
void Require(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error(message);
}

constexpr u32 VK_QUEUE_FAMILY_IGNORED = ~u32{};
constexpr u64 VK_WHOLE_SIZE = ~u64{};
namespace vk {
using Buffer = u64;
using QueryPool = u64;
using DescriptorSet = u64;
using DeviceSize = u64;
using RenderPass = u64;
using QueryControlFlags = u32;
using PipelineStageFlags = u32;
using AccessFlags = u32;
enum class QueryType { eOcclusion };
namespace QueryResultFlagBits { constexpr u32 e64 = 1, eWait = 2; }
namespace QueryControlFlagBits { constexpr u32 ePrecise = 1; }
namespace PipelineStageFlagBits {
constexpr u32 eTransfer = 1, eComputeShader = 2, eVertexShader = 4, eConditionalRenderingEXT = 8, eAllCommands = ~u32{};
}
namespace AccessFlagBits {
constexpr u32 eTransferRead = 1, eTransferWrite = 2, eShaderRead = 4, eShaderWrite = 8,
              eConditionalRenderingReadEXT = 16, eMemoryRead = 21, eMemoryWrite = 10;
}
enum class PipelineBindPoint { eCompute };
enum class ShaderStageFlagBits { eCompute };
struct QueryPoolCreateInfo { QueryType queryType; u32 queryCount; };
struct BufferCopy { u64 srcOffset, dstOffset, size; };
struct MemoryBarrier { u32 srcAccessMask, dstAccessMask; };
struct BufferMemoryBarrier {
    u32 srcAccessMask{}, dstAccessMask{};
    u32 srcQueueFamilyIndex{}, dstQueueFamilyIndex{};
    Buffer buffer{};
    u64 offset{}, size{};
};
template<class T> struct ArrayProxy { const T &value; };
namespace raii { class CommandBuffer; }
}

struct Storage { std::vector<u8> bytes; explicit Storage(size_t size) : bytes(size, 0xCD) {} };
static u64 nextHandle{1};
static std::map<u64, std::weak_ptr<Storage>> buffers;
static std::shared_ptr<Storage> Get(vk::Buffer buffer) {
    auto storage = buffers.at(buffer).lock();
    Require(bool(storage), "deferred recording referenced a destroyed buffer");
    return storage;
}
static u64 Read(vk::Buffer buffer, u64 offset = 0, size_t size = 8) {
    auto storage = Get(buffer);
    Require(offset + size <= storage->bytes.size(), "read outside buffer");
    u64 value{};
    std::memcpy(&value, storage->bytes.data() + offset, size);
    return value;
}
static void Write(vk::Buffer buffer, u64 offset, u64 value, size_t size = 8) {
    auto storage = Get(buffer);
    Require(offset + size <= storage->bytes.size(), "write outside buffer");
    std::memcpy(storage->bytes.data() + offset, &value, size);
}
namespace skyline::gpu {
namespace memory {
struct Buffer {
    std::shared_ptr<Storage> storage;
    vk::Buffer vkBuffer{};
    explicit Buffer(size_t size) : storage(std::make_shared<Storage>(size)), vkBuffer(nextHandle++) { buffers[vkBuffer] = storage; }
    size_t size_bytes() const { return storage->bytes.size(); }
};
}
struct FenceCycle {
    bool signalled{};
    std::vector<std::shared_ptr<void>> dependencies;
    bool Poll(bool) { return signalled; }
};
struct GPU;
struct DescriptorAllocator {
    struct ActiveDescriptorSet {
        vk::DescriptorSet handle;
        vk::DescriptorSet operator*() const { return handle; }
    };
};
struct BufferBinding {
    vk::Buffer buffer{};
    u64 offset{};
    explicit operator bool() const { return buffer != 0; }
};
struct GuestBuffer {
    memory::Buffer allocation{64};
    bool blocked{}, dirty{};
    void BlockSequencedCpuBackingWrites() { blocked = true; }
    void MarkGpuDirty(int &) { dirty = true; }
};
struct BufferView {
    std::shared_ptr<GuestBuffer> buffer;
    u64 offset{};
    BufferBinding GetBinding(GPU &) const { return {buffer->allocation.vkBuffer, offset}; }
    GuestBuffer *GetBuffer() { return buffer.get(); }
};
namespace interconnect { struct InterconnectContext; }
struct CachedMappedBufferView {
    std::optional<BufferView> view;
    void Update(interconnect::InterconnectContext &, u64 address, u32 size);
    BufferView &operator*() { return *view; }
    BufferView *operator->() { return &*view; }
    void PurgeCaches() { view.reset(); }
};
namespace shaders {
struct QueryResolveHelperShader {
    struct PushConstants { u32 firstIndex, count, resetAccumulator; };
    std::shared_ptr<int> pipeline{std::make_shared<int>(1)}, pipelineLayout{std::make_shared<int>(2)};
    DescriptorAllocator::ActiveDescriptorSet CreateDescriptorSet(GPU &, vk::Buffer results, u64, vk::Buffer accumulator);
    void Resolve(vk::raii::CommandBuffer &, vk::DescriptorSet, u32, u32, bool);
    void PrepareForTransfer(vk::raii::CommandBuffer &);
};
}
struct GPU {
    int vkDevice{};
    struct { bool supportsOcclusionQueryPrecise{true}; } traits;
    struct { memory::Buffer AllocateBuffer(size_t size) { return memory::Buffer(size); } } memory;
    struct { shaders::QueryResolveHelperShader queryResolveHelperShader; } helperShaders;
    struct Mega {
        template<class Bytes> BufferBinding Push(const std::shared_ptr<FenceCycle> &cycle, Bytes bytes) {
            auto allocation = std::make_shared<memory::Buffer>(bytes.size());
            std::memcpy(allocation->storage->bytes.data(), bytes.data(), bytes.size());
            cycle->dependencies.emplace_back(allocation);
            auto binding = BufferBinding{allocation->vkBuffer, 0};
            return binding;
        }
    } megaBufferAllocator;
};
}
namespace skyline::soc::gm20b { using IOVA = u64; }
namespace skyline {
template<class T> struct span {
    T *pointer;
    size_t count;
    explicit span(T &value) : pointer(&value), count(1) {}
    span(T *pointer, size_t count) : pointer(pointer), count(count) {}
    template<class U> span<U> cast() const { return {reinterpret_cast<U *>(pointer), count * sizeof(T) / sizeof(U)}; }
    size_t size() const { return count; }
    T *data() const { return pointer; }
};
}
// GLSL arithmetic functions compile unchanged with host implementations of their
// scalar/vector types and storage declarations. This tests the real carry code;
// it does not compile SPIR-V or validate driver shader execution.
namespace query_shader {
using uint = u32;
struct uvec2 {
    uint x, y;
    uvec2() : x(0), y(0) {}
    explicit uvec2(uint both) : x(both), y(both) {}
    uvec2(uint x, uint y) : x(x), y(y) {}
};
struct { std::vector<uvec2> values; } queryResults;
struct { uvec2 value; } accumulator;
skyline::gpu::shaders::QueryResolveHelperShader::PushConstants pc;
#define main Run
#include "shader.inc"
#undef main
}
struct PoolStorage { std::vector<u64> queries; std::vector<bool> ended; explicit PoolStorage(u32 size) : queries(size), ended(size) {} };
static std::map<u64, std::weak_ptr<PoolStorage>> pools;
static std::map<u64, std::pair<vk::Buffer, vk::Buffer>> descriptors;
namespace vk::raii {
struct QueryPool {
    u64 handle;
    std::shared_ptr<PoolStorage> storage;
    QueryPool(int, QueryPoolCreateInfo info) : handle(nextHandle++), storage(std::make_shared<PoolStorage>(info.queryCount)) { pools[handle] = storage; }
    u64 operator*() const { return handle; }
};
struct CommandBuffer {
    struct Access { u64 serial; u32 stage, access; u64 offset, size; };
    struct Barrier { u64 serial; u32 srcStage, dstStage, srcAccess, dstAccess; Buffer buffer; u64 offset, size; };
    std::map<Buffer, std::vector<Access>> accesses;
    std::vector<Barrier> barriers;
    std::vector<std::string> events;
    std::optional<std::pair<u64, u32>> active;
    u64 serial{};
    bool inside{};
    bool validateHazards{};
    u32 lastQueryFlags{};
    DescriptorSet descriptor{};
    skyline::gpu::shaders::QueryResolveHelperShader::PushConstants constants{};
    void AccessBuffer(Buffer buffer, u32 stage, u32 access, u64 offset = 0, u64 size = VK_WHOLE_SIZE) {
        auto storage = Get(buffer);
        if (size == VK_WHOLE_SIZE) size = storage->bytes.size() - offset;
        constexpr u32 writes = AccessFlagBits::eTransferWrite | AccessFlagBits::eShaderWrite;
        if (validateHazards) for (auto &previous : accesses[buffer]) {
            auto begin = std::max(offset, previous.offset);
            auto end = std::min(offset + size, previous.offset + previous.size);
            if (begin >= end || !((access | previous.access) & writes)) continue;
            bool ordered{};
            for (auto &barrier : barriers) {
                if (barrier.serial <= previous.serial ||
                    (barrier.srcStage & previous.stage) != previous.stage ||
                    (barrier.dstStage & stage) != stage) continue;
                if (barrier.buffer && (barrier.buffer != buffer || barrier.offset > begin ||
                    (barrier.size != VK_WHOLE_SIZE && barrier.offset + barrier.size < end))) continue;
                // WAR needs an execution dependency. A prior write also needs
                // availability and visibility matching the actual access masks.
                auto previousWrites = previous.access & writes;
                if (!previousWrites || ((barrier.srcAccess & previousWrites) == previousWrites &&
                                        (barrier.dstAccess & access) == access)) ordered = true;
            }
            Require(ordered, "buffer access lacks an intervening matching barrier");
        }
        accesses[buffer].push_back({++serial, stage, access, offset, size});
    }
    void resetQueryPool(u64 pool, u32 first, u32 count) {
        Require(!inside, "query reset inside render pass");
        auto storage = pools.at(pool).lock(); Require(bool(storage), "destroyed query pool reset");
        for (u32 i = first; i < first + count; i++) { storage->queries.at(i) = 0; storage->ended.at(i) = false; }
        events.emplace_back("reset:" + std::to_string(pool));
    }
    void beginQuery(u64 pool, u32 index, QueryControlFlags flags) {
        Require(inside && !active, "beginQuery must be inside RP with no active query");
        Require(bool(pools.at(pool).lock()), "destroyed query pool begun");
        Require(flags == QueryControlFlagBits::ePrecise || flags == 0, "invalid query flags");
        lastQueryFlags = flags;
        active = {{pool, index}}; events.emplace_back("begin");
    }
    void Draw(u64 samples) {
        Require(inside, "draw outside RP");
        if (active) pools.at(active->first).lock()->queries.at(active->second) += samples;
    }
    void endQuery(u64 pool, u32 index) {
        Require(inside && active == std::make_pair(pool, index), "query ended outside its active render pass");
        pools.at(pool).lock()->ended.at(index) = true;
        active.reset(); events.emplace_back("end");
    }
    void copyQueryPoolResults(u64 pool, u32 first, u32 count, Buffer destination, u64 offset, u64 stride, u32 flags) {
        Require(!inside, "query results copied inside RP");
        Require(flags == (QueryResultFlagBits::e64 | QueryResultFlagBits::eWait), "query result flags require 64-bit WAIT");
        auto storage = pools.at(pool).lock(); Require(bool(storage), "destroyed query pool copied");
        AccessBuffer(destination, PipelineStageFlagBits::eTransfer, AccessFlagBits::eTransferWrite, offset, count * stride);
        for (u32 i = 0; i < count; i++) { Require(storage->ended.at(first + i), "query copied before end"); Write(destination, offset + i * stride, storage->queries.at(first + i)); }
        events.emplace_back("results");
    }
    void fillBuffer(Buffer buffer, u64 offset, u64 size, u32 value) {
        Require(!inside, "fill inside RP");
        AccessBuffer(buffer, PipelineStageFlagBits::eTransfer, AccessFlagBits::eTransferWrite, offset, size);
        auto storage = Get(buffer);
        for (u64 i = offset; i < offset + size; i += 4) std::memcpy(storage->bytes.data() + i, &value, 4);
        events.emplace_back("fill");
    }
    void pipelineBarrier(u32 src, u32 dst, u32, MemoryBarrier barrier, std::initializer_list<int>, std::initializer_list<int>) {
        barriers.push_back({++serial, src, dst, barrier.srcAccessMask, barrier.dstAccessMask, 0, 0, VK_WHOLE_SIZE});
    }
    void pipelineBarrier(u32 src, u32 dst, u32, std::initializer_list<MemoryBarrier>, BufferMemoryBarrier barrier, std::initializer_list<int>) {
        Require(barrier.srcQueueFamilyIndex == barrier.dstQueueFamilyIndex, "unexpected queue ownership transfer");
        barriers.push_back({++serial, src, dst, barrier.srcAccessMask, barrier.dstAccessMask, barrier.buffer, barrier.offset, barrier.size});
    }
    void copyBuffer(Buffer source, Buffer destination, BufferCopy region) {
        Require(!inside, "copy inside RP");
        AccessBuffer(source, PipelineStageFlagBits::eTransfer, AccessFlagBits::eTransferRead, region.srcOffset, region.size);
        AccessBuffer(destination, PipelineStageFlagBits::eTransfer, AccessFlagBits::eTransferWrite, region.dstOffset, region.size);
        auto src = Get(source), dst = Get(destination);
        std::memcpy(dst->bytes.data() + region.dstOffset, src->bytes.data() + region.srcOffset, region.size);
        events.emplace_back("report");
    }
    void bindPipeline(PipelineBindPoint, int) {}
    void bindDescriptorSets(PipelineBindPoint, int, u32, DescriptorSet set, std::nullptr_t) { descriptor = set; }
    void pushConstants(int, ShaderStageFlagBits, u32, ArrayProxy<const skyline::gpu::shaders::QueryResolveHelperShader::PushConstants> values) { constants = values.value; }
    void dispatch(u32 x, u32 y, u32 z) {
        Require(x == 1 && y == 1 && z == 1 && !inside, "resolve dispatch dimensions/scope");
        auto [results, accumulator] = descriptors.at(descriptor);
        AccessBuffer(results, PipelineStageFlagBits::eComputeShader, AccessFlagBits::eShaderRead);
        AccessBuffer(accumulator, PipelineStageFlagBits::eComputeShader, AccessFlagBits::eShaderRead | AccessFlagBits::eShaderWrite);
        query_shader::pc = constants;
        auto initial = Read(accumulator);
        query_shader::accumulator.value = {u32(initial), u32(initial >> 32)};
        query_shader::queryResults.values.resize(Get(results)->bytes.size() / sizeof(u64));
        for (u32 i = 0; i < query_shader::queryResults.values.size(); i++) {
            auto value = Read(results, i * sizeof(u64));
            query_shader::queryResults.values[i] = {u32(value), u32(value >> 32)};
        }
        query_shader::Run();
        auto total = query_shader::accumulator.value;
        Write(accumulator, 0, u64(total.x) | (u64(total.y) << 32));
        events.emplace_back("resolve");
    }
};
}
namespace skyline::gpu::shaders {
DescriptorAllocator::ActiveDescriptorSet QueryResolveHelperShader::CreateDescriptorSet(GPU &, vk::Buffer results, u64, vk::Buffer accumulator) {
    u64 handle = nextHandle++; descriptors[handle] = {results, accumulator}; return {handle};
}
#include "resolve.inc"
}
namespace skyline::gpu::interconnect {
struct HostAllocator {
    std::vector<std::shared_ptr<void>> allocations;
    template<class T> T *EmplaceUntracked(T value) { auto item = std::make_shared<T>(std::move(value)); auto pointer = item.get(); allocations.emplace_back(item); return pointer; }
};
struct CommandExecutor {
    using CommandFunction = std::function<void(vk::raii::CommandBuffer &, const std::shared_ptr<FenceCycle> &, GPU &)>;
    using SubpassFunction = std::function<void(vk::raii::CommandBuffer &, const std::shared_ptr<FenceCycle> &, GPU &, vk::RenderPass, u32)>;
    struct SubpassHooks { SubpassFunction before; CommandFunction beforeRenderPassEnd, afterRenderPass; };
    GPU &gpu;
    ContextTag executionTag{1};
    std::shared_ptr<FenceCycle> cycle{std::make_shared<FenceCycle>()};
    std::shared_ptr<HostAllocator> allocator{std::make_shared<HostAllocator>()};
    int usageTracker{};
    bool renderPass{};
    u32 renderPassIndex{};
    std::vector<CommandFunction> nodes, post, beforeEnd;
    explicit CommandExecutor(GPU &gpu) : gpu(gpu) {}
    void AttachDependency(const std::shared_ptr<void> &resource) { cycle->dependencies.emplace_back(resource); }
    void AttachBuffer(BufferView view) { AttachDependency(view.buffer); }
    void BreakRenderPass() { FinishRenderPass(); }
    void AddCommand(CommandFunction function) { nodes.emplace_back(std::move(function)); }
    void InsertPreExecuteCommand(CommandFunction function) { nodes.insert(nodes.begin(), std::move(function)); }
    void InsertPostRpCommand(CommandFunction function) { post.emplace_back(std::move(function)); }
    void AddPostRpCommand(CommandFunction function) { (renderPass ? post : nodes).emplace_back(std::move(function)); }
    void BeginRenderPass() {
        Require(!renderPass, "fixture nested render pass"); renderPass = true;
        AddCommand([](auto &cb, const auto &, auto &) { Require(!cb.inside, "nested RP recording"); cb.inside = true; cb.events.emplace_back("rp begin"); });
    }
    void ApplyHooks(SubpassHooks hooks) {
        if (hooks.before) AddCommand([function = std::move(hooks.before)](auto &cb, const auto &cycle, auto &gpu) { function(cb, cycle, gpu, 1, 0); });
        if (hooks.beforeRenderPassEnd) beforeEnd.emplace_back(std::move(hooks.beforeRenderPassEnd));
        if (hooks.afterRenderPass) post.emplace_back(std::move(hooks.afterRenderPass));
    }
    void FinishRenderPass() {
        if (!renderPass) return;
        for (auto &function : beforeEnd) AddCommand(std::move(function));
        beforeEnd.clear();
        AddCommand([](auto &cb, const auto &, auto &) { Require(!cb.active, "query still active at RP exit"); cb.inside = false; cb.events.emplace_back("rp end"); });
        for (auto &function : post) AddCommand(std::move(function));
        post.clear();
        renderPass = false; renderPassIndex++;
    }
    void Record(vk::raii::CommandBuffer &cb) {
        Require(!renderPass, "unfinished RP at recording");
        for (auto &function : nodes) function(cb, cycle, gpu);
        for (auto &function : post) function(cb, cycle, gpu);
        nodes.clear(); post.clear(); beforeEnd.clear();
    }
    void NextExecution() { Require(nodes.empty() && post.empty(), "unrecorded execution"); cycle = std::make_shared<FenceCycle>(); allocator = std::make_shared<HostAllocator>(); executionTag++; }
};
struct InterconnectContext { GPU &gpu; CommandExecutor &executor; std::map<u64, BufferView> guest; };
}
void skyline::gpu::CachedMappedBufferView::Update(interconnect::InterconnectContext &ctx, u64 address, u32 size) {
    auto found = ctx.guest.find(address);
    if (found == ctx.guest.end()) { view.reset(); return; }
    view = found->second;
    Require(view->offset + size <= view->buffer->allocation.size_bytes(), "guest report range");
}
#include "queries_header.inc"
#include "queries_source.inc"

using namespace skyline::gpu;
using namespace skyline::gpu::interconnect;
using skyline::gpu::interconnect::maxwell3d::Queries;
struct Fixture {
    GPU gpu;
    CommandExecutor executor{gpu};
    InterconnectContext ctx{gpu, executor, {}};
    const vk::Buffer accumulatorHandle = nextHandle;
    std::unique_ptr<Queries> queries{std::make_unique<Queries>(gpu)};
    vk::raii::CommandBuffer cb;
    static constexpr auto counter = Queries::CounterType::Occulusion;
    BufferView Target(u64 address) { auto view = BufferView{std::make_shared<GuestBuffer>(), 0}; ctx.guest[address] = view; return view; }
    void Draw(bool enabled, u64 samples) {
        if (!executor.renderPass) executor.BeginRenderPass();
        executor.ApplyHooks(queries->PrepareDraw(ctx, counter, enabled, executor.renderPassIndex));
        executor.AddCommand([samples](auto &cb, const auto &, auto &) { cb.Draw(samples); });
    }
    void End() { queries->Pause(ctx); executor.FinishRenderPass(); }
    void Report(u64 address, std::optional<u64> timestamp = {}) { queries->Query(ctx, address, counter, timestamp); }
    void Run() { executor.Record(cb); }
};

void toggles() {
    Fixture f; auto result = f.Target(0x1000);
    f.Draw(true, 4); f.Draw(false, 100); f.Draw(true, 7); f.End(); f.Report(0x1000); f.Run();
    Require(Read(result.buffer->allocation.vkBuffer, 0, 4) == 11, "disabled draws contributed to count");
    Require(std::count(f.cb.events.begin(), f.cb.events.end(), "begin") == 2, "toggle must begin two query segments");
}
void optional_precise_feature() {
    for (bool precise : {false, true}) {
        Fixture f;
        f.gpu.traits.supportsOcclusionQueryPrecise = precise;
        f.Target(0x1000);
        f.Draw(true, 1); f.End(); f.Report(0x1000); f.Run();
        Require(f.cb.lastQueryFlags == (precise ? vk::QueryControlFlagBits::ePrecise : 0),
                "precise query flag must follow feature enablement");
    }
}
void render_passes_and_reports() {
    Fixture f; auto first = f.Target(0x1000), second = f.Target(0x2000), repeated = f.Target(0x3000);
    f.Draw(true, 13); f.End(); f.Draw(true, 5); f.End(); f.Report(0x1000); f.Report(0x3000);
    f.queries->ResetCounter(f.ctx, Fixture::counter); f.Draw(true, 3); f.End(); f.Report(0x2000); f.Run();
    Require(Read(first.buffer->allocation.vkBuffer, 0, 4) == 18, "counts lost across render passes");
    Require(Read(repeated.buffer->allocation.vkBuffer, 0, 4) == 18, "repeated report reset accumulated count");
    Require(Read(second.buffer->allocation.vkBuffer, 0, 4) == 3, "reset retained previous count");
    Require(first.buffer->blocked && first.buffer->dirty, "report buffer coherency omitted");
}
void reset_and_reports_inside_render_pass() {
    Fixture f; auto first = f.Target(0x1000), second = f.Target(0x2000);
    f.Draw(true, 99); f.queries->ResetCounter(f.ctx, Fixture::counter);
    f.Draw(true, 3); f.Report(0x1000);
    f.Draw(true, 4); f.Report(0x2000); f.End(); f.Run();
    Require(Read(first.buffer->allocation.vkBuffer, 0, 4) == 3, "reset included discarded pending segments");
    Require(Read(second.buffer->allocation.vkBuffer, 0, 4) == 7, "in-RP report broke continued accumulation");
}
void report_before_subsequent_command() {
    Fixture f; auto result = f.Target(0x1000);
    f.Draw(true, 9); f.Report(0x1000);
    f.executor.AddCommand([buffer = result.buffer->allocation.vkBuffer](auto &, const auto &, auto &) {
        Require(Read(buffer, 0, 4) == 9, "a command after Query observed the old report destination");
    });
    f.Draw(true, 4); f.End(); f.Run();
}
void overwrite_report_after_graphics_consumer() {
    Fixture f; f.cb.validateHazards = true; auto result = f.Target(0x1000);
    result.offset = 16; f.ctx.guest[0x1000] = result;
    f.executor.AddCommand([buffer = result.buffer->allocation.vkBuffer](auto &cb, const auto &, auto &) {
        cb.AccessBuffer(buffer, vk::PipelineStageFlagBits::eAllCommands, vk::AccessFlagBits::eMemoryRead, 28, 4);
        (void)Read(buffer, 28, 4);
    });
    f.Report(0x1000, 19); f.Run();
}
void report_visible_to_later_consumer() {
    Fixture f; f.cb.validateHazards = true; auto result = f.Target(0x1000);
    result.offset = 16; f.ctx.guest[0x1000] = result;
    f.Draw(true, 17); f.Report(0x1000, 31);
    f.executor.AddCommand([buffer = result.buffer->allocation.vkBuffer](auto &cb, const auto &, auto &) {
        cb.AccessBuffer(buffer, vk::PipelineStageFlagBits::eAllCommands, vk::AccessFlagBits::eMemoryRead, 16, 16);
        Require(Read(buffer, 16) == 17, "report not visible to a later consumer");
        Require(Read(buffer, 24) == 31, "report timestamp not visible to a later consumer");
    });
    f.End(); f.Run();
}
void report_width_and_carry() {
    Fixture f; auto narrow = f.Target(0x1000), wide = f.Target(0x2000);
    constexpr u64 timestamp = 0xFEDCBA9876543210ULL;
    f.Draw(true, 0xFFFFFFFFULL); f.Draw(false, 99); f.Draw(true, 3); f.End();
    f.Report(0x1000); f.Report(0x2000, timestamp); f.Run();
    Require(Read(narrow.buffer->allocation.vkBuffer, 0, 4) == 2, "32-bit report did not truncate");
    Require(Read(narrow.buffer->allocation.vkBuffer, 4, 4) == 0xCDCDCDCD, "32-bit report overwrote following bytes");
    Require(Read(wide.buffer->allocation.vkBuffer) == 0x100000002ULL, "64-bit report lost low-word carry");
    Require(Read(wide.buffer->allocation.vkBuffer, 8) == timestamp, "timestamp misplaced");
}
void pool_rollover() {
    Fixture f; auto result = f.Target(0x1000);
    for (u32 i = 0; i < 1027; i++) { f.Draw(true, 1); f.Draw(false, 0); }
    f.End(); f.Report(0x1000); f.Run();
    Require(Read(result.buffer->allocation.vkBuffer, 0, 4) == 1027, "query bank rollover lost counts");
    Require(std::count_if(f.cb.events.begin(), f.cb.events.end(), [](auto &event) { return event.starts_with("reset:"); }) == 2, "rollover did not use two banks");
}
void bank_fences() {
    Fixture f; auto result = f.Target(0x1000);
    f.Draw(true, 2); f.End(); f.queries->Flush(f.ctx); auto firstCycle = f.executor.cycle; f.Run();
    auto firstReset = *std::find_if(f.cb.events.begin(), f.cb.events.end(), [](auto &event) { return event.starts_with("reset:"); });
    f.cb.events.clear(); f.executor.NextExecution();
    f.Draw(true, 3); f.End(); f.queries->Flush(f.ctx); auto secondCycle = f.executor.cycle; f.Run();
    auto secondReset = *std::find_if(f.cb.events.begin(), f.cb.events.end(), [](auto &event) { return event.starts_with("reset:"); });
    Require(firstReset != secondReset, "unsignalled bank reused by another execution");
    firstCycle->signalled = true; f.cb.events.clear(); f.executor.NextExecution();
    f.Draw(true, 5); f.End(); f.Report(0x1000); f.Run();
    Require(std::find(f.cb.events.begin(), f.cb.events.end(), firstReset) != f.cb.events.end(), "signalled bank was not reused");
    Require(Read(result.buffer->allocation.vkBuffer, 0, 4) == 10, "accumulator did not persist across executions");
}
void empty_reset_barrier() {
    Fixture f; f.cb.validateHazards = true;
    f.Target(0x1000); f.Target(0x2000);
    f.Report(0x1000); f.queries->ResetCounter(f.ctx, Fixture::counter); f.Report(0x2000); f.Run();
}
void compute_then_empty_reset_barrier() {
    Fixture f; f.cb.validateHazards = true; f.Target(0x1000);
    f.Draw(true, 7); f.End(); f.queries->Flush(f.ctx);
    f.queries->ResetCounter(f.ctx, Fixture::counter); f.Report(0x1000); f.Run();
}
void repeated_report_barrier() {
    Fixture f; f.cb.validateHazards = true;
    auto result = f.Target(0x1000);
    f.Report(0x1000); f.Report(0x1000); f.Report(0x1000, 17); f.Report(0x1000, 21); f.Run();
    Require(Read(result.buffer->allocation.vkBuffer, 0, 4) == 0, "empty report value");
}
void destroyed_before_recording() {
    Fixture f; auto result = f.Target(0x1000);
    f.Draw(true, 7); f.End(); f.Report(0x1000); f.queries.reset();
    // Handles deliberately carry no ownership. Check before recording so the old
    // raw-this callback fails deterministically without invoking undefined behavior.
    for (auto &[descriptor, bindings] : descriptors) {
        if (!buffers.at(bindings.first).expired())
            Require(!buffers.at(bindings.second).expired(), "accumulator freed while a scheduled query bank still references it");
    }
    f.Run(); Require(Read(result.buffer->allocation.vkBuffer, 0, 4) == 7, "destroyed Queries lost deferred report");
    Require(!buffers.at(f.accumulatorHandle).expired(), "recorded accumulator not retained for the in-flight cycle");
}
void empty_destroyed_before_recording() {
    Fixture f; auto result = f.Target(0x1000); f.Report(0x1000);
    f.queries.reset();
    Require(!buffers.at(f.accumulatorHandle).expired(), "empty deferred reset/report did not retain accumulator");
    f.Run(); Require(Read(result.buffer->allocation.vkBuffer, 0, 4) == 0, "empty report after destruction");
    Require(!buffers.at(f.accumulatorHandle).expired(), "empty recorded accumulator not retained for the in-flight cycle");
}
int main() {
    unsigned failures{};
    for (auto [name, test] : std::vector<std::pair<const char *, void (*)()>>{
        {"enabled/disabled/enabled", toggles}, {"optional precise feature", optional_precise_feature}, {"RP boundaries and report/reset", render_passes_and_reports},
        {"in-RP reset and reports", reset_and_reports_inside_render_pass}, {"report before following command", report_before_subsequent_command}, {"overwrite report after prior consumer", overwrite_report_after_graphics_consumer},
        {"report visible to later consumer", report_visible_to_later_consumer}, {"report widths and carry", report_width_and_carry}, {"pool rollover", pool_rollover},
        {"in-flight bank fences", bank_fences}, {"empty reset barrier", empty_reset_barrier},
        {"compute then empty reset barrier", compute_then_empty_reset_barrier}, {"repeated report barrier", repeated_report_barrier}, {"destroy before recording", destroyed_before_recording},
        {"empty destroy before recording", empty_destroyed_before_recording}}) {
        try { test(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception &error) { failures++; std::cerr << "FAIL " << name << ": " << error.what() << '\n'; }
    }
    return failures ? 1 : 0;
}
