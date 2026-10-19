#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
using u8 = uint8_t;
using u32 = uint32_t;
using u64 = uint64_t;
using i32 = int32_t;
#define TRACE_EVENT(...)
using ContextTag = u64;
struct exception : std::runtime_error { template<class... T> explicit exception(const char *message, T&&...) : std::runtime_error(message) {} };
void Check(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
namespace soc::gm20b { using IOVA = u64; }
namespace vk {
using RenderPass = u32; struct Rect2D {};
using Buffer = u64; using DescriptorSet = u64; using DeviceSize = u64; using PipelineStageFlags = u32;
namespace PipelineStageFlagBits { constexpr u32 eAllCommands = 1, eTransfer = 2, eComputeShader = 4, eConditionalRenderingEXT = 8; }
enum class IndexType { eUint32 };
namespace AccessFlagBits { constexpr u32 eMemoryWrite = 1, eConditionalRenderingReadEXT = 2, eTransferRead = 4, eTransferWrite = 8, eShaderRead = 16, eShaderWrite = 32; }
struct MemoryBarrier { u32 srcAccessMask, dstAccessMask; };
struct ConditionalRenderingBeginInfoEXT { u64 buffer, offset; };
struct BufferCopy { u64 srcOffset, dstOffset, size; };
namespace raii { struct CommandBuffer; }
}
struct Storage { std::vector<u32> words; explicit Storage(size_t bytes) : words((bytes + 3) / 4) {} };
static u64 nextHandle{1};
static std::map<u64, std::weak_ptr<Storage>> allocations;
static std::map<u64, u64> descriptors;
static std::shared_ptr<Storage> Get(u64 handle) { auto value = allocations.at(handle).lock(); Check(bool(value), "resource destroyed before deferred recording"); return value; }
namespace memory {
struct Buffer {
    std::shared_ptr<Storage> storage; u64 vkBuffer{};
    explicit Buffer(size_t bytes) : storage(std::make_shared<Storage>(bytes)), vkBuffer(nextHandle++) { allocations[vkBuffer] = storage; }
};
}
struct DescriptorAllocator { struct ActiveDescriptorSet { u64 handle; u64 operator*() const { return handle; } }; };
struct BufferBinding { u64 buffer{}, offset{}, size{}; };
struct GPU;
namespace engine { enum class DrawTopology { Triangles }; }
namespace conversion::quads { inline u32 GetIndexCount(u32 count) { return count; } }
struct TextureView {};
struct StateUpdater { void RecordAll(GPU &, vk::raii::CommandBuffer &) {} };
struct BufferBinding;
struct StateUpdateBuilder { template<class T> explicit StateUpdateBuilder(T &) {} void SetIndexBuffer(BufferBinding, vk::IndexType) {} StateUpdater Build() { return {}; } };
struct Buffer : std::enable_shared_from_this<Buffer> {
    memory::Buffer allocation{64}; u64 generation{}; bool blocked{}, dirty{}, currentDirty{}, locked{};
    u64 GetWriteGeneration() const { return generation; }
    void BlockSequencedCpuBackingWrites() { blocked = true; }
    bool IsCurrentExecutionGpuDirty() const { return currentDirty; }
};
struct BufferView {
    std::shared_ptr<Buffer> owner; u64 offset{}, size{};
    Buffer *GetBuffer() const { return owner.get(); }
    u64 GetOffset() const { return offset; }
    BufferBinding GetBinding(GPU &) const { return {owner->allocation.vkBuffer, offset, size}; }
    explicit operator bool() { return bool(owner); }
    bool LockWithTag(u64) { bool result = !owner->locked; owner->locked = true; return result; }
    void unlock() { owner->locked = false; }
    template<class S> void Read(bool first, const std::function<void()> &flush, S data, u64 readOffset) const {
        if (owner->dirty && !first) { flush(); Check(owner->locked, "CPU predicate resumed reading after Submit released its buffer lock"); }
        owner->dirty = false;
        std::memcpy(data.data(), reinterpret_cast<u8 *>(owner->allocation.storage->words.data()) + offset + readOffset, data.size());
    }
};
template<class T> struct ContextLock { T &value; bool first; ContextLock(u64 tag, T &value) : value(value), first(value.LockWithTag(tag)) {} ~ContextLock() { if (first) value.unlock(); } bool IsFirstUsage() const { return first; } };
template<class T> struct span {
    T *pointer; size_t count;
    span(T &value) : pointer(&value), count(1) {}
    span(T *pointer, size_t count) : pointer(pointer), count(count) {}
    T *data() const { return pointer; } size_t size() const { return count; }
    template<class U> span<U> cast() const { return {reinterpret_cast<U *>(pointer), count * sizeof(T) / sizeof(U)}; }
};
#include "shader.inc"
namespace vk::raii {
struct ConditionalRenderingBeginInfoEXTDummy {};
struct CommandBuffer {
    bool enabled{true};
    void beginTransformFeedbackEXT(u32, std::initializer_list<int>, std::initializer_list<int>) {}
    void endTransformFeedbackEXT(u32, std::initializer_list<int>, std::initializer_list<int>) {}
    void draw(u32,u32,u32,u32);
    void drawIndexed(u32,u32,u32,i32,u32);
    void beginConditionalRenderingEXT(vk::ConditionalRenderingBeginInfoEXT);
    void endConditionalRenderingEXT() { enabled = true; }
    template<class M> void pipelineBarrier(u32, u32, int, M, std::initializer_list<int>, std::initializer_list<int>) {}
    void copyBuffer(u64 src, u64 dst, vk::BufferCopy copy) {
        auto source = Get(src), destination = Get(dst);
        Check(copy.srcOffset + copy.size <= source->words.size() * 4 && copy.dstOffset + copy.size <= destination->words.size() * 4, "copy exceeds storage");
        std::memcpy(reinterpret_cast<u8 *>(destination->words.data()) + copy.dstOffset, reinterpret_cast<u8 *>(source->words.data()) + copy.srcOffset, copy.size);
    }
};
}
struct ConditionalCompare {
    int compares{};
    DescriptorAllocator::ActiveDescriptorSet CreateDescriptorSet(GPU &, u64 buffer) { auto handle = nextHandle++; descriptors[handle] = buffer; return {handle}; }
    void Compare(vk::raii::CommandBuffer &, u64 set, bool notEqual, bool nonZero = false) { compares++; auto memory = Get(descriptors.at(set)); ShaderMain(memory->words.data(), notEqual, nonZero); }
};
struct GPU { std::function<void()> writer; struct { bool supportsConditionalRendering{true}; bool supportsTransformFeedback{}; struct { bool relaxedRenderPassCompatibility{true}; } quirks; } traits; struct { memory::Buffer AllocateBuffer(size_t size) { return memory::Buffer(size); } } memory; struct { ConditionalCompare conditionalCompareHelperShader; } helperShaders; };
static GPU *recordingGPU;
void vk::raii::CommandBuffer::draw(u32,u32,u32,u32) { if (enabled && recordingGPU->writer) recordingGPU->writer(); }
void vk::raii::CommandBuffer::drawIndexed(u32,u32,u32,i32,u32) { draw(0,0,0,0); }
void vk::raii::CommandBuffer::beginConditionalRenderingEXT(vk::ConditionalRenderingBeginInfoEXT info) { enabled = Get(info.buffer)->words.at(info.offset / 4) != 0; }
struct FenceCycle {};
struct Allocator {
    std::vector<std::shared_ptr<void>> allocations;
    template<class T> T *EmplaceUntracked(T value) { auto object = std::make_shared<T>(std::move(value)); auto pointer = object.get(); allocations.push_back(std::move(object)); return pointer; }
};
struct Executor {
    u64 executionTag{1}, tag{1}; int submits{};
    Allocator allocatorStorage; Allocator *allocator{&allocatorStorage};
    int usageTracker{};
    template<class F> void RunWithTextureBackingRetry(F function) { function(); }
    void AddCheckpoint(const char *) {}
    template<class F, class T, class C, class H> void AddSubpass(F function, vk::Rect2D, T, std::initializer_list<int>, C, TextureView *, bool, u32, u32, H hook, bool attachmentFeedbackAllowed = false) { (void)attachmentFeedbackAllowed; hook(0); AddOutsideRpCommand([function](auto &cb, const auto &cycle, auto &gpu) { function(cb, cycle, gpu, 0, 0); }); }
    GPU *gpu{};
    std::vector<std::function<void(vk::raii::CommandBuffer &, const std::shared_ptr<FenceCycle> &, GPU &)>> commands;
    std::vector<std::shared_ptr<void>> dependencies;
    std::vector<std::shared_ptr<Buffer>> attached;
    std::function<void()> flush;
    void AttachBuffer(BufferView &view) { view.owner->locked = true; attached.push_back(view.owner); }
    template<class T> void AttachDependency(std::shared_ptr<T> value) { dependencies.push_back(value); }
    void BreakRenderPass() {}
    template<class F> void AddOutsideRpCommand(F function) { commands.emplace_back(function); }
    void Record() { recordingGPU = gpu; vk::raii::CommandBuffer command; for (auto &function : commands) function(command, {}, *gpu); commands.clear(); }
    void Submit() { ++submits; if (flush) flush(); Record(); ++executionTag; for (auto &buffer : attached) { buffer->locked = false; buffer->currentDirty = false; } attached.clear(); dependencies.clear(); }
};
struct InterconnectContext {
    GPU gpu; Executor executor;
    std::map<u64, BufferView> mappings;
    InterconnectContext() { executor.gpu = &gpu; }
};
struct CachedMappedBufferView {
    BufferView view;
    u64 address{};
    void Update(InterconnectContext &ctx, u64 input, u64 size) { if (!view || address != input) { view = ctx.mappings.at(input); address = input; } view.size = size; }
    void PurgeCaches() { view = {}; }
    BufferView &operator*() { return view; }
};
struct Queries { enum class CounterType { Occulusion }; int PrepareDraw(InterconnectContext &, CounterType, bool, u32) { return 0; } void Flush(InterconnectContext &) {} };
struct Pipeline {
    struct Desc { bool is_written{}; u32 count{}; };
    struct Stage { std::vector<Desc> storageBufferDescs, storageTexelBufferDescs; u32 uniformTexelBufferDescTotalCount{}; };
    struct { std::array<Stage, 3> stages; } descriptorInfo;
    std::vector<CachedMappedBufferView> storageBufferViews, texelBufferViews;
    u32 stageMask{1}; u64 lastExecutionTag{};
    bool MayWriteBuffer(Buffer *) const;
    void InheritDescriptorBufferViews(const Pipeline &);
    void AddWriter(Buffer *buffer) { descriptorInfo.stages[0].storageBufferDescs.push_back({true,1}); storageBufferViews.push_back({{buffer->shared_from_this(),0,8},0}); }
};
struct TransformFeedbackBufferState {
    struct Registers { bool streamOutEnable{}; struct { u32 size{}; } streamOutBuffer; } registers;
    Registers *engine{&registers}; CachedMappedBufferView view;
    bool MayWriteBuffer(Buffer *) const;
};
struct ActiveState {
    struct State { TransformFeedbackBufferState value; TransformFeedbackBufferState &Get() { return value; } };
    std::array<State, 2> transformFeedbackBuffers;
    std::vector<TextureView *> GetColorAttachments() { return {}; }
    TextureView *GetDepthAttachment() { return nullptr; }
    bool MayWriteTransformFeedbackBuffer(Buffer *);
};
#include "writers.inc"
struct UsageTracker { struct Intervals { void Insert(int) {} } dirtyIntervals, sequencedIntervals; };
struct GenerationBuffer {
    u64 writeGeneration{}; bool alreadyGpuDirty{}, everHadInlineUpdate{}, isDirect{}; std::optional<int> guest{1};
    u64 GetWriteGeneration() const { return writeGeneration; }
    void MarkGpuDirty(UsageTracker &);
    bool Write(span<u8>, u64, UsageTracker &, const std::function<void()> &);
    void CopyFrom(u64, GenerationBuffer *, u64, u64, UsageTracker &, const std::function<void()> &);
    void AdvanceSequence() {}
    void MarkGpuDirtyImpl() { alreadyGpuDirty = true; }
    bool WriteImplDirect(span<u8>,u64,UsageTracker &,const std::function<void()> &) { return false; }
    bool WriteImplStaged(span<u8>,u64,const std::function<void()> &) { return false; }
    void CopyFromImplDirect(u64,GenerationBuffer *,u64,u64,UsageTracker &,const std::function<void()> &) {}
    void CopyFromImplStaged(u64,GenerationBuffer *,u64,u64,UsageTracker &,const std::function<void()> &) {}
};
#include "generation.inc"
struct Maxwell3D {
    enum class RenderConditionMode : u8 { NonZero, Equal, NotEqual };
    InterconnectContext ctx; Queries queries;
    ActiveState activeState; Pipeline pipeline;
    Pipeline *activeDescriptorPipeline{&pipeline};
    struct { struct { bool NeedsQuadConversion() { return false; } } inputAssembly; } directState;
    struct { void ResetQuickBind() {} } constantBuffers;
    std::shared_ptr<memory::Buffer> quadConversionBuffer;
    std::vector<TextureView *> activeDescriptorSetSampledImages;
    u32 sampleCounterEnable{};
    vk::Rect2D GetDrawScissor() { return {}; }
    size_t UpdateQuadConversionBuffer(u32,u32) { return 0; }
    void PrepareDraw(StateUpdateBuilder &, engine::DrawTopology, bool, bool, u32,u32,u32 &,u32 &) {}
    void Draw(engine::DrawTopology, bool, bool, u32,u32,u32,u32,u32);
#include "members.inc"
    Maxwell3D() {
        renderConditionResources = std::make_shared<RenderConditionResources>(ctx.gpu);
        ctx.executor.flush = [this] { FlushQueries(); };
    }
    void ClearRenderCondition();
    bool SetRenderCondition(u64, std::optional<u64>, RenderConditionMode);
    bool CheckRenderCondition(u64, std::optional<u64>, RenderConditionMode);
    void RefreshRenderCondition();
    void FlushQueries();
    std::shared_ptr<Buffer> Map(u64 address, u64 value) { auto buffer = std::make_shared<Buffer>(); std::memcpy(buffer->allocation.storage->words.data(), &value, 8); ctx.mappings[address] = {buffer, 0, 8}; return buffer; }
    u32 Predicate() { auto binding = activeRenderCondition.Resolve(ctx.gpu); return Get(binding.buffer)->words.at(binding.offset / 4); }
};
#include "production.inc"
namespace gpu::interconnect::maxwell3d { using Maxwell3D = ::Maxwell3D; }
using IOVA = u64;
struct EngineMaxwell3D {
    struct Registers {
        struct RenderEnableOverride { enum class Mode { UseRenderEnable, AlwaysRender, NeverRender }; Mode mode{}; };
        struct RenderEnable { enum class Mode { True, False, Conditional, RenderIfEqual, RenderIfNotEqual }; Mode mode{}; u64 offset{0x100}; };
        RenderEnableOverride overrideStorage; RenderEnable enableStorage;
        RenderEnableOverride *renderEnableOverride{&overrideStorage};
        RenderEnable *renderEnable{&enableStorage};
    } registers;
    Maxwell3D &interconnect;
    explicit EngineMaxwell3D(Maxwell3D &interconnect) : interconnect(interconnect) {}
    struct Channel { struct AddressSpace { struct Gmmu { template<class T> T Read(u64) { return 0; } } gmmu; } addressSpace; AddressSpace *asCtx{&addressSpace}; } channelCtx;
    bool CheckRenderEnable();
};
#include "engine.inc"
int main() {
    int failures{};
    auto test = [&](const char *name, auto body) { try { body(); std::cout << "PASS " << name << '\n'; } catch (const std::exception &e) { ++failures; std::cerr << "FAIL " << name << ": " << e.what() << '\n'; } };
    test("equal compares high 32 bits", [] {
        Maxwell3D maxwell; maxwell.Map(0x100, 1ULL << 32); maxwell.Map(0x110, 0);
        maxwell.SetRenderCondition(0x100, 0x110, Maxwell3D::RenderConditionMode::Equal); maxwell.ctx.executor.Record();
        Check(maxwell.Predicate() == 0, "distinct 64-bit values compared equal");
    });
    test("not equal compares high 32 bits", [] {
        Maxwell3D maxwell; maxwell.Map(0x100, 1ULL << 32); maxwell.Map(0x110, 0);
        maxwell.SetRenderCondition(0x100, 0x110, Maxwell3D::RenderConditionMode::NotEqual); maxwell.ctx.executor.Record();
        Check(maxwell.Predicate() == 1, "high-word difference was discarded");
    });
    test("nonzero retains baseline low-word semantics pending hardware evidence", [] {
        Maxwell3D maxwell; maxwell.Map(0x100, 1ULL << 32);
        maxwell.SetRenderCondition(0x100, {}, Maxwell3D::RenderConditionMode::NonZero); maxwell.ctx.executor.Record();
        Check(maxwell.Predicate() == 0, "unverified nonzero semantics changed");
    });
    test("operand writes invalidate comparison and unchanged reads reuse it", [] {
        Maxwell3D maxwell; maxwell.Map(0x100, 7); auto rhs = maxwell.Map(0x110, 7);
        maxwell.SetRenderCondition(0x100, 0x110, Maxwell3D::RenderConditionMode::Equal); maxwell.ctx.executor.Record();
        Check(maxwell.Predicate() == 1, "first predicate");
        auto count = maxwell.ctx.gpu.helperShaders.conditionalCompareHelperShader.compares;
        maxwell.SetRenderCondition(0x100, 0x110, Maxwell3D::RenderConditionMode::Equal); maxwell.ctx.executor.Record();
        Check(maxwell.ctx.gpu.helperShaders.conditionalCompareHelperShader.compares == count, "unchanged input forced a new compare");
        rhs->allocation.storage->words[0] = 8; ++rhs->generation;
        maxwell.SetRenderCondition(0x100, 0x110, Maxwell3D::RenderConditionMode::Equal); maxwell.ctx.executor.Record();
        Check(maxwell.Predicate() == 0, "write reused the stale predicate");
    });
    test("both operands block sequenced CPU writes", [] {
        Maxwell3D maxwell; auto lhs = maxwell.Map(0x100, 1); auto rhs = maxwell.Map(0x110, 1);
        maxwell.SetRenderCondition(0x100, 0x110, Maxwell3D::RenderConditionMode::Equal);
        Check(lhs->blocked && rhs->blocked, "deferred compare inputs can be overwritten early");
    });
    test("flush discards cached guest mappings", [] {
        Maxwell3D maxwell; maxwell.Map(0x100, 1); maxwell.Map(0x110, 1);
        maxwell.SetRenderCondition(0x100, 0x110, Maxwell3D::RenderConditionMode::Equal); maxwell.ctx.executor.Submit();
        maxwell.Map(0x100, 2);
        maxwell.SetRenderCondition(0x100, 0x110, Maxwell3D::RenderConditionMode::Equal); maxwell.ctx.executor.Record();
        Check(maxwell.Predicate() == 0, "remapped IOVA read an old allocation");
    });
    test("scratch survives engine destruction through deferred recording", [] {
        Executor saved; GPU gpu;
        { auto maxwell = std::make_unique<Maxwell3D>(); maxwell->Map(0x100, 1); maxwell->Map(0x110, 1); maxwell->SetRenderCondition(0x100, 0x110, Maxwell3D::RenderConditionMode::Equal); saved.commands = std::move(maxwell->ctx.executor.commands); saved.dependencies = std::move(maxwell->ctx.executor.dependencies); }
        saved.gpu = &gpu; saved.Record();
    });
    test("untracked draw binding does not own a leaked resource reference", [] {
        Maxwell3D maxwell; maxwell.Map(0x100, 1); maxwell.Map(0x110, 1);
        maxwell.SetRenderCondition(0x100, 0x110, Maxwell3D::RenderConditionMode::Equal);
        using Binding = Maxwell3D::RenderConditionBinding;
        alignas(Binding) std::array<std::byte, sizeof(Binding)> storage;
        auto *untracked = std::construct_at(reinterpret_cast<Binding *>(storage.data()), maxwell.activeRenderCondition);
        std::weak_ptr<void> resources = maxwell.renderConditionResources;
        maxwell.ctx.executor.Record(); maxwell.ctx.executor.dependencies.clear();
        maxwell.activeRenderCondition = {}; maxwell.renderConditionResources.reset();
        const bool released = resources.expired();
        std::destroy_at(untracked);
        Check(released, "untracked DrawParams retained a nontrivial resource owner");
    });
    test("optional-extension fallback obeys zero predicates without submitting clean work", [] {
        Maxwell3D maxwell; maxwell.ctx.gpu.traits.supportsConditionalRendering = false; maxwell.Map(0x100, 0);
        EngineMaxwell3D engine{maxwell};
        engine.registers.renderEnable->mode = EngineMaxwell3D::Registers::RenderEnable::Mode::Conditional;
        Check(!engine.CheckRenderEnable(), "host without EXT rendered a false predicate");
        Check(maxwell.ctx.executor.submits == 0, "clean CPU predicate forced a submission");
        Check(!maxwell.activeRenderCondition.enabled, "CPU fallback retained a GPU condition");
    });
    test("fallback compares full 64-bit values", [] {
        Maxwell3D maxwell; maxwell.ctx.gpu.traits.supportsConditionalRendering = false;
        maxwell.Map(0x100, 1ULL << 32); maxwell.Map(0x110, 0);
        EngineMaxwell3D engine{maxwell};
        engine.registers.renderEnable->mode = EngineMaxwell3D::Registers::RenderEnable::Mode::RenderIfEqual;
        Check(!engine.CheckRenderEnable(), "fallback compared only low words");
        engine.registers.renderEnable->mode = EngineMaxwell3D::Registers::RenderEnable::Mode::RenderIfNotEqual;
        Check(engine.CheckRenderEnable(), "fallback ignored a high-word difference");
        Check(maxwell.ctx.executor.submits == 0, "clean comparison forced a submission");
    });
    test("fallback submits only the pending GPU-dirty predicate work", [] {
        Maxwell3D maxwell; maxwell.ctx.gpu.traits.supportsConditionalRendering = false;
        auto input = maxwell.Map(0x100, 1); input->dirty = input->currentDirty = input->locked = true;
        auto view = maxwell.ctx.mappings.at(0x100); maxwell.ctx.executor.AttachBuffer(view);
        maxwell.ctx.executor.AddOutsideRpCommand([input](auto &, const auto &, auto &) { input->allocation.storage->words[0] = 0; });
        EngineMaxwell3D engine{maxwell};
        engine.registers.renderEnable->mode = EngineMaxwell3D::Registers::RenderEnable::Mode::Conditional;
        Check(!engine.CheckRenderEnable(), "fallback read before pending GPU write");
        Check(maxwell.ctx.executor.submits == 1, "GPU dirty read did not submit exactly once");
        Check(!engine.CheckRenderEnable() && maxwell.ctx.executor.submits == 1, "clean repeated read submitted again");
    });
    test("arbitrary GPU writes use GPU conditions without query history", [] {
        Maxwell3D maxwell; maxwell.Map(0x100, 0);
        EngineMaxwell3D engine{maxwell};
        engine.registers.renderEnable->mode = EngineMaxwell3D::Registers::RenderEnable::Mode::Conditional;
        Check(engine.CheckRenderEnable(), "GPU condition was rejected");
        Check(maxwell.ctx.executor.commands.empty(), "condition published nodes before texture preparation");
        maxwell.RefreshRenderCondition(); maxwell.ctx.executor.Record();
        Check(maxwell.activeRenderCondition.enabled && maxwell.Predicate() == 0, "draw did not refresh the requested condition");
        Check(maxwell.ctx.executor.submits == 0, "GPU conditional path submitted CPU work");
    });
    test("texture retry refreshes operands and retains requested condition across submit", [] {
        Maxwell3D maxwell; maxwell.Map(0x100, 1); maxwell.Map(0x110, 1);
        Check(maxwell.CheckRenderCondition(0x100, 0x110, Maxwell3D::RenderConditionMode::Equal), "condition request");
        maxwell.RefreshRenderCondition(); maxwell.ctx.executor.Submit();
        auto replacement = maxwell.Map(0x100, 2);
        maxwell.RefreshRenderCondition();
        Check(std::find(maxwell.ctx.executor.attached.begin(), maxwell.ctx.executor.attached.end(), replacement) != maxwell.ctx.executor.attached.end(), "retry lost operand attachment");
        maxwell.ctx.executor.Record();
        Check(maxwell.Predicate() == 0, "retry reused preparation from the previous execution");
    });
    test("cached writer draw invalidates a predicate without descriptor rebind", [] {
        Maxwell3D maxwell; auto lhs = maxwell.Map(0x100, 1); maxwell.Map(0x110, 1);
        maxwell.CheckRenderCondition(0x100, 0x110, Maxwell3D::RenderConditionMode::Equal);
        maxwell.pipeline.AddWriter(lhs.get());
        maxwell.ctx.gpu.writer = [lhs] { lhs->allocation.storage->words[0] = 2; };
        maxwell.Draw(engine::DrawTopology::Triangles, false, false, 3,0,1,0,0); maxwell.ctx.executor.Record();
        maxwell.ctx.gpu.writer = {};
        maxwell.Draw(engine::DrawTopology::Triangles, false, false, 3,0,1,0,0); maxwell.ctx.executor.Record();
        Check(maxwell.Predicate() == 0, "cached SSBO writer left a stale predicate");
    });
    test("unconditioned writer draw invalidates a previously cached predicate", [] {
        Maxwell3D maxwell; auto lhs = maxwell.Map(0x100, 1); maxwell.Map(0x110, 1);
        maxwell.CheckRenderCondition(0x100, 0x110, Maxwell3D::RenderConditionMode::Equal); maxwell.RefreshRenderCondition(); maxwell.ctx.executor.Record();
        maxwell.ClearRenderCondition(); maxwell.pipeline.AddWriter(lhs.get());
        maxwell.ctx.gpu.writer = [lhs] { lhs->allocation.storage->words[0] = 2; };
        maxwell.Draw(engine::DrawTopology::Triangles, false, false, 3,0,1,0,0); maxwell.ctx.executor.Record();
        maxwell.CheckRenderCondition(0x100, 0x110, Maxwell3D::RenderConditionMode::Equal); maxwell.RefreshRenderCondition(); maxwell.ctx.executor.Record();
        Check(maxwell.Predicate() == 0, "unconditioned writer left a stale predicate");
    });
    test("plain draws keep the comparison cache", [] {
        Maxwell3D maxwell; maxwell.Map(0x100, 1); maxwell.Map(0x110, 1);
        maxwell.CheckRenderCondition(0x100, 0x110, Maxwell3D::RenderConditionMode::Equal);
        maxwell.Draw(engine::DrawTopology::Triangles, false, false, 3,0,1,0,0); maxwell.ctx.executor.Record();
        maxwell.Draw(engine::DrawTopology::Triangles, false, false, 3,0,1,0,0); maxwell.ctx.executor.Record();
        Check(maxwell.ctx.gpu.helperShaders.conditionalCompareHelperShader.compares == 1, "plain draw forced another compare");
    });
    test("write generation advances on every production Buffer writer entry point", [] {
        GenerationBuffer buffer, source; UsageTracker usage; u8 byte{};
        const auto initial = buffer.GetWriteGeneration();
        buffer.MarkGpuDirty(usage); buffer.MarkGpuDirty(usage);
        Check(buffer.GetWriteGeneration() >= initial + 2, "repeated GPU dirty writes did not advance generation");
        const auto afterGpu = buffer.GetWriteGeneration();
        buffer.Write(span<u8>{byte},0,usage,{});
        Check(buffer.GetWriteGeneration() > afterGpu, "inline write did not advance generation");
        const auto afterWrite = buffer.GetWriteGeneration();
        buffer.CopyFrom(0,&source,0,1,usage,{});
        Check(buffer.GetWriteGeneration() > afterWrite, "DMA copy did not advance generation");
    });
    test("writer detection respects read-only buffers and descriptor array indices", [] {
        Maxwell3D maxwell; auto lhs = maxwell.Map(0x100,1); auto other = maxwell.Map(0x110,1);
        Pipeline pipeline;
        pipeline.descriptorInfo.stages[0].storageBufferDescs = {{false,1},{true,1}};
        pipeline.storageBufferViews = {{{lhs,0,8},0},{{other,0,8},0}};
        Check(!pipeline.MayWriteBuffer(lhs.get()) && pipeline.MayWriteBuffer(other.get()), "SSBO write metadata or indices ignored");
        pipeline.descriptorInfo.stages[1].storageTexelBufferDescs = {{true,1}};
        pipeline.descriptorInfo.stages[1].uniformTexelBufferDescTotalCount = 1;
        pipeline.stageMask = 3; pipeline.texelBufferViews = {{{lhs,0,8},0},{{other,0,8},0}};
        Check(!pipeline.MayWriteBuffer(lhs.get()), "uniform texel descriptor was classified as writable");
        pipeline.texelBufferViews[1].view = {lhs,0,8};
        Check(pipeline.MayWriteBuffer(lhs.get()), "storage texel descriptor did not advance past uniform views");
    });
    test("compatible pipeline inherits descriptors used for write tracking", [] {
        Maxwell3D maxwell; auto lhs = maxwell.Map(0x100,1); Pipeline before, after;
        before.AddWriter(lhs.get()); after.descriptorInfo = before.descriptorInfo;
        before.lastExecutionTag = 42;
        after.InheritDescriptorBufferViews(before);
        Check(after.MayWriteBuffer(lhs.get()) && after.lastExecutionTag == 42, "compatible pipeline lost inherited writable descriptors");
    });
    test("transform feedback invalidates only enabled bound predicate buffers", [] {
        Maxwell3D maxwell; auto lhs = maxwell.Map(0x100,1); maxwell.Map(0x110,1);
        maxwell.ctx.gpu.traits.supportsTransformFeedback = true;
        auto &xfb = maxwell.activeState.transformFeedbackBuffers[0].Get();
        xfb.view.view = {lhs,0,8}; xfb.registers = {true,{8}};
        maxwell.CheckRenderCondition(0x100,0x110,Maxwell3D::RenderConditionMode::Equal);
        maxwell.ctx.gpu.writer = [lhs] { lhs->allocation.storage->words[0] = 2; };
        maxwell.Draw(engine::DrawTopology::Triangles,true,false,3,0,1,0,0); maxwell.ctx.executor.Record();
        maxwell.RefreshRenderCondition(); maxwell.ctx.executor.Record();
        Check(maxwell.Predicate() == 0, "cached transform-feedback writer did not invalidate predicate");
        xfb.registers.streamOutEnable = false;
        Check(!maxwell.activeState.MayWriteTransformFeedbackBuffer(lhs.get()), "disabled transform feedback was classified as a writer");
    });
    return failures ? 1 : 0;
}
