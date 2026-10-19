// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <memory>
#include <vulkan/vulkan.hpp>

namespace skyline::gpu {
    class Buffer;
    class BufferManager;
    /** Raised before changing alias authority; the executor submits and retries at its operation boundary. */
    struct BufferMappingReconciliationRequest {
        BufferManager *manager;
        std::shared_ptr<Buffer> target;
        bool write;
        vk::DeviceSize offset, size;
        bool allowRepeatedCpuWrite{}; //!< Only ordered, immediately writable CPU transports may opt in.
        void Complete() const;
    };
}
