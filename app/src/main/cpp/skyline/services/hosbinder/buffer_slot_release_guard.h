// SPDX-License-Identifier: MIT OR MPL-2.0
// Guard against a delayed presentation callback releasing a different frame
// which subsequently reused the same BufferQueue slot.
#pragma once

#include <cstdint>

namespace skyline::service::hosbinder {
    [[nodiscard]] constexpr bool ShouldReleaseQueuedFrame(std::uint64_t currentFrame,
                                                           std::uint64_t completedFrame,
                                                           bool slotIsQueued) noexcept {
        return slotIsQueued && currentFrame == completedFrame;
    }
}
