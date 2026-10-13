// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace skyline::gpu {
    class Texture;
    namespace texture {
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
    };
}
