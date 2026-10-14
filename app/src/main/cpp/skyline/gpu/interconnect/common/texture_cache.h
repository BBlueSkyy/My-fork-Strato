// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>

namespace skyline::gpu::interconnect::texture_cache {
    /**
     * One TIC may be requested by shaders using different Vulkan view types
     * (for example, a 1D texture as Color1D or ColorArray1D). Both identity
     * components must be used by the long-lived view store.
     */
    template<typename Tic, typename ShaderType>
    struct Key {
        Tic tic{};
        ShaderType shaderType{};

        bool operator==(const Key &) const = default;
    };

    template<typename Tic, typename ShaderType, typename TicHash>
    struct KeyHash {
        std::size_t operator()(const Key<Tic, ShaderType> &key) const {
            std::size_t hash{TicHash{}(key.tic)};
            const auto typeHash{std::hash<ShaderType>{}(key.shaderType)};
            hash ^= typeHash + 0x9E3779B9U + (hash << 6) + (hash >> 2);
            return hash;
        }
    };

    /**
     * Keep the view alive independently of the long-lived TIC map. A second
     * TIC index may still reference an earlier view when that map is replaced.
     *
     * Validate identity and replacement on EVERY lookup; a channel-sequence
     * fast path must not bypass these checks.
     */
    template<typename Tic, typename ShaderType, typename View>
    struct Entry {
        Tic tic{};
        ShaderType shaderType{};
        std::shared_ptr<View> view{};

        bool Matches(const Tic &requestedTic, ShaderType requestedType) const {
            return view && shaderType == requestedType && tic == requestedTic &&
                   !view->texture->IsObsoleteForViewReuse();
        }
    };
}
