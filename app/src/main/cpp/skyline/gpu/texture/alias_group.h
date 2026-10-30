// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace skyline::gpu {
    class Texture;
    class TextureManager;
    struct GuestTexture;
    namespace texture {
        /**
         * Atomic shared_ptr wrapper for Android libc++ versions that do not
         * provide std::atomic<std::shared_ptr<T>>. The standard shared_ptr
         * atomic free functions are available since C++11 and preserve the
         * same publication semantics without changing alias ownership.
         */
        template<typename T>
        class AtomicSharedPtr {
          private:
            std::shared_ptr<T> value{};

          public:
            std::shared_ptr<T> load(std::memory_order order = std::memory_order_seq_cst) const noexcept {
                return std::atomic_load_explicit(&value, order);
            }

            void store(std::shared_ptr<T> desired, std::memory_order order = std::memory_order_seq_cst) noexcept {
                std::atomic_store_explicit(&value, std::move(desired), order);
            }
        };

        // Only identical, ordered guest mappings join a group. Images retain
        // their own layout, views, locks and fences. This mutex protects metadata
        // only: never acquire a texture/state lock or wait on a fence under it.
        struct AliasGroup {
            std::mutex mutex;
            // Acquire after a Texture lock. Serializes guest encoding and writer
            // publication across different members, including CPU trap callbacks.
            std::recursive_mutex operationMutex;
            std::vector<std::weak_ptr<Texture>> members;
            std::weak_ptr<Texture> writer;
            Texture *writerIdentity{}; // Identity only; allows the last writer to finish readback during destruction.
            uint64_t generation{1};
            uint64_t guestGeneration{1};
        };
        inline std::atomic<uint64_t> gpuWriteSequence{};
    }
    struct TextureAliasReconciliationRequest {
        std::shared_ptr<Texture> target;
        std::vector<std::shared_ptr<Texture>> registration;
        std::vector<std::shared_ptr<void>> guestMemoryLeases{};
        std::shared_ptr<GuestTexture> registrationLayout{};
        std::vector<std::shared_ptr<Texture>> overlapReadbacks{};
        // A cold regional lookup must finish outside operation preparation:
        // retrying after Submit alone can reattach the same member forever.
        TextureManager *lookupManager{}; // GPU-owned; channelLock spans the retry.
        std::shared_ptr<GuestTexture> lookupLayout{};
    };
}
