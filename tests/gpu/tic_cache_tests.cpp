// SPDX-License-Identifier: MPL-2.0
// Standalone, CPU-only exercises of the actual typed TIC-cache policy.
#include <gpu/interconnect/common/texture_cache.h>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstddef>
#include <functional>
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
            return (static_cast<std::size_t>(t.address) << 1) ^ t.format;
        }
    };
    enum class ShaderType { Color1D, ColorArray1D, Color2D };
    struct Texture {
        bool replaced{};
        std::atomic<bool> aliasRetired{};
        bool IsObsoleteForViewReuse() const {
            return replaced || aliasRetired.load(std::memory_order_acquire);
        }
    };
    struct TextureView {
        std::shared_ptr<Texture> texture;
        ShaderType viewType{};
    };

    using CacheKey = skyline::gpu::interconnect::texture_cache::Key<Tic, ShaderType>;
    using KeyHash = skyline::gpu::interconnect::texture_cache::KeyHash<Tic, ShaderType, TicHash>;
    using Entry = skyline::gpu::interconnect::texture_cache::Entry<Tic, ShaderType, TextureView>;

    std::shared_ptr<TextureView> MakeView(ShaderType type) {
        return std::make_shared<TextureView>(TextureView{
            std::make_shared<Texture>(), type
        });
    }
}

int main() {
    const Tic tic{0x18420000, 42}, changed{0x18420000, 43};
    auto oneD{MakeView(ShaderType::Color1D)};
    auto arrayOneD{MakeView(ShaderType::ColorArray1D)};
    // Same physical TIC description can be requested under different
    // shader-derived view types. The global cache MUST retain both.
    std::unordered_map<CacheKey, std::shared_ptr<TextureView>, KeyHash> views;
    views[{tic, ShaderType::Color1D}] = oneD;
    views[{tic, ShaderType::ColorArray1D}] = arrayOneD;
    assert(views.size() == 2);
    assert(views.at({tic, ShaderType::Color1D})->viewType == ShaderType::Color1D);
    assert(views.at({tic, ShaderType::ColorArray1D})->viewType == ShaderType::ColorArray1D);
    assert(views.at({tic, ShaderType::Color1D}) != views.at({tic, ShaderType::ColorArray1D}));
    assert(views.find({changed, ShaderType::Color1D}) == views.end());

    Entry entry{tic, ShaderType::Color1D, oneD};
    assert(entry.Matches(tic, ShaderType::Color1D));
    assert(!entry.Matches(changed, ShaderType::Color1D));
    assert(!entry.Matches(tic, ShaderType::ColorArray1D));

    // Replacement can happen inside one channel sequence. There is no
    // special "same sequence" bypass in Entry::Matches.
    oneD->texture->replaced = true;
    assert(!entry.Matches(tic, ShaderType::Color1D));
    oneD->texture->replaced = false;
    oneD->texture->aliasRetired.store(true);
    assert(!entry.Matches(tic, ShaderType::Color1D));
    oneD->texture->aliasRetired.store(false);
    assert(entry.Matches(tic, ShaderType::Color1D));

    // The per-index entry must keep the old view alive if a global TIC
    // entry is replaced while another index still references it.
    std::weak_ptr<TextureView> oldWeak{oneD};
    views[{tic, ShaderType::Color1D}] = MakeView(ShaderType::Color1D);
    oneD.reset();
    assert(!oldWeak.expired());
    assert(entry.view == oldWeak.lock());
    entry = Entry{};
    assert(oldWeak.expired());

    // Empty entries never match, including when the TIC and type are zero.
    assert(!Entry{}.Matches(Tic{}, ShaderType::Color1D));

    std::puts("PASS: typed TIC keys, stale-view rejection, and cache-entry ownership");
}
