// SPDX-License-Identifier: MPL-2.0
#include <utility>
#include <vulkan/vulkan_raii.hpp>
#include <boost/functional/hash.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <vector>

using u32 = uint32_t;
template<typename T> using span = std::span<T>;
namespace skyline::gpu {
    class GPU;
    namespace cache {
        #define private public
        #include "class.inc"
        #undef private
        #include "production.inc"
    }
}
using Cache = skyline::gpu::cache::RenderPassCache;

static void Check(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

// Android libc++ compares stored key -> borrowed query. Forward both host STL
// argument orders to that same production overload, avoiding implicit owned-key
// conversion by libstdc++. Hashes and equality bodies remain production code.
struct Equal {
    using is_transparent = std::true_type;
    bool operator()(const Cache::RenderPassMetadata &a, const Cache::RenderPassMetadata &b) const {
        return Cache::RenderPassEqual{}(a, b);
    }
    bool operator()(const Cache::RenderPassMetadata &a, const vk::RenderPassCreateInfo &b) const {
        return Cache::RenderPassEqual{}(a, b);
    }
    bool operator()(const vk::RenderPassCreateInfo &a, const Cache::RenderPassMetadata &b) const {
        return Cache::RenderPassEqual{}(b, a);
    }
};
using Map = std::unordered_map<Cache::RenderPassMetadata, u32, Cache::RenderPassHash, Equal>;

struct Fixture {
    std::array<vk::AttachmentDescription, 2> attachments{};
    std::array<vk::AttachmentReference, 2> references{};
    std::array<vk::SubpassDescription, 2> subpasses{};
    std::array<u32, 1> preserved{1};
    vk::SubpassDependency dependency{};
    vk::RenderPassCreateInfo info{};
    Fixture() {
        attachments[0].format = vk::Format::eR8G8B8A8Unorm;
        attachments[1].format = vk::Format::eD32Sfloat;
        references[0] = {.attachment = 0, .layout = vk::ImageLayout::eColorAttachmentOptimal};
        references[1] = {.attachment = 1, .layout = vk::ImageLayout::eDepthStencilAttachmentOptimal};
        subpasses[0] = {.pipelineBindPoint = vk::PipelineBindPoint::eGraphics,
            .colorAttachmentCount = 1, .pColorAttachments = references.data()};
        subpasses[1] = subpasses[0];
        info = {.attachmentCount = 2, .pAttachments = attachments.data(),
            .subpassCount = 1, .pSubpasses = subpasses.data()};
    }
};

static void CheckLookup(const vk::RenderPassCreateInfo &info) {
    Cache::RenderPassMetadata owned{info};
    Check(Cache::RenderPassEqual{}(owned, info), "equal representations rejected");
    Check(Cache::RenderPassHash{}(owned) == Cache::RenderPassHash{}(info),
          "equal owned/borrowed render-pass keys must hash identically");
    Map map;
    map.try_emplace(owned, 17);
    auto hit{map.find(info)};
    Check(hit != map.end() && hit->second == 17, "borrowed query must hit existing owned entry");
    Check(map.size() == 1, "lookup changed cache cardinality");
}

static void Regression() {
    Fixture f;
    CheckLookup(f.info); // Regression: color attachments with absent resolves.
    f.subpasses[0].colorAttachmentCount = 0;
    f.subpasses[0].pColorAttachments = nullptr;
    CheckLookup(f.info); // Empty color and resolve lists.
    f.subpasses[0].pResolveAttachments = f.references.data();
    CheckLookup(f.info); // Non-null resolve pointer with zero count.
    f.subpasses[0].colorAttachmentCount = 1;
    f.subpasses[0].pColorAttachments = f.references.data();
    CheckLookup(f.info); // Actual resolve list.
    f.subpasses[0].pDepthStencilAttachment = &f.references[1];
    f.subpasses[0].inputAttachmentCount = 1;
    f.subpasses[0].pInputAttachments = f.references.data();
    f.subpasses[0].preserveAttachmentCount = 1;
    f.subpasses[0].pPreserveAttachments = f.preserved.data();
    f.info.dependencyCount = 1;
    f.info.pDependencies = &f.dependency;
    CheckLookup(f.info);
    f.info.subpassCount = 2; // Mixed resolve and no-resolve subpasses.
    CheckLookup(f.info);
    Map map;
    map.try_emplace(Cache::RenderPassMetadata{f.info}, 7);
    f.attachments[0].format = vk::Format::eR8G8B8A8Uint;
    Check(map.find(f.info) == map.end(), "different attachment format reused old key");
    f.attachments[0].format = vk::Format::eR8G8B8A8Unorm;
    Check(map.find(f.info) != map.end(), "owned key borrowed mutable format storage");
    f.references[0].layout = vk::ImageLayout::eGeneral;
    Check(map.find(f.info) == map.end(), "different color/resolve layout reused old key");
    std::puts("Production render-pass keys: 6 hash/lookup cases, ownership and changed-key misses passed");
}

static void Benchmark() {
    Fixture f;
    constexpr size_t iterations{200000};
    Map map;
    size_t rebuilds{};
    // Mirrors GetRenderPass: find(borrowed), then construct owned metadata and
    // try_emplace on misses. Vulkan creation is intentionally excluded.
    auto lookup = [&]() {
        auto it{map.find(f.info)};
        if (it == map.end()) {
            ++rebuilds;
            return map.try_emplace(Cache::RenderPassMetadata{f.info}, 5).first->second;
        }
        return it->second;
    };
    lookup();
    rebuilds = 0;
    u32 checksum{};
    const auto start{std::chrono::steady_clock::now()};
    for (size_t i{}; i < iterations; ++i) checksum += lookup();
    const double ms{std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count()};
    Check(map.size() == 1 && checksum == iterations * 5, "benchmark changed cache results");
    std::printf("synthetic lookups=%zu metadata_rebuilds=%zu ms=%.3f checksum=%u\n", iterations, rebuilds, ms, checksum);
}

int main(int argc, char **argv) {
    if (argc == 2 && std::string_view{argv[1]} == "--benchmark") {
        for (size_t sample{}; sample < 5; ++sample) Benchmark();
    }
    else Regression();
}
