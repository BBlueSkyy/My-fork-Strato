// SPDX-License-Identifier: MPL-2.0
// Diagnostic prerequisite probes; no runtime policy and no fake Vulkan claims.
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>
#define TRACE_EVENT(...) ((void)0)

namespace skyline::gpu {
using u8 = std::uint8_t;
namespace vk {
enum class ImageLayout { eUndefined, eGeneral };
enum class ImageTiling { eOptimal, eLinear };
inline std::string to_string(ImageTiling) { return "fixture"; }
namespace raii { struct CommandBuffer {}; }
}
template<class... Args>
std::runtime_error exception(const char *message, Args&&...) {
    return std::runtime_error(message);
}
struct Counts {
    int deleteCalls{}, doubleDeletes{}, trapCalls{}, mirrorUnmaps{};
    int copies{}, textureWaits{}, copyWaits{};
    bool trapLive{true}, imageReleasedWhilePending{};
    std::vector<u8> guest = std::vector<u8>(32, 0);
    std::vector<u8> image = std::vector<u8>(32, 0x6D);
};
struct TrapFacade {
    Counts &counts;
    void DeleteTrap(int) {
        ++counts.deleteCalls;
        if (!counts.trapLive) ++counts.doubleDeletes;
        counts.trapLive = false;
    }
    void TrapRegions(int, bool) { ++counts.trapCalls; }
};
struct Process { TrapFacade trap; };
struct State { Process *process; };
namespace memory {
struct StagingBuffer {
    std::vector<u8> bytes;
    explicit StagingBuffer(std::size_t size) : bytes(size) {}
    u8 *data() { return bytes.data(); }
};
struct Image {
    std::vector<u8> bytes = std::vector<u8>(32, 0x6D);
    u8 *data() { return bytes.data(); }
};
}
struct FenceCycle {
    bool pending{true};
    int waits{};
    void Wait() { ++waits; pending = false; }
};
struct Scheduler {
    std::shared_ptr<FenceCycle> copy;
    template<class F> std::shared_ptr<FenceCycle> Submit(F &&record) {
        vk::raii::CommandBuffer command;
        record(command);
        copy = std::make_shared<FenceCycle>();
        return copy;
    }
};
struct Memory {
    std::shared_ptr<memory::StagingBuffer> AllocateStagingBuffer(std::size_t size) {
        return std::make_shared<memory::StagingBuffer>(size);
    }
};
struct GPU { State state; Memory memory; Scheduler scheduler; };
struct Texture;
struct AliasGroup {
    std::recursive_mutex operationMutex;
    std::mutex mutex;
    Texture *writerIdentity{};
    std::uint64_t generation{2}, guestGeneration{1};
};
struct GroupFacade {
    std::shared_ptr<AliasGroup> value;
    std::shared_ptr<AliasGroup> load() const { return value; }
};
struct MirrorFacade {
    Counts *counts{};
    bool valid() const { return counts != nullptr; }
    void *data() { return counts; }
    std::size_t size() const { return sizeof(Counts); }
};
// We count cleanup without touching real address-space mappings.
inline int munmap(void *pointer, std::size_t) {
    ++static_cast<Counts *>(pointer)->mirrorUnmaps;
    return 0;
}
struct Guest { int format{1}; };
// Observes whether a pending externally retained cycle outlives the image.
// This facade is NOT the production Image or a Vulkan ICD.
struct BackingSentinel {
    Counts &counts;
    std::shared_ptr<FenceCycle> observed;
    ~BackingSentinel() {
        counts.imageReleasedWhilePending = observed && observed->pending;
    }
};
struct Texture : std::enable_shared_from_this<Texture> {
    enum class DirtyState { Clean, CpuDirty, GpuDirty };
    GPU &gpu;
    Counts &counts;
    // Sentinel precedes cycle as the production backing does.
    BackingSentinel sentinel;
    std::variant<int, memory::Image> backing{0};
    MirrorFacade alignedMirror;
    std::optional<int> trapHandle{7};
    DirtyState dirtyState{DirtyState::CpuDirty};
    bool memoryFreed{};
    std::recursive_mutex stateMutex;
    GroupFacade aliasGroup;
    std::atomic<bool> aliasStale{}, aliasReadOnly{};
    std::optional<Guest> guest{Guest{}};
    int format{1};
    vk::ImageLayout layout{vk::ImageLayout::eGeneral};
    vk::ImageTiling tiling{vk::ImageTiling::eOptimal};
    std::size_t surfaceSize{32};
    std::uint64_t gpuReadbackSerial{}, gpuWriteSerial{1};
    std::shared_ptr<memory::StagingBuffer> downloadStagingBuffer;
    std::shared_ptr<FenceCycle> cycle;
    Texture(GPU &gpu, Counts &counts)
        : gpu(gpu), counts(counts), sentinel{counts, {}}, alignedMirror{&counts} {}
    ~Texture();
    void SynchronizeGuest(bool cpuDirty = false, bool skipTrap = false);
    void WaitOnBacking() {}
    void WaitOnFence() {
        ++counts.textureWaits;
        if (cycle) { cycle->Wait(); cycle.reset(); }
    }
    void CopyIntoStagingBuffer(const vk::raii::CommandBuffer &,
                              const std::shared_ptr<memory::StagingBuffer> &buffer) {
        buffer->bytes = counts.image;
    }
    void CopyToGuest(const u8 *bytes) {
        ++counts.copies;
        std::copy_n(bytes, counts.guest.size(), counts.guest.begin());
    }
};
#include "production.inc"

void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}
struct Fixture {
    Counts counts;
    Process process{{counts}};
    GPU gpu{{&process}, {}, {}};
    std::shared_ptr<Texture> make() { return std::make_shared<Texture>(gpu, counts); }
    void cleanupChecked() const {
        require(counts.mirrorUnmaps == 1, "mirror cleanup count");
    }
};

void cpuDirty() {
    Fixture f;
    f.make().reset();
    require(f.counts.deleteCalls == 1 && f.counts.doubleDeletes == 0, "CpuDirty trap cleanup");
    require(f.counts.copies == 0, "CpuDirty must not overwrite guest");
    f.cleanupChecked();
    std::cout << "OBSERVED CpuDirty: one trap delete, no readback\n";
}
void clean() {
    Fixture f;
    auto texture = f.make();
    texture->dirtyState = Texture::DirtyState::Clean;
    texture.reset();
    require(f.counts.deleteCalls == 2 && f.counts.doubleDeletes == 1,
            "audited Clean double deletion changed; review prerequisite");
    f.cleanupChecked();
    std::cout << "BLOCKER Clean: same trap deletion requested twice\n";
}
void gpuDirty() {
    Fixture f;
    auto texture = f.make();
    texture->dirtyState = Texture::DirtyState::GpuDirty;
    auto pending = std::make_shared<FenceCycle>();
    texture->cycle = pending;
    texture.reset();
    require(pending->waits == 1 && !pending->pending, "GPU predecessor must be waited");
    require(f.gpu.scheduler.copy && f.gpu.scheduler.copy->waits == 1, "copy must be waited");
    require(f.counts.copies == 1 && f.counts.guest == f.counts.image, "encoded image bytes");
    require(f.counts.deleteCalls == 2 && f.counts.doubleDeletes == 1,
            "audited GpuDirty double deletion changed; review prerequisite");
    f.cleanupChecked();
    std::cout << "BLOCKER GpuDirty: synchronous readback followed by duplicate trap deletion\n";
}
void pendingRead() {
    Fixture f;
    auto texture = f.make();
    auto pending = std::make_shared<FenceCycle>();
    texture->cycle = pending;
    texture->sentinel.observed = pending;
    texture.reset();
    require(pending->waits == 0 && f.counts.imageReleasedWhilePending,
            "audited CpuDirty pending-cycle behavior changed");
    f.cleanupChecked();
    std::cout << "OBLIGATION CpuDirty pending cycle: destructor body supplies no wait/independent retention\n";
}
void guardedAlias(bool readOnly) {
    Fixture f;
    auto texture = f.make();
    texture->dirtyState = Texture::DirtyState::GpuDirty;
    texture->aliasStale = !readOnly;
    texture->aliasReadOnly = readOnly;
    texture.reset();
    require(f.counts.copies == 0 && f.counts.deleteCalls == 1, "alias guard must skip encoding");
    f.cleanupChecked();
    std::cout << "OBSERVED " << (readOnly ? "read-only" : "stale") << ": no obsolete readback\n";
}
void incompatibleEncoding() {
    Fixture f;
    auto texture = f.make();
    texture->dirtyState = Texture::DirtyState::GpuDirty;
    texture->format = 2;
    texture.reset();
    require(f.counts.copies == 0, "converted format must not encode through native guest layout");
    f.cleanupChecked();
    std::cout << "OBLIGATION converted backing: synchronization return is not proof of preservation\n";
}
void externalOwner() {
    Fixture f;
    auto cache = f.make();
    auto external = cache;
    cache.reset();
    require(f.counts.deleteCalls == 0, "removing one owner must not imply destruction");
    external.reset();
    require(f.counts.deleteCalls == 1, "final owner must release");
    f.cleanupChecked();
    std::cout << "OBSERVED external shared owner delays destructor\n";
}
void groupedReadback() {
    Fixture f;
    auto texture = f.make();
    auto group = std::make_shared<AliasGroup>();
    group->writerIdentity = texture.get();
    texture->aliasGroup.value = group;
    texture->dirtyState = Texture::DirtyState::GpuDirty;
    texture->SynchronizeGuest(false, true);
    require(group->guestGeneration == group->generation, "group publication after copy");
    require(texture->gpuReadbackSerial == texture->gpuWriteSerial, "completed serial");
    require(f.counts.guest == f.counts.image, "group guest content");
    // Isolate destructor's Clean branch from the grouped publication probe.
    texture->aliasStale = true;
    texture.reset();
    f.cleanupChecked();
    std::cout << "OBSERVED grouped writer publishes guest generation after copy\n";
}
}
int main() {
    using namespace skyline::gpu;
    try {
        cpuDirty(); clean(); gpuDirty(); pendingRead();
        guardedAlias(false); guardedAlias(true); incompatibleEncoding();
        externalOwner(); groupedReadback();
        std::cout << "9 prerequisite scenarios; simulated OS/GPU interfaces, no Vulkan validation\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
