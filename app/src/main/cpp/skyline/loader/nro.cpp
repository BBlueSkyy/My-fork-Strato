// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <nce.h>
#include <kernel/types/KProcess.h>
#include <kernel/svc.h>
#include <vfs/nacp.h>
#include <vfs/region_backing.h>
#include "nro.h"

namespace skyline::loader {
    namespace {
        struct HomebrewConfigEntry {
            u32 key;
            u32 flags;
            u64 value[2];
        };
        static_assert(sizeof(HomebrewConfigEntry) == 0x18);

        enum class HomebrewConfigKey : u32 {
            EndOfList = 0,
            MainThreadHandle = 1,
            Argv = 5,
            SyscallAvailableHint = 6,
            AppletType = 7,
            ProcessHandle = 10,
        };

        constexpr u64 AppletTypeSystemApplication{4};
        constexpr u64 EnvAppletFlagApplicationOverride{1};
        constexpr u32 SvcExitProcessInstruction{0xD40000E1};

        std::array<u64, 2> GetSyscallHints() {
            std::array<u64, 2> hints{};
            for (size_t index{}; index < kernel::svc::SvcTable.size(); index++) {
                if (kernel::svc::SvcTable[index].function)
                    hints[index / 64] |= 1ULL << (index % 64);
            }
            return hints;
        }
    }

    NroLoader::NroLoader(std::shared_ptr<vfs::Backing> pBacking, std::optional<std::string> pLaunchPath)
        : backing(std::move(pBacking)), launchPath(std::move(pLaunchPath)) {
        header = backing->Read<NroHeader>();

        if (header.magic != util::MakeMagic<u32>("NRO0"))
            throw exception("Invalid NRO magic! 0x{0:X}", header.magic);

        // The homebrew asset section is appended to the end of an NRO file
        if (backing->size > header.size) {
            assetHeader = backing->Read<NroAssetHeader>(header.size);

            if (assetHeader.magic != util::MakeMagic<u32>("ASET"))
                throw exception("Invalid ASET magic! 0x{0:X}", assetHeader.magic);

            NroAssetSection &nacpHeader{assetHeader.nacp};
            nacp.emplace(std::make_shared<vfs::RegionBacking>(backing, header.size + nacpHeader.offset, nacpHeader.size));

            NroAssetSection &romFsHeader{assetHeader.romFs};
            romFs = std::make_shared<vfs::RegionBacking>(backing, header.size + romFsHeader.offset, romFsHeader.size);
        }
    }

    std::vector<u8> NroLoader::GetIcon(language::ApplicationLanguage language) {
        NroAssetSection &segmentHeader{assetHeader.icon};
        std::vector<u8> buffer(segmentHeader.size);

        backing->Read(buffer, header.size + segmentHeader.offset);
        return buffer;
    }

    std::vector<u8> NroLoader::GetSegment(const NroSegmentHeader &segment) {
        std::vector<u8> buffer(segment.size);

        backing->Read(buffer, segment.offset);
        return buffer;
    }

    void *NroLoader::LoadProcessData(const std::shared_ptr<kernel::type::KProcess> &process, const DeviceState &state) {
        Executable executable{};

        executable.text.contents = GetSegment(header.text);
        executable.text.offset = 0;

        executable.ro.contents = GetSegment(header.ro);
        executable.ro.offset = header.text.size;

        executable.data.contents = GetSegment(header.data);
        executable.data.offset = header.text.size + header.ro.size;

        const size_t homebrewDataOffset{util::AlignUp(executable.data.contents.size() + header.bssSize, constant::PageSize)};
        executable.bssSize = homebrewDataOffset + constant::PageSize - executable.data.contents.size();

        if (header.dynsym.offset > header.ro.offset && header.dynsym.offset + header.dynsym.size < header.ro.offset + header.ro.size && header.dynstr.offset > header.ro.offset && header.dynstr.offset + header.dynstr.size < header.ro.offset + header.ro.size) {
            executable.dynsym = {header.dynsym.offset, header.dynsym.size};
            executable.dynstr = {header.dynstr.offset, header.dynstr.size};
        }

        std::optional<size_t> exitProcessOffset;
        for (size_t offset{}; offset + sizeof(u32) <= executable.text.contents.size(); offset += sizeof(u32)) {
            u32 instruction{};
            std::memcpy(&instruction, executable.text.contents.data() + offset, sizeof(instruction));
            if (instruction == SvcExitProcessInstruction) {
                exitProcessOffset = offset;
                break;
            }
        }

        state.process->npdm.meta.flags.is64Bit = true;
        state.process->memory.InitializeVmm(memory::AddressSpaceType::AddressSpace39Bit);
        auto applicationName{nacp ? nacp->GetApplicationName(nacp->GetFirstSupportedTitleLanguage()) : ""};
        auto loadInfo{LoadExecutable(process, state, executable, 0, applicationName.empty() ? "main.nro" : applicationName + ".nro")};

        const u64 programAddress{reinterpret_cast<u64>(loadInfo.entry)};
        homebrewConfigAddress = programAddress + executable.data.offset + homebrewDataOffset;

        std::vector<HomebrewConfigEntry> entries;
        const size_t mainThreadEntryIndex{entries.size()};
        entries.push_back({static_cast<u32>(HomebrewConfigKey::MainThreadHandle), 0, {0, 0}});
        const size_t processEntryIndex{entries.size()};
        entries.push_back({static_cast<u32>(HomebrewConfigKey::ProcessHandle), 0, {0, 0}});
        entries.push_back({static_cast<u32>(HomebrewConfigKey::AppletType), 0,
                           {AppletTypeSystemApplication, EnvAppletFlagApplicationOverride}});

        mainThreadHandleAddress = homebrewConfigAddress + mainThreadEntryIndex * sizeof(HomebrewConfigEntry) + offsetof(HomebrewConfigEntry, value);
        processHandleAddress = homebrewConfigAddress + processEntryIndex * sizeof(HomebrewConfigEntry) + offsetof(HomebrewConfigEntry, value);

        const auto syscallHints{GetSyscallHints()};
        entries.push_back({static_cast<u32>(HomebrewConfigKey::SyscallAvailableHint), 0,
                           {syscallHints[0], syscallHints[1]}});

        std::string argv;
        std::optional<size_t> argvEntryIndex;
        if (launchPath) {
            argvEntryIndex = entries.size();
            entries.push_back({static_cast<u32>(HomebrewConfigKey::Argv), 0, {0, 0}});
            argv = "\"";
            argv += *launchPath;
            argv += "\"";
            argv.push_back('\0');
        }

        entries.push_back({static_cast<u32>(HomebrewConfigKey::EndOfList), 0, {0, 0}});

        const size_t entriesSize{entries.size() * sizeof(HomebrewConfigEntry)};
        if (entriesSize + argv.size() > constant::PageSize)
            throw exception("Homebrew ABI block exceeds one page");

        if (argvEntryIndex)
            entries[*argvEntryIndex].value[1] = homebrewConfigAddress + entriesSize;

        auto *configHost{process->memory.TranslateVirtualPointer<u8 *>(homebrewConfigAddress)};
        std::memset(configHost, 0, constant::PageSize);
        std::memcpy(configHost, entries.data(), entriesSize);
        if (!argv.empty())
            std::memcpy(configHost + entriesSize, argv.data(), argv.size());

        const u64 returnAddress{exitProcessOffset ? programAddress + *exitProcessOffset : 0};
        if (!exitProcessOffset)
            LOGW("NRO does not contain svcExitProcess; returning from main will fault");

        auto *trampolineGuest{loadInfo.base + loadInfo.size};
        process->memory.MapCodeMemory(span<u8>{trampolineGuest, constant::PageSize}, memory::Permission{true, false, true});
        auto *trampolineHost{process->memory.TranslateVirtualPointer<u8 *>(reinterpret_cast<u64>(trampolineGuest))};
        std::memset(trampolineHost, 0, constant::PageSize);

        // Homebrew ABI entry: X0 already contains the config pointer supplied to CreateThread.
        // The trampoline fixes X1 to -1, installs the loader-return address in LR, then branches
        // to the real NRO entry. X16 is an ABI scratch register and is only used for the branch.
        constexpr std::array<u32, 4> trampoline{
            0x92800001, // mov x1, #-1
            0x5800007E, // ldr x30, [pc, #12]
            0x58000090, // ldr x16, [pc, #16]
            0xD61F0200, // br x16
        };
        std::memcpy(trampolineHost, trampoline.data(), sizeof(trampoline));
        std::memcpy(trampolineHost + 0x10, &returnAddress, sizeof(returnAddress));
        std::memcpy(trampolineHost + 0x18, &programAddress, sizeof(programAddress));
        __builtin___clear_cache(reinterpret_cast<char *>(trampolineHost),
                                reinterpret_cast<char *>(trampolineHost + 0x20));

        state.process->memory.InitializeRegions(span<u8>{loadInfo.base, loadInfo.size + constant::PageSize});
        return trampolineGuest;
    }

    void NroLoader::OnMainThreadCreated(const std::shared_ptr<kernel::type::KProcess> &process, KHandle handle) {
        if (mainThreadHandleAddress) {
            auto *mainThreadHandle{process->memory.TranslateVirtualPointer<u64 *>(mainThreadHandleAddress)};
            *mainThreadHandle = handle;
        }

        if (processHandleAddress) {
            const KHandle processHandle{process->InsertSelfHandle()};
            auto *processHandleValue{process->memory.TranslateVirtualPointer<u64 *>(processHandleAddress)};
            *processHandleValue = processHandle;
        }
    }
}
