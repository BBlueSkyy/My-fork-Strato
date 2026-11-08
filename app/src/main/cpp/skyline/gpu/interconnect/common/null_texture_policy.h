// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstdint>

namespace skyline::gpu::interconnect {
    /**
     * @brief Why Textures::GetTexture handed back the shared null texture instead of a guest-backed view
     * @note The shared null texture is a 1x1 sampled-only image. It is a valid answer for sampling, but it is a
     *       stand-in for three very different situations and consumers must not treat them alike.
     */
    enum class NullTextureReason : std::uint8_t {
        None,                //!< A guest-backed view was returned
        PoolIndexOutOfRange, //!< The TIC index is outside the bound texture header pool
        UnsupportedFormat,   //!< The TIC format has no host translation, the guest surface itself is valid
        Unmapped,            //!< The TIC address does not resolve to resident GPU virtual memory
    };

    /**
     * @brief What a storage image binding must do with the result of Textures::GetTexture
     */
    enum class StorageImageNullAction : std::uint8_t {
        UseGuestTexture,    //!< Bind the guest-backed view (subject to the usual storage usage validation)
        BindNullDescriptor, //!< Bind a Vulkan null descriptor: loads return zero, stores and atomics are discarded
        Reject,             //!< No semantically faithful fallback exists, the binding must be reported as an error
    };

    /**
     * @brief Selects how a storage image descriptor is satisfied when no guest-backed texture is available
     * @note Only a TIC pointing at non-resident GPU memory is substituted: a store to an unmapped GPU address has no
     *       backing to modify, so dropping it (and returning zero for loads) is the faithful host equivalent.
     *       An out-of-range index or a TIC with an untranslated format refers to something the guest expects to
     *       exist, silently discarding its stores would corrupt output later, so those stay hard errors.
     * @note Without robustness2 nullDescriptor there is no way to express "discard stores" in Vulkan, binding a
     *       writable stand-in image would instead leak stores from one binding into loads of another.
     */
    constexpr StorageImageNullAction ResolveStorageImageNullAction(NullTextureReason reason, bool supportsNullDescriptor) {
        switch (reason) {
            case NullTextureReason::None:
                return StorageImageNullAction::UseGuestTexture;
            case NullTextureReason::Unmapped:
                return supportsNullDescriptor ? StorageImageNullAction::BindNullDescriptor : StorageImageNullAction::Reject;
            case NullTextureReason::PoolIndexOutOfRange:
            case NullTextureReason::UnsupportedFormat:
                return StorageImageNullAction::Reject;
        }
        return StorageImageNullAction::Reject;
    }
}
