// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstdint>

namespace skyline::gpu::interconnect {
    // Maxwell's unbound bindless texture handle is all ones: TIC 0xFFFFF + TSC 0xFFF.
    // Keep these masks here so shader compilation and descriptor binding agree on the sentinel.
    inline constexpr std::uint32_t NullBindlessHandleRaw{0xFFFFFFFFU};
    inline constexpr std::uint32_t NullBindlessTextureIndex{0xFFFFFU};
    inline constexpr std::uint32_t NullBindlessSamplerIndex{0xFFFU};

    constexpr bool IsNullBindlessHandle(std::uint32_t raw) {
        return raw == NullBindlessHandleRaw;
    }

    /**
     * @brief Why Textures::GetTexture handed back the shared null texture instead of a guest-backed view
     * @note The shared null texture is a 1x1 sampled-only image. It is a valid answer for sampling, but it is a
     *       stand-in for three very different situations and consumers must not treat them alike.
     */
    enum class NullTextureReason : std::uint8_t {
        None,                //!< A guest-backed view was returned
        PoolIndexOutOfRange, //!< The TIC index is outside the bound texture header pool
        UnsupportedFormat,   //!< The TIC format has no host translation, the guest surface itself is valid
        Unmapped,            //!< A decoded TIC has missing backing in GPU virtual memory
        InvalidDescriptor,   //!< A reserved TIC target; no valid image extent or storage semantics
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
     * @note Only a TIC pointing at non-resident GPU memory is substituted: this preserves the existing emulator compatibility
     *       policy of zero loads and discarded stores. It is not proof of Maxwell fault/access semantics.
     *       An out-of-range index or a TIC with an untranslated format refers to something the guest expects to
     *       exist, silently discarding its stores would corrupt output later, so those stay hard errors.
     * @note Without robustness2 nullDescriptor this binder cannot express "discard stores"; binding a
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
            case NullTextureReason::InvalidDescriptor:
                return StorageImageNullAction::Reject;
        }
        return StorageImageNullAction::Reject;
    }
}
