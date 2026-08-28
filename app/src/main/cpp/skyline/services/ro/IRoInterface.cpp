// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <algorithm>
#include <fmt/ranges.h>
#include <mbedtls/sha256.h>
#include <loader/nro.h>
#include <nce.h>
#include "IRoInterface.h"

namespace skyline::service::ro {
    namespace {
        bool RangeOverlaps(uintptr_t address, size_t size, const kernel::MemoryRegion &region) {
            const auto end{address + size};
            const auto regionStart{reinterpret_cast<uintptr_t>(region.guest.data())};
            const auto regionEnd{reinterpret_cast<uintptr_t>(region.guest.end().base())};
            return address < regionEnd && regionStart < end;
        }
    }

    IRoInterface::IRoInterface(const DeviceState &state, ServiceManager &manager) : BaseService(state, manager) {}

    Result IRoInterface::LoadModule(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        if (state.process->memory.addressSpaceType == memory::AddressSpaceType::AddressSpace32Bit)
            throw exception("ldr:ro LoadModule is not yet supported on 32-bit processes!");

        u64 pid{request.Pop<u64>()};
        u64 nroAddress{request.Pop<u64>()};
        u64 nroSize{request.Pop<u64>()};
        u64 bssAddress{request.Pop<u64>()};
        u64 bssSize{request.Pop<u64>()};

        if (!util::IsPageAligned(nroAddress) || !util::IsPageAligned(bssAddress))
            return result::InvalidAddress;

        if (!util::IsPageAligned(nroSize) || !nroSize || !util::IsPageAligned(bssSize))
            return result::InvalidSize;

        if (nroSize < sizeof(loader::NroHeader))
            return result::InvalidNro;

        auto data{span{reinterpret_cast<u8 *>(nroAddress), nroSize}};
        auto &header{data.as<loader::NroHeader>()};

        if (header.magic != util::MakeMagic<u32>("NRO0") || header.size != nroSize)
            return result::InvalidNro;

        std::array<u8, 0x20> hash{};
        mbedtls_sha256_ret(data.data(), data.size(), hash.data(), 0);

        if (std::any_of(loadedNros.begin(), loadedNros.end(), [&hash](const LoadedNro &nro) {
                return nro.hash == hash;
            }))
            return result::AlreadyLoaded;

        // We don't handle NRRs here since they're purely used for signature verification which we will never do
        if (bssSize != header.bssSize)
            return result::InvalidNro;

        const auto segmentIsValid{[&data](const loader::NroSegmentHeader &segment) {
            return segment.offset <= data.size() && segment.size <= data.size() - segment.offset;
        }};

        if (!segmentIsValid(header.text) || !segmentIsValid(header.ro) || !segmentIsValid(header.data))
            return result::InvalidNro;

        if (header.text.offset != 0 ||
            static_cast<u64>(header.text.offset) + header.text.size != header.ro.offset ||
            static_cast<u64>(header.ro.offset) + header.ro.size != header.data.offset ||
            static_cast<u64>(header.data.offset) + header.data.size != nroSize)
            return result::InvalidNro;

        if (!util::IsPageAligned(header.text.size) ||
            !util::IsPageAligned(header.ro.size) ||
            !util::IsPageAligned(header.data.size) ||
            !util::IsPageAligned(header.bssSize))
            return result::InvalidNro;

        loader::Executable executable{};

        executable.text.offset = 0;
        executable.text.contents.resize(header.text.size);
        span(executable.text.contents).copy_from(data.subspan(header.text.offset, header.text.size));

        executable.ro.offset = header.text.size;
        executable.ro.contents.resize(header.ro.size);
        span(executable.ro.contents).copy_from(data.subspan(header.ro.offset, header.ro.size));

        executable.data.offset = header.text.size + header.ro.size;
        executable.data.contents.resize(header.data.size);
        span(executable.data.contents).copy_from(data.subspan(header.data.offset, header.data.size));

        executable.bssSize = header.bssSize;

        if (header.dynsym.offset > header.ro.offset && header.dynsym.offset + header.dynsym.size < header.ro.offset + header.ro.size && header.dynstr.offset > header.ro.offset && header.dynstr.offset + header.dynstr.size < header.ro.offset + header.ro.size) {
            executable.dynsym = {header.dynsym.offset, header.dynsym.size};
            executable.dynstr = {header.dynstr.offset, header.dynstr.size};
        }

        u64 textSize{executable.text.contents.size()};
        u64 roSize{executable.ro.contents.size()};
        u64 dataSize{executable.data.contents.size() + executable.bssSize};

        auto patch{state.nce->GetPatchData(executable.text.contents)};
        size_t size{patch.size + textSize + roSize + dataSize};

        const auto baseStart{reinterpret_cast<uintptr_t>(state.process->memory.base.data())};
        const auto baseEnd{reinterpret_cast<uintptr_t>(state.process->memory.base.end().base())};

        if (size > baseEnd - baseStart)
            return result::OutOfAddressSpace;

        const auto candidateIsValid{[&](uintptr_t candidateAddress) {
            if (candidateAddress < baseStart || candidateAddress > baseEnd - size)
                return false;

            if (RangeOverlaps(candidateAddress, size, state.process->memory.heap) ||
                RangeOverlaps(candidateAddress, size, state.process->memory.alias))
                return false;

            auto candidate{reinterpret_cast<u8 *>(candidateAddress)};
            auto desc{state.process->memory.GetChunk(candidate)};
            if (!desc || desc->second.state != memory::states::Unmapped)
                return false;

            const auto descStart{reinterpret_cast<uintptr_t>(desc->first)};
            const auto descEnd{descStart + desc->second.size};

            return candidateAddress >= descStart &&
                   candidateAddress <= descEnd &&
                   size <= descEnd - candidateAddress;
        }};

        u8 *ptr{};

        constexpr size_t RandomPlacementAttempts{4096};
        for (size_t attempt{}; attempt < RandomPlacementAttempts && !ptr; attempt++) {
            auto randomPtr{util::RandomNumber(state.process->memory.base.data(), std::prev(state.process->memory.base.end()).base())};
            const auto randomAddress{reinterpret_cast<uintptr_t>(randomPtr)};

            if (randomAddress < baseStart + size)
                continue;

            const auto candidateAddress{util::AlignDown(randomAddress - size, constant::PageSize)};
            if (candidateIsValid(candidateAddress))
                ptr = reinterpret_cast<u8 *>(candidateAddress);
        }

        // If ASLR sampling did not hit a suitable hole, fall back to a complete chunk scan.
        for (uintptr_t cursor{baseStart}; !ptr && cursor < baseEnd;) {
            const auto candidateAddress{util::AlignUp(cursor, constant::PageSize)};
            if (candidateAddress > baseEnd - size)
                break;

            auto desc{state.process->memory.GetChunk(reinterpret_cast<u8 *>(candidateAddress))};
            if (!desc)
                break;

            const auto descStart{reinterpret_cast<uintptr_t>(desc->first)};
            const auto descEnd{std::min(baseEnd, descStart + desc->second.size)};

            if (descEnd <= cursor)
                break;

            if (desc->second.state == memory::states::Unmapped) {
                auto scan{std::max(candidateAddress, descStart)};

                while (scan < descEnd && size <= descEnd - scan) {
                    if (RangeOverlaps(scan, size, state.process->memory.alias)) {
                        scan = util::AlignUp(reinterpret_cast<uintptr_t>(state.process->memory.alias.guest.end().base()), constant::PageSize);
                        continue;
                    }

                    if (RangeOverlaps(scan, size, state.process->memory.heap)) {
                        scan = util::AlignUp(reinterpret_cast<uintptr_t>(state.process->memory.heap.guest.end().base()), constant::PageSize);
                        continue;
                    }

                    if (candidateIsValid(scan)) {
                        ptr = reinterpret_cast<u8 *>(scan);
                        break;
                    }

                    break;
                }
            }

            cursor = descEnd;
        }

        if (!ptr)
            return result::OutOfAddressSpace;

        auto loadInfo{state.loader->LoadExecutable(state.process, state, executable, static_cast<size_t>(ptr - state.process->memory.base.data()), util::HexDump(hash) + ".nro")};

        loadedNros.push_back({
            .hash = hash,
            .allocationBase = loadInfo.base,
            .allocationSize = loadInfo.size,
            .mappedAddress = static_cast<u8 *>(loadInfo.entry),
        });

        response.Push(loadInfo.entry);
        return {};
    }

    Result IRoInterface::UnloadModule(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        u64 pid{request.Pop<u64>()};
        u64 nroAddress{request.Pop<u64>()};

        if (!util::IsPageAligned(nroAddress))
            return result::InvalidAddress;

        auto mappedAddress{reinterpret_cast<u8 *>(nroAddress)};
        auto loadedNro{std::find_if(loadedNros.begin(), loadedNros.end(), [mappedAddress](const LoadedNro &nro) {
            return nro.mappedAddress == mappedAddress;
        })};

        if (loadedNro == loadedNros.end())
            return result::NotLoaded;

        span<u8> allocation{loadedNro->allocationBase, loadedNro->allocationSize};
        auto hostAllocationBase{state.process->memory.GetHostSpan(allocation).data()};

        if (!state.process->memory.UnmapMemory(allocation))
            return result::InvalidCurrentMemory;

        if (!state.loader->UnloadExecutable(hostAllocationBase))
            LOGW("UnloadModule: symbolic information for NRO @ {} was not found", fmt::ptr(mappedAddress));

        LOGD("Unloaded NRO @ {}, Allocation: {} - {}, Size: 0x{:X}",
             fmt::ptr(mappedAddress),
             fmt::ptr(loadedNro->allocationBase),
             fmt::ptr(loadedNro->allocationBase + loadedNro->allocationSize),
             loadedNro->allocationSize);

        loadedNros.erase(loadedNro);
        return {};
    }

    Result IRoInterface::RegisterModuleInfo(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        return {};
    }

    Result IRoInterface::UnregisterModuleInfo(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        return {};
    }

    Result IRoInterface::RegisterProcessHandle(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        return {};
    }

    Result IRoInterface::RegisterProcessModuleInfo(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        return {};
    }
}
