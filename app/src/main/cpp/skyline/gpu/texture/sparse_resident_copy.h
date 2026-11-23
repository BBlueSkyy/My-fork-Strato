// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>
#include <common/fault_breadcrumb.h>

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
        // Records the last completed/entered step of this thread so an unhandled SIGSEGV in here can be located (see ReportFaultBreadcrumb)
        auto &crumb{skyline::faultBreadcrumb};
        struct Reset {
            skyline::FaultBreadcrumb &crumb;
            ~Reset() { crumb.step = nullptr; crumb.mirror = 0; }
        } reset{crumb};
        crumb.sequence++;
        crumb.count = count;

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

            crumb.index = i;
            crumb.begin = alignedBegin;
            crumb.size = alignedSize;
            crumb.mirror = 0;
            crumb.step = "map-begin";
            uint8_t *mirror{mapper.Map(reinterpret_cast<uint8_t *>(alignedBegin), alignedSize)};
            crumb.mirror = reinterpret_cast<uintptr_t>(mirror);
            crumb.step = "map-end";
            try {
                crumb.step = "copy-begin";
                std::memcpy(destination + segment.offset, mirror + (begin - alignedBegin), segment.size);
                crumb.step = "copy-end";
            } catch (...) {
                mapper.Unmap(mirror, alignedSize);
                throw;
            }
            crumb.step = "unmap-begin";
            mapper.Unmap(mirror, alignedSize);
            crumb.step = "unmap-end";
        }
    }

    enum class SparseReadResult {
        Stable, //!< The snapshot was copied and GMMU residency did not change while copying it
        Unmapped, //!< The range is genuinely unmapped, the backing is entirely zero
        Unstable, //!< Residency kept changing during every attempt, the copy may mix two residency states and must not be treated as final
    };

    /**
     * @brief Reads a sparse backing consistently without holding the GMMU lock across the copy (seqlock style)
     * @param snapshot Called as `bool snapshot(std::vector<SparseSegment> &, uint64_t &generation)`, false for genuinely unmapped VA
     * @param currentGeneration Called as `uint64_t currentGeneration()`, the lock-free GMMU mapping generation
     * @param copy Called as `void copy(const std::vector<SparseSegment> &)`, must zero-fill holes and copy the resident runs
     * @param zero Called to clear the destination when unmapped
     * @param yield Called between attempts so a remap storm can finish
     */
    template<typename Snapshot, typename Generation, typename Copy, typename Zero, typename Yield>
    SparseReadResult ReadSparseConsistent(int attempts, Snapshot &&snapshot, Generation &&currentGeneration, Copy &&copy, Zero &&zero, Yield &&yield) {
        for (int attempt{}; attempt < attempts; attempt++) {
            std::vector<SparseSegment> segments;
            uint64_t generation{};
            if (!snapshot(segments, generation)) {
                zero();
                return SparseReadResult::Unmapped;
            }
            copy(segments);
            if (currentGeneration() == generation)
                return SparseReadResult::Stable;
            yield();
        }
        return SparseReadResult::Unstable;
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
