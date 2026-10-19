// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <common/trace.h>
#include <kernel/types/KProcess.h>
#include "address_space.h"

#define MAP_MEMBER(returnType) template<typename VaType, VaType UnmappedVa, typename PaType, PaType UnmappedPa, bool PaContigSplit, size_t AddressSpaceBits, typename ExtraBlockInfo> requires AddressSpaceValid<VaType, AddressSpaceBits> returnType FlatAddressSpaceMap<VaType, UnmappedVa, PaType, UnmappedPa, PaContigSplit, AddressSpaceBits, ExtraBlockInfo>

#define MM_MEMBER(returnType) template<typename VaType, VaType UnmappedVa, size_t AddressSpaceBits, size_t VaGranularityBits, size_t VaL2GranularityBits> requires AddressSpaceValid<VaType, AddressSpaceBits> returnType FlatMemoryManager<VaType, UnmappedVa, AddressSpaceBits, VaGranularityBits, VaL2GranularityBits>

#define ALLOC_MEMBER(returnType) template<typename VaType, VaType UnmappedVa, size_t AddressSpaceBits> requires AddressSpaceValid<VaType, AddressSpaceBits> returnType FlatAllocator<VaType, UnmappedVa, AddressSpaceBits>

namespace skyline {
    MAP_MEMBER()::FlatAddressSpaceMap(VaType vaLimit, std::function<void(VaType, VaType)> unmapCallback) :
        vaLimit(vaLimit),
        unmapCallback(std::move(unmapCallback)) {
        if (vaLimit > VaMaximum)
            throw exception("Invalid VA limit!");
    }

    MAP_MEMBER(void)::MapLocked(VaType virt, PaType phys, VaType size, ExtraBlockInfo extraInfo) {
        TRACE_EVENT("containers", "FlatAddressSpaceMap::Map");

        VaType virtEnd{virt + size};

        if (virtEnd > vaLimit)
            throw exception("Trying to map a block past the VA limit: virtEnd: 0x{:X}, vaLimit: 0x{:X}", virtEnd, vaLimit);

        auto blockEndSuccessor{std::lower_bound(blocks.begin(), blocks.end(), virtEnd)};
        if (blockEndSuccessor == blocks.begin())
            throw exception("Trying to map a block before the VA start: virtEnd: 0x{:X}", virtEnd);

        auto blockEndPredecessor{std::prev(blockEndSuccessor)};

        if (blockEndSuccessor != blocks.end()) {
            // We have blocks in front of us, if one is directly in front then we don't have to add a tail
            if (blockEndSuccessor->virt != virtEnd) {
                PaType tailPhys{[&]() -> PaType {
                    if (!PaContigSplit || blockEndPredecessor->Unmapped())
                        return blockEndPredecessor->phys; // Always propagate unmapped regions rather than calculating offset
                    else
                        return blockEndPredecessor->phys + virtEnd - blockEndPredecessor->virt;
                }()};

                if (blockEndPredecessor->virt >= virt) {
                    // If this block's start would be overlapped by the map then reuse it as a tail block
                    blockEndPredecessor->virt = virtEnd;
                    blockEndPredecessor->phys = tailPhys;
                    blockEndPredecessor->extraInfo = blockEndPredecessor->extraInfo;

                    // No longer predecessor anymore
                    blockEndSuccessor = blockEndPredecessor--;
                } else {
                    // Else insert a new one and we're done
                    blocks.insert(blockEndSuccessor, {Block(virt, phys, extraInfo), Block(virtEnd, tailPhys, blockEndPredecessor->extraInfo)});
                    if (unmapCallback)
                        unmapCallback(virt, size);

                    return;
                }
            }
        } else {
            // blockEndPredecessor will always be unmapped as blocks has to be terminated by an unmapped chunk
            if (blockEndPredecessor != blocks.begin() && blockEndPredecessor->virt >= virt) {
                // Move the unmapped block start backwards
                blockEndPredecessor->virt = virtEnd;

                // No longer predecessor anymore
                blockEndSuccessor = blockEndPredecessor--;
            } else {
                // Else insert a new one and we're done
                blocks.insert(blockEndSuccessor, {Block(virt, phys, extraInfo), Block(virtEnd, UnmappedPa, {})});
                if (unmapCallback)
                    unmapCallback(virt, size);

                return;
            }
        }

        auto blockStartSuccessor{blockEndSuccessor};

        // Walk the block vector to find the start successor as this is more efficient than another binary search in most scenarios
        while (std::prev(blockStartSuccessor)->virt >= virt)
            blockStartSuccessor--;

        // Check that the start successor is either the end block or something in between
        if (blockStartSuccessor->virt > virtEnd) {
            throw exception("Unsorted block in AS map: virt: 0x{:X}", blockStartSuccessor->virt);
        } else if (blockStartSuccessor->virt == virtEnd) {
            // We need to create a new block as there are none spare that we would overwrite
            blocks.insert(blockStartSuccessor, Block(virt, phys, extraInfo));
        } else {
            // Erase overwritten blocks
            if (auto eraseStart{std::next(blockStartSuccessor)}; eraseStart != blockEndSuccessor)
                blocks.erase(eraseStart, blockEndSuccessor);

            // Reuse a block that would otherwise be overwritten as a start block
            blockStartSuccessor->virt = virt;
            blockStartSuccessor->phys = phys;
            blockStartSuccessor->extraInfo = extraInfo;
        }

        if (unmapCallback)
            unmapCallback(virt, size);
    }

    MAP_MEMBER(void)::UnmapLocked(VaType virt, VaType size) {
        TRACE_EVENT("containers", "FlatAddressSpaceMap::Unmap");

        VaType virtEnd{virt + size};

        if (virtEnd > vaLimit)
            throw exception("Trying to map a block past the VA limit: virtEnd: 0x{:X}, vaLimit: 0x{:X}", virtEnd, vaLimit);

        auto blockEndSuccessor{std::lower_bound(blocks.begin(), blocks.end(), virtEnd)};
        if (blockEndSuccessor == blocks.begin())
            throw exception("Trying to unmap a block before the VA start: virtEnd: 0x{:X}", virtEnd);

        auto blockEndPredecessor{std::prev(blockEndSuccessor)};

        auto walkBackToPredecessor{[&](auto iter) {
            while (iter->virt >= virt)
                iter--;

            return iter;
        }};

        auto eraseBlocksWithEndUnmapped{[&](auto unmappedEnd) {
            auto blockStartPredecessor{walkBackToPredecessor(unmappedEnd)};
            auto blockStartSuccessor{std::next(blockStartPredecessor)};

            auto eraseEnd{[&]() {
                if (blockStartPredecessor->Unmapped()) {
                    // If the start predecessor is unmapped then we can erase everything in our region and be done
                    return std::next(unmappedEnd);
                } else {
                    // Else reuse the end predecessor as the start of our unmapped region then erase all up to it
                    unmappedEnd->virt = virt;
                    return unmappedEnd;
                }
            }()};

            // We can't have two unmapped regions after each other
            if (eraseEnd != blocks.end() && (eraseEnd == blockStartSuccessor || (blockStartPredecessor->Unmapped() && eraseEnd->Unmapped())))
                throw exception("Multiple contiguous unmapped regions are unsupported!");

            blocks.erase(blockStartSuccessor, eraseEnd);
        }};

        // We can avoid any splitting logic if these are the case
        if (blockEndPredecessor->Unmapped()) {
            if (blockEndPredecessor->virt > virt)
                eraseBlocksWithEndUnmapped(blockEndPredecessor);

            if (unmapCallback)
                unmapCallback(virt, size);

            return; // The region is unmapped, bail out early
        } else if (blockEndSuccessor->virt == virtEnd && blockEndSuccessor->Unmapped()) {
            eraseBlocksWithEndUnmapped(blockEndSuccessor);

            if (unmapCallback)
                unmapCallback(virt, size);

            return; // The region is unmapped here and doesn't need splitting, bail out early
        } else if (blockEndSuccessor == blocks.end()) {
            // This should never happen as the end should always follow an unmapped block
            throw exception("Unexpected Memory Manager state!");
        } else if (blockEndSuccessor->virt != virtEnd) {
            // If one block is directly in front then we don't have to add a tail

            // The previous block is mapped so we will need to add a tail with an offset
            PaType tailPhys{[&]() {
                if constexpr (PaContigSplit)
                    return blockEndPredecessor->phys + virtEnd - blockEndPredecessor->virt;
                else
                    return blockEndPredecessor->phys;
            }()};

            if (blockEndPredecessor->virt >= virt) {
                // If this block's start would be overlapped by the unmap then reuse it as a tail block
                blockEndPredecessor->virt = virtEnd;
                blockEndPredecessor->phys = tailPhys;

                // No longer predecessor anymore
                blockEndSuccessor = blockEndPredecessor--;
            } else {
                blocks.insert(blockEndSuccessor, {Block(virt, UnmappedPa, {}), Block(virtEnd, tailPhys, blockEndPredecessor->extraInfo)});
                if (unmapCallback)
                    unmapCallback(virt, size);

                return; // The previous block is mapped and ends before
            }
        }

        // Walk the block vector to find the start predecessor as this is more efficient than another binary search in most scenarios
        auto blockStartPredecessor{walkBackToPredecessor(blockEndSuccessor)};
        auto blockStartSuccessor{std::next(blockStartPredecessor)};

        if (blockStartSuccessor->virt > virtEnd) {
            throw exception("Unsorted block in AS map: virt: 0x{:X}", blockStartSuccessor->virt);
        } else if (blockStartSuccessor->virt == virtEnd) {
            // There are no blocks between the start and the end that would let us skip inserting a new one for head

            // The previous block is may be unmapped, if so we don't need to insert any unmaps after it
            if (blockStartPredecessor->Mapped())
                blocks.insert(blockStartSuccessor, Block(virt, UnmappedPa, {}));
        } else if (blockStartPredecessor->Unmapped()) {
            // If the previous block is unmapped
            blocks.erase(blockStartSuccessor, blockEndPredecessor);
        } else {
            // Erase overwritten blocks, skipping the first one as we have written the unmapped start block there
            if (auto eraseStart{std::next(blockStartSuccessor)}; eraseStart != blockEndSuccessor)
                blocks.erase(eraseStart, blockEndSuccessor);

            // Add in the unmapped block header
            blockStartSuccessor->virt = virt;
            blockStartSuccessor->phys = UnmappedPa;
        }

        if (unmapCallback)
            unmapCallback(virt, size);
    }


    MM_MEMBER(void)::ReplaceMappingLocked(VaType virt, u8 *phys, VaType size, MemoryManagerBlockInfo info) {
        ValidateRange(virt, size);
        if (!size) return;
        constexpr VaType pageMask{(VaType{1} << VaGranularityBits) - 1};
        if ((virt | size) & pageMask)
            throw exception("GPU mapping is not page aligned");
        auto &blocks{this->blocks};
        // Allocate before mutating either representation. Erase/insert below
        // cannot allocate, and block/table publication remains under one lock.
        if (blocks.capacity() - blocks.size() < 2)
            blocks.reserve(std::max(blocks.size() + 2, blocks.capacity() * 2));
        const size_t end{size_t{virt} + size};
        auto first{std::lower_bound(blocks.begin(), blocks.end(), virt)};
        auto after{std::upper_bound(blocks.begin(), blocks.end(), end,
            [](auto address, const auto &block) { return address < block.virt; })};
        auto tail{*std::prev(after)};
        if (tail.phys && !tail.extraInfo.sparseMapped) tail.phys += end - tail.virt;
        const bool hasTail{end <= this->vaLimit};
        if (hasTail) tail.virt = static_cast<VaType>(end);
        const auto index{first - blocks.begin()};
        blocks.erase(first, after);
        blocks.insert(blocks.begin() + index, {virt, phys, info});
        if (hasTail) blocks.insert(blocks.begin() + index + 1, tail);

        // Merge equivalent neighbours without discarding sparse/resident state.
        for (size_t i{index ? static_cast<size_t>(index - 1) : 0}; i + 1 < blocks.size() && blocks[i].virt <= end;) {
            const auto &left{blocks[i]};
            const auto &right{blocks[i + 1]};
            const bool sameKind{left.extraInfo.sparseMapped == right.extraInfo.sparseMapped};
            const bool contiguous{sameKind && (left.extraInfo.sparseMapped ||
                (!left.phys && !right.phys) || (left.phys && right.phys &&
                reinterpret_cast<uintptr_t>(left.phys) + (right.virt - left.virt) == reinterpret_cast<uintptr_t>(right.phys)))};
            if (contiguous) blocks.erase(blocks.begin() + i + 1);
            else ++i;
        }

        // Clear only the changed pages. Refresh surviving head/tail blocks too:
        // their cached extents must no longer cross the replacement interval.
        blockSegmentTable.Set(virt, end, {});
        auto current{std::upper_bound(blocks.begin(), blocks.end(), virt,
            [](auto address, const auto &block) { return address < block.virt; })};
        if (current - blocks.begin() > 1) --current;
        --current;
        for (; current != blocks.end() && current->virt <= end; ++current) {
            auto next{std::next(current)};
            const size_t blockEnd{next == blocks.end() ? size_t{this->vaLimit} + 1 : next->virt};
            // Sparse/unmapped entries stay empty in the fast table. Their
            // authoritative interval lookup avoids rewriting an entire sparse
            // reservation whenever one small resident tile is replaced.
            if (current->phys && !current->extraInfo.sparseMapped)
                blockSegmentTable.Set(current->virt, blockEnd,
                    {current->virt, current->phys, blockEnd - current->virt, current->extraInfo});
        }
        mappingGeneration.fetch_add(1, std::memory_order_release);
    }

    MM_MEMBER(TranslatedAddressRange)::TranslateRangeImpl(VaType virt, VaType size, std::function<void(span<u8>)> cpuAccessCallback) {
        TranslatedAddressRange ranges;
        while (size) {
            const auto entry{LookupEntryLocked(virt)};
            const auto length{std::min<size_t>(size, entry.extent - (virt - entry.virt))};
            span<u8> memory{MappingKind(entry) == MemoryMappingKind::Resident ? entry.phys + (virt - entry.virt) : nullptr, length};
            if (memory.valid() && cpuAccessCallback) cpuAccessCallback(memory);
            if (memory.valid() && !ranges.empty() && ranges.back().valid() &&
                reinterpret_cast<uintptr_t>(ranges.back().data()) + ranges.back().size() == reinterpret_cast<uintptr_t>(memory.data()))
                ranges.back() = {ranges.back().data(), ranges.back().size() + length};
            else
                ranges.push_back(memory);
            virt += length;
            size -= length;
        }
        return ranges;
    }

    MM_MEMBER()::FlatMemoryManager() = default;
    MM_MEMBER()::~FlatMemoryManager() = default;

    MM_MEMBER(void)::Read(u8 *destination, VaType virt, VaType size, std::function<void(span<u8>)> cpuAccessCallback) {
        std::shared_lock lock{this->blockMutex};
        ValidateRange(virt, size);
        while (size) {
            const auto mapping{LookupMappingLocked(virt, size)};
            const auto length{mapping.memory.size()};
            if (mapping.kind == MemoryMappingKind::Unmapped)
                throw exception("Page fault at 0x{:X}", virt);
            if (mapping.kind == MemoryMappingKind::Sparse) {
                std::memset(destination, 0, length);
            } else {
                if (cpuAccessCallback) cpuAccessCallback(mapping.memory);
                std::memcpy(destination, mapping.memory.data(), length);
            }
            destination += length;
            virt += length;
            size -= length;
        }
    }

    MM_MEMBER(void)::Write(VaType virt, u8 *source, VaType size, std::function<void(span<u8>)> cpuAccessCallback) {
        std::shared_lock lock{this->blockMutex};
        ValidateRange(virt, size);
        while (size) {
            const auto mapping{LookupMappingLocked(virt, size)};
            const auto length{mapping.memory.size()};
            if (mapping.kind == MemoryMappingKind::Unmapped)
                throw exception("Page fault at 0x{:X}", virt);
            if (mapping.kind == MemoryMappingKind::Resident) {
                if (cpuAccessCallback) cpuAccessCallback(mapping.memory);
                std::memcpy(mapping.memory.data(), source, length);
            }
            source += length;
            virt += length;
            size -= length;
        }
    }

    MM_MEMBER(void)::Copy(VaType dst, VaType src, VaType size, std::function<void(span<u8>)> cpuAccessCallback) {
        std::shared_lock lock{this->blockMutex};
        ValidateRange(src, size);
        ValidateRange(dst, size);
        while (size) {
            const auto source{LookupMappingLocked(src, size)};
            const auto destination{LookupMappingLocked(dst, source.memory.size())};
            const auto length{destination.memory.size()};
            if (source.kind == MemoryMappingKind::Unmapped || destination.kind == MemoryMappingKind::Unmapped)
                throw exception("Page fault during GPU copy: 0x{:X} -> 0x{:X}", src, dst);
            if (destination.kind == MemoryMappingKind::Resident) {
                if (cpuAccessCallback) cpuAccessCallback(destination.memory);
                if (source.kind == MemoryMappingKind::Sparse) {
                    std::memset(destination.memory.data(), 0, length);
                } else {
                    span<u8> input{source.memory.data(), length};
                    if (cpuAccessCallback) cpuAccessCallback(input);
                    std::memcpy(destination.memory.data(), input.data(), length);
                }
            }
            src += length;
            dst += length;
            size -= length;
        }
    }

    ALLOC_MEMBER()::FlatAllocator(VaType vaStart, VaType vaLimit) : Base(vaLimit), vaStart(vaStart), currentLinearAllocEnd(vaStart) {}

    ALLOC_MEMBER(VaType)::Allocate(VaType size) {
        TRACE_EVENT("containers", "FlatAllocator::Allocate");

        std::scoped_lock lock(this->blockMutex);

        VaType allocStart{UnmappedVa};
        VaType allocEnd{currentLinearAllocEnd + size};

        // Avoid searching backwards in the address space if possible
        if (allocEnd >= currentLinearAllocEnd && allocEnd <= this->vaLimit) {
            auto allocEndSuccessor{std::lower_bound(this->blocks.begin(), this->blocks.end(), allocEnd)};
            if (allocEndSuccessor == this->blocks.begin())
                throw exception("First block in AS map is invalid!");

            auto allocEndPredecessor{std::prev(allocEndSuccessor)};
            if (allocEndPredecessor->virt <= currentLinearAllocEnd) {
                allocStart = currentLinearAllocEnd;
            } else {
                // Skip over fixed any mappings in front of us
                while (allocEndSuccessor != this->blocks.end()) {
                    if (allocEndSuccessor->virt - allocEndPredecessor->virt < size || allocEndPredecessor->Mapped() ) {
                        allocStart = allocEndPredecessor->virt;
                        break;
                    }

                    allocEndPredecessor = allocEndSuccessor++;

                    // Use the VA limit to calculate if we can fit in the final block since it has no successor
                    if (allocEndSuccessor == this->blocks.end()) {
                        allocEnd = allocEndPredecessor->virt + size;

                        if (allocEnd >= allocEndPredecessor->virt && allocEnd <= this->vaLimit)
                            allocStart = allocEndPredecessor->virt;
                    }
                }
            }
        }

        if (allocStart != UnmappedVa) {
            currentLinearAllocEnd = allocStart + size;
        } else {  // If linear allocation overflows the AS then find a gap
            if (this->blocks.size() <= 2)
                throw exception("Unexpected allocator state!");

            auto searchPredecessor{std::next(this->blocks.begin())};
            auto searchSuccessor{std::next(searchPredecessor)};

            while (searchSuccessor != this->blocks.end() &&
                (searchSuccessor->virt - searchPredecessor->virt < size || searchPredecessor->Mapped())) {
                searchPredecessor = searchSuccessor++;
            }

            if (searchSuccessor != this->blocks.end())
                allocStart = searchPredecessor->virt;
            else
                return {}; // AS is full
        }


        this->MapLocked(allocStart, true, size, {});
        return allocStart;
    }

    ALLOC_MEMBER(void)::AllocateFixed(VaType virt, VaType size) {
        std::scoped_lock lock(this->blockMutex);
        this->MapLocked(virt, true, size, {});
    }

    ALLOC_MEMBER(void)::Free(VaType virt, VaType size) {
        std::scoped_lock lock(this->blockMutex);
        this->UnmapLocked(virt, size);
    }
}
