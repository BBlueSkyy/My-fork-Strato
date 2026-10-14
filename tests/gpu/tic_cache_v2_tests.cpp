// SPDX-License-Identifier: MPL-2.0
// CPU-only regression exercises for the actual production TicCache V2 policy.
#include <gpu/interconnect/common/tic_cache_v2.h>
#include <cassert>
#include <cstdio>
#include <memory>
#include <unordered_map>

namespace {
    struct Tic {
        unsigned address{};
        unsigned format{};
        bool operator==(const Tic &) const = default;
    };
    struct TicHash {
        std::size_t operator()(const Tic &t) const {
            return (static_cast<std::size_t>(t.address) << 2) ^ t.format;
        }
    };
    enum class ShaderType { Color1D, ColorArray1D, Color2D };
    struct Texture {
        bool replaced{};
    };
    struct View {
        std::shared_ptr<Texture> texture{std::make_shared<Texture>()};
        unsigned identity{};
    };

    using Key = skyline::gpu::interconnect::tic_cache::Key<Tic, ShaderType>;
    using KeyHash = skyline::gpu::interconnect::tic_cache::KeyHash<Tic, ShaderType, TicHash>;
    using Store = std::unordered_map<Key, std::shared_ptr<View>, KeyHash>;
    using Cache = skyline::gpu::interconnect::tic_cache::Cache<Tic, ShaderType, View, Store>;
    std::shared_ptr<View> Make(unsigned identity) {
        auto result{std::make_shared<View>()};
        result->identity = identity;
        return result;
    }
}

int main() {
    Cache cache;
    const Tic tic{0x18710000, 1}, changed{0x18710000, 2};
    cache.ObservePool(3, 1);
    assert(!cache.Lookup(0, tic, ShaderType::Color1D));

    // Identical TIC + different shader view types must never alias.
    auto linear{Make(10)}, array{Make(11)};
    cache.Remember(0, tic, ShaderType::Color1D, linear);
    assert(cache.Lookup(0, tic, ShaderType::Color1D) == linear.get());
    assert(!cache.Lookup(0, tic, ShaderType::ColorArray1D));
    cache.Remember(0, tic, ShaderType::ColorArray1D, array);
    assert(cache.Lookup(0, tic, ShaderType::ColorArray1D) == array.get());
    assert(cache.Lookup(0, tic, ShaderType::Color1D) == linear.get());
    assert(cache.SharedViewCount() == 2);
    assert(!cache.Lookup(0, changed, ShaderType::Color1D));

    // A submit invalidates only the indexed fast path, not shared views.
    cache.InvalidateIndexed();
    assert(cache.Lookup(0, tic, ShaderType::Color1D) == linear.get());
    assert(cache.SharedViewCount() == 2);

    // A replaced parent is never returned, even inside one submission.
    linear->texture->replaced = true;
    assert(!cache.Lookup(0, tic, ShaderType::Color1D));
    assert(cache.SharedViewCount() == 1);
    auto replacement{Make(12)};
    cache.Remember(0, tic, ShaderType::Color1D, replacement);
    assert(cache.Lookup(0, tic, ShaderType::Color1D) == replacement.get());

    // Index-local strong references survive replacement of the map slot.
    cache.Remember(1, tic, ShaderType::Color1D, replacement);
    std::weak_ptr<View> alive{replacement};
    cache.Remember(0, tic, ShaderType::Color1D, Make(13));
    replacement.reset();
    assert(!alive.expired());
    assert(cache.Lookup(1, tic, ShaderType::Color1D) == alive.lock().get());

    // Pool remapping with unchanged descriptor bytes must not reuse a
    // previously resolved view from the old physical pool.
    cache.ObservePool(3, 2);
    assert(cache.SharedViewCount() == 0);
    assert(!cache.Lookup(0, tic, ShaderType::Color1D));

    // Explicit header-cache invalidation drops the shared index/store paths.
    auto finalView{Make(14)};
    cache.Remember(2, tic, ShaderType::Color2D, finalView);
    assert(cache.Lookup(2, tic, ShaderType::Color2D) == finalView.get());
    cache.InvalidateAll();
    assert(cache.SharedViewCount() == 0);
    assert(!cache.Lookup(2, tic, ShaderType::Color2D));

    // New pool extent causes indexed reset and validates bounds.
    cache.ObservePool(1, 2);
    assert(!cache.Lookup(2, tic, ShaderType::Color2D));
    cache.Remember(2, tic, ShaderType::Color2D, Make(20));
    assert(cache.SharedViewCount() == 0);
    cache.Remember(0, changed, ShaderType::Color1D, Make(21));
    assert(cache.Lookup(0, changed, ShaderType::Color1D)->identity == 21);

    std::puts("PASS: TicCache V2 view type, generations, eviction validity and lifetime");
}
