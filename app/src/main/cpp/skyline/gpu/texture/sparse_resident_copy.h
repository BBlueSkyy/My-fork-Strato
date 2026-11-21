// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace skyline::gpu::texture {
    /**
     * @brief A resident (non-sparse-hole) run of a sampled sparse texture's physical backing
     * @note `cpu` points at *guest process memory*. That memory may be page-protected by the TrapManager on behalf of any
     *       other GPU-tracked resource, so it must never be dereferenced directly by the GPU/executor thread, see `CopySparseSegments`.
     */
    struct SparseSegment {
        uint64_t offset; //!< Byte offset of this run inside the texture's physical backing
        uint8_t *cpu; //!< First byte of the run in guest process memory
        size_t size;
    };

    /**
     * @brief Reads the resident runs of a sparse backing into `destination`, zero-filling every sparse hole
     * @param mapper Provides `uint8_t *Map(uint8_t *alignedBegin, size_t alignedSize)` returning an alias mapping of the page-aligned guest range that is
     *               *not* subject to TrapManager protection (a `MemoryManager::CreateMirror`) and `void Unmap(uint8_t *, size_t)` releasing it
     * @note Reading `SparseSegment::cpu` directly is a deadlock: a PROT_NONE page owned by a GPU-dirty resource makes the access fault into
     *       `TrapManager::HandleTrap`, whose lock callback waits for that resource's fence cycle. During `CommandExecutor::SubmitInternal`
     *       the cycle that has to be waited on is the submission that the faulting thread is itself in the middle of recording, so it can never be
     *       submitted. Ordinary textures avoid this by only ever reading their mirror, sparse textures must do the same.
     */
    template<typename Mapper>
    void CopySparseSegments(uint8_t *destination, size_t destinationSize, const SparseSegment *segments, size_t count, Mapper &mapper, size_t pageSize) {
        std::memset(destination, 0, destinationSize);
        for (size_t i{}; i < count; i++) {
            const auto &segment{segments[i]};
            if (!segment.size)
                continue;
            if (segment.offset > destinationSize || segment.size > destinationSize - segment.offset)
                throw std::out_of_range("Sparse resident segment exceeds the texture backing");

            const auto begin{reinterpret_cast<uintptr_t>(segment.cpu)};
            const auto alignedBegin{begin & ~(static_cast<uintptr_t>(pageSize) - 1)};
            const auto alignedEnd{(begin + segment.size + pageSize - 1) & ~(static_cast<uintptr_t>(pageSize) - 1)};
            const size_t alignedSize{alignedEnd - alignedBegin};

            uint8_t *mirror{mapper.Map(reinterpret_cast<uint8_t *>(alignedBegin), alignedSize)};
            try {
                std::memcpy(destination + segment.offset, mirror + (begin - alignedBegin), segment.size);
            } catch (...) {
                mapper.Unmap(mirror, alignedSize);
                throw;
            }
            mapper.Unmap(mirror, alignedSize);
        }
    }

    /**
     * @brief Merges a run into the previous one when both the backing offset and the host address are contiguous
     */
    inline void AppendSparseSegment(std::vector<SparseSegment> &segments, uint64_t offset, uint8_t *cpu, size_t size) {
        if (!size)
            return;
        if (!segments.empty()) {
            auto &last{segments.back()};
            if (last.offset + last.size == offset && last.cpu + last.size == cpu) {
                last.size += size;
                return;
            }
        }
        segments.push_back({offset, cpu, size});
    }
}
