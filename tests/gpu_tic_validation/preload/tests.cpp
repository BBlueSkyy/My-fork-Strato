// SPDX-License-Identifier: MPL-2.0
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <vector>

using u32 = std::uint32_t;
using i64 = std::int64_t;
using u64 = std::uint64_t;
using exception = std::runtime_error;
template<class... Args> void Log(Args &&...) {}
#define LOGI(...) Log(__VA_ARGS__)
#define LOGW(...) Log(__VA_ARGS__)
namespace constant { constexpr int NsInMillisecond = 1000000; }
namespace util { inline u64 GetTimeNs() { return 0; } }
using PackedPipelineState = int;

struct PipelineStateBundle {
    int key{}, snapshot{};
    bool Deserialise(std::stringstream &stream) {
        char value;
        if (!stream.get(value)) return false;
        if (value == '!') throw exception("corrupted bundle");
        if (value == '?') throw std::logic_error("non-cache exception");
        key = 7;
        snapshot = value - '0';
        return true;
    }
    template<class T> T GetKey() const { return key; }
};
struct FilePipelineStateAccessor {
    const PipelineStateBundle &bundle;
};
struct Resource {
    bool alive{true};
    bool pending{true};
};
struct Assembler {
    std::vector<std::shared_ptr<Resource>> queued;
    std::function<void()> callback;
    int waits{}, destroyedBeforeDrain{};
    void RegisterCompilationCallback(std::function<void()> value) { callback = std::move(value); }
    void UnregisterCompilationCallback() { callback = {}; }
    void WaitIdle() {
        ++waits;
        for (auto &resource : queued) {
            if (!resource->alive) ++destroyedBeforeDrain;
            resource->pending = false;
            if (callback) callback();
        }
        queued.clear();
    }
    void SavePipelineCache() {}
};
struct Cache {
    std::string contents;
    bool invalidated{};
    auto OpenReadStream() { return std::pair{std::stringstream{contents}, 4u}; }
    void InvalidateAllAfter(u64) { invalidated = true; }
};
struct GPU {
    std::shared_ptr<Cache> graphicsPipelineCacheManager{std::make_shared<Cache>()};
    std::shared_ptr<Assembler> graphicsPipelineAssembler{std::make_shared<Assembler>()};
};
struct JvmManager {
    bool hidden{};
    void ShowPipelineLoadingScreen(u32) {}
    void UpdatePipelineLoadingProgress(u32) {}
    void HidePipelineLoadingScreen() { hidden = true; }
};
struct Pipeline {
    int specialization;
    std::shared_ptr<Resource> layout{std::make_shared<Resource>()};
    int &destroyedBeforeDrain;
    Pipeline(GPU &gpu, FilePipelineStateAccessor &accessor, int)
        : specialization{accessor.bundle.snapshot},
          destroyedBeforeDrain{gpu.graphicsPipelineAssembler->destroyedBeforeDrain} {
        gpu.graphicsPipelineAssembler->queued.push_back(layout);
    }
    ~Pipeline() {
        if (layout->pending) ++destroyedBeforeDrain;
        layout->alive = false;
    }
};
struct PipelineManager {
    std::map<int, std::vector<std::unique_ptr<Pipeline>>> map;
    PipelineManager(GPU &, JvmManager &);
};
#include "constructor.inc"

int main() {
    for (bool corrupted : {false, true}) {
        GPU gpu;
        JvmManager jvm;
        // Duplicate snapshot 1, distinct snapshot 2, then optional corruption.
        gpu.graphicsPipelineCacheManager->contents = corrupted ? "112!" : "112";
        PipelineManager manager{gpu, jvm};
        if (gpu.graphicsPipelineAssembler->destroyedBeforeDrain ||
            !gpu.graphicsPipelineAssembler->queued.empty()) {
            std::fprintf(stderr, "FAIL preload %s: resources destroyed before drain=%d pending=%zu\n",
                         corrupted ? "corrupt" : "valid", gpu.graphicsPipelineAssembler->destroyedBeforeDrain,
                         gpu.graphicsPipelineAssembler->queued.size());
            return 1;
        }
        const auto &variants = manager.map.at(7);
        if (variants.size() != 2 || variants[0]->specialization != 1 ||
            variants[1]->specialization != 2 || !jvm.hidden ||
            gpu.graphicsPipelineCacheManager->invalidated != corrupted) return 2;
    }
    {
        GPU gpu;
        JvmManager jvm;
        gpu.graphicsPipelineCacheManager->contents = "112?";
        bool caught{};
        try { PipelineManager manager{gpu,jvm}; }
        catch (const std::logic_error&) { caught=true; }
        if (!caught || gpu.graphicsPipelineAssembler->destroyedBeforeDrain || !gpu.graphicsPipelineAssembler->queued.empty() || !jvm.hidden || gpu.graphicsPipelineAssembler->callback) {
            std::fprintf(stderr,"FAIL preload non-cache exception: destroyed=%d pending=%zu hidden=%d\n",gpu.graphicsPipelineAssembler->destroyedBeforeDrain,gpu.graphicsPipelineAssembler->queued.size(),jvm.hidden);
            return 3;
        }
    }
    std::puts("PASS production preload: duplicate lifetime on success/corruption; distinct snapshots retained");
}
