#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i32 = std::int32_t;
using i64 = std::int64_t;
template<class T> using span = std::span<T>;
#define LOGW(...)
#define LOGD(...)
template<class T> int ToString(T) { return 0; }
template<class... A> std::runtime_error exception(const char *message, A...) { return std::runtime_error(message); }
enum class AndroidStatus { Ok, BadValue, InvalidOperation, NoMemory, NoInit };
enum class AndroidPixelFormat { None, RGBA8888, RGBX8888, BGRA8888, RGB565 };
enum class NativeWindowScalingMode { Freeze, ScaleToWindow, ScaleCrop, NoScaleCrop };
enum class NativeWindowTransform { Identity, MirrorHorizontal, MirrorVertical, Rotate90, Rotate180, Rotate270, MirrorHorizontalRotate90, MirrorVerticalRotate90, InvertDisplay };
enum class NativeWindowApi { None, EGL, CPU, Media, Camera };
enum class NvDisplayScanFormat { Progressive };
enum class NvSurfaceLayout { Blocklinear, Pitch, Tiled };
enum class PosixResult { Success };
struct AndroidRect { u32 left{}, top{}, right{}, bottom{}; };
struct AndroidFence {
    u32 token{};
    std::function<void()> onWait;
    template<class T> void Wait(T &) const { if (onWait) onWait(); }
};
struct Surface {
    u32 nvmapHandle{}, width{1}, height{1}, offset{}, size{4}, blockHeightLog2{}, pitch{4};
    NvDisplayScanFormat scanFormat{NvDisplayScanFormat::Progressive};
    NvSurfaceLayout layout{NvSurfaceLayout::Pitch};
};
struct NvGraphicHandle {
    static constexpr u32 Magic{1};
    u32 magic{Magic}, surfaceCount{1}, nvmapId{}, stride{};
    AndroidPixelFormat format{AndroidPixelFormat::RGBA8888};
    std::array<Surface, 1> surfaces;
};
struct GraphicBuffer {
    static constexpr u32 Magic{2};
    u32 magic{Magic}, intCount{sizeof(NvGraphicHandle) / sizeof(u32)}, width{1}, height{1}, usage{};
    AndroidPixelFormat format{AndroidPixelFormat::RGBA8888};
    NvGraphicHandle graphicHandle;
};
namespace vk { enum class ImageViewType { e2D }; }
namespace gpu {
    struct TextureView { u32 identity{}; };
    namespace texture {
        using Format = int;
        enum class TileMode { Block, Pitch };
        struct TileConfig { TileMode mode{}; u32 blockHeight{}, blockDepth{}, pitch{}; };
        struct Dimensions { Dimensions(u32, u32) {} };
    }
    namespace format { constexpr int R8G8B8A8Unorm=1, B8G8R8A8Unorm=2, R5G6B5Unorm=3; }
    struct GuestTexture {
        std::array<span<u8>, 1> mappings;
        GuestTexture(span<u8>, texture::Dimensions, int, texture::TileConfig, vk::ImageViewType) {}
        u32 GetLayerStride() { return 4; }
    };
}
struct NvMapHandle {
    u32 origSize{4096};
    PosixResult Duplicate(bool) { return PosixResult::Success; }
    void *GetPointer() { return nullptr; }
};
struct NvMap { std::shared_ptr<NvMapHandle> GetHandle(u32) { return std::make_shared<NvMapHandle>(); } };
// Allows a deterministic operation at the actual unlock boundary, without sleeps.
struct TestMutex {
    std::mutex native;
    std::function<void()> onUnlock;
    void lock() { native.lock(); }
    void unlock() {
        auto hook = std::exchange(onUnlock, {});
        native.unlock();
        if (hook) hook();
    }
};
struct Event { void Signal() {} };
struct Presentation {
    struct Item { std::shared_ptr<gpu::TextureView> texture; AndroidFence fence; std::function<void()> release; };
    std::vector<Item> pending;
    bool immediate{};
    u64 presented{};
    NativeWindowTransform GetTransformHint() { return NativeWindowTransform::Identity; }
    void Present(std::shared_ptr<gpu::TextureView> texture, i64, u32, AndroidRect,
                 NativeWindowScalingMode, NativeWindowTransform, const AndroidFence &fence,
                 std::function<void()> callback) {
        ++presented;
        if (immediate) callback();
        else pending.push_back({std::move(texture), fence, std::move(callback)});
    }
};
struct GPU {
    Presentation presentation;
    std::mutex channelLock;
    struct TextureManager { std::shared_ptr<gpu::TextureView> FindOrCreate(gpu::GuestTexture &) { return std::make_shared<gpu::TextureView>(); } } texture;
};
struct Process { struct Memory { template<class T> T TranslateVirtualPointer(u64) { return nullptr; } } memory; };
struct Soc { int host1x{}; };
struct DeviceState { GPU *gpu; Soc *soc; Process *process; };
#include "production_slots.inc"
struct GraphicBufferProducer : std::enable_shared_from_this<GraphicBufferProducer> {
    DeviceState state;
    TestMutex mutex;
    std::condition_variable_any freeCondition;
    static constexpr u8 MaxSlotCount{16};
    std::array<BufferSlot, MaxSlotCount> queue;
    u8 activeSlotCount{}, preallocatedBufferCount{};
    u32 defaultWidth{1}, defaultHeight{1};
    AndroidPixelFormat defaultFormat{AndroidPixelFormat::RGBA8888};
    NativeWindowApi connectedApi{NativeWindowApi::EGL};
    u64 frameNumber{};
    NvMap nvMap;
    std::shared_ptr<Event> bufferEvent{std::make_shared<Event>()};
    explicit GraphicBufferProducer(DeviceState state) : state(state) {}
    void FreeGraphicBufferNvMap(GraphicBuffer &) {}
    u32 GetPendingBufferCount();
    AndroidStatus RequestBuffer(i32, GraphicBuffer *&);
    AndroidStatus DequeueBuffer(bool, u32, u32, AndroidPixelFormat, u32, i32 &, std::optional<AndroidFence> &);
    AndroidStatus QueueBuffer(i32, i64, bool, AndroidRect, NativeWindowScalingMode, NativeWindowTransform, NativeWindowTransform, bool, u32, const AndroidFence &, u32 &, u32 &, NativeWindowTransform &, u32 &);
    void CancelBuffer(i32, const AndroidFence &);
    AndroidStatus Disconnect(NativeWindowApi);
    AndroidStatus SetPreallocatedBuffer(i32, const GraphicBuffer *);
};
#include "production.inc"

void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
struct Fixture {
    GPU gpu;
    Soc soc;
    Process process;
    std::shared_ptr<GraphicBufferProducer> producer{std::make_shared<GraphicBufferProducer>(DeviceState{&gpu, &soc, &process})};
    GraphicBuffer graphic;
    Fixture() { reset(); }
    void reset() {
        require(producer->SetPreallocatedBuffer(0, &graphic) == AndroidStatus::Ok, "preallocate");
        producer->queue[0].texture = std::make_shared<gpu::TextureView>();
    }
    AndroidFence dequeue() {
        i32 slot{-1}; std::optional<AndroidFence> fence;
        require(producer->DequeueBuffer(false, 1, 1, graphic.format, 0, slot, fence) == AndroidStatus::Ok && slot == 0, "dequeue");
        GraphicBuffer *requested{};
        require(producer->RequestBuffer(slot, requested) == AndroidStatus::Ok && requested, "request");
        return *fence;
    }
    void queue(const AndroidFence &fence = {}) {
        u32 width{}, height{}, pending{}; NativeWindowTransform hint{};
        require(producer->QueueBuffer(0, 0, true, {}, NativeWindowScalingMode::Freeze,
                                     NativeWindowTransform::Identity, NativeWindowTransform::Identity,
                                     false, 1, fence, width, height, hint, pending) == AndroidStatus::Ok, "queue");
    }
};

void staleReleaseAfterReset(bool disconnect, bool queueReplacement) {
    Fixture f;
    f.dequeue(); f.queue();
    auto releaseOld = f.gpu.presentation.pending[0].release;
    const auto oldFrame = f.producer->queue[0].frameNumber;
    if (disconnect) require(f.producer->Disconnect(NativeWindowApi::EGL) == AndroidStatus::Ok, "disconnect");
    f.reset(); f.dequeue();
    if (queueReplacement) f.queue();
    const auto expectedState = queueReplacement ? BufferState::Queued : BufferState::Dequeued;
    releaseOld();
    require(f.producer->queue[0].state == expectedState, "stale presentation callback freed a replacement slot");
    if (queueReplacement) {
        require(f.producer->queue[0].frameNumber > oldFrame, "frame identity must remain monotonic across reset");
        f.gpu.presentation.pending.back().release();
        require(f.producer->queue[0].state == BufferState::Free, "current presentation callback must free its slot");
    }
}

void snapshotTextureBeforeUnlock() {
    Fixture f;
    f.dequeue();
    auto original = f.producer->queue[0].texture;
    f.producer->mutex.onUnlock = [&] { f.reset(); };
    f.queue();
    require(f.gpu.presentation.pending[0].texture == original, "queued texture changed after producer mutex unlock");
}

void cancelWakesDequeuer() {
    Fixture f;
    f.dequeue();
    std::promise<void> enrolled;
    auto enrollment = enrolled.get_future();
    f.producer->mutex.onUnlock = [&] { enrolled.set_value(); };
    auto waiting = std::async(std::launch::async, [&] { return f.dequeue(); });
    enrollment.get();
    bool fenceWaited{};
    AndroidFence cancelFence{44, [&] {
        require(f.producer->queue[0].state == BufferState::Dequeued, "cancel released before its fence wait");
        fenceWaited = true;
    }};
    f.producer->CancelBuffer(0, cancelFence);
    bool woke = waiting.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
    if (!woke) f.producer->freeCondition.notify_all(); // Join cleanly when testing the broken revision.
    waiting.get();
    require(fenceWaited, "cancel must preserve the existing guest fence wait");
    require(woke, "CancelBuffer did not wake a blocked dequeuer");
}

void ordinaryReleaseKeepsFence() {
    Fixture f;
    f.dequeue(); f.queue(AndroidFence{77, {}});
    require(f.gpu.presentation.pending[0].fence.token == 77, "presentation lost producer fence");
    f.gpu.presentation.pending[0].release();
    require(f.dequeue().token == 77, "dequeue changed existing fence behavior");
}

void duplicateReleaseDoesNotFreeDequeuedSlot() {
    Fixture f;
    f.dequeue(); f.queue();
    f.gpu.presentation.pending[0].release();
    f.dequeue();
    f.gpu.presentation.pending[0].release();
    require(f.producer->queue[0].state == BufferState::Dequeued, "duplicate callback freed a dequeued slot with the same frame number");
}

int main(int argc, char **argv) {
    if (argc > 1 && std::string_view(argv[1]) == "--benchmark") {
        Fixture f;
        f.gpu.presentation.immediate = true;
        f.dequeue();
        constexpr u32 iterations{1000000};
        const auto iteration = [&] {
            f.producer->queue[0].state = BufferState::Dequeued;
            f.queue();
        };
        for (u32 i{}; i < 10000; ++i) iteration();
        std::array<double, 5> samples;
        std::cout << "CPU synthetic queue/immediate-release, ns/iteration:";
        for (auto &sample : samples) {
            const auto start = std::chrono::steady_clock::now();
            for (u32 i{}; i < iterations; ++i) iteration();
            const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
            sample = double(ns) / iterations;
            std::cout << ' ' << sample;
        }
        std::sort(samples.begin(), samples.end());
        std::cout << "; median=" << samples[2] << "; presented=" << f.gpu.presentation.presented << '\n';
        return 0;
    }
    int failures{};
    const auto run = [&](const char *name, auto test) {
        try { test(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception &error) { ++failures; std::cerr << "FAIL " << name << ": " << error.what() << '\n'; }
    };
    run("stale release after preallocation, dequeued", [] { staleReleaseAfterReset(false, false); });
    run("stale release after preallocation, queued", [] { staleReleaseAfterReset(false, true); });
    run("stale release after disconnect, dequeued", [] { staleReleaseAfterReset(true, false); });
    run("stale release after disconnect, queued", [] { staleReleaseAfterReset(true, true); });
    run("texture snapshot at unlock", snapshotTextureBeforeUnlock);
    run("cancel wakes blocked dequeue", cancelWakesDequeuer);
    run("ordinary release preserves guest fence", ordinaryReleaseKeepsFence);
    run("duplicate release keeps dequeued ownership", duplicateReleaseDoesNotFreeDequeuedSlot);
    return failures ? 1 : 0;
}
