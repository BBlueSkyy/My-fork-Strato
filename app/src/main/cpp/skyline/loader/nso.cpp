// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/strato-emu/)

#include <lz4.h>
#include <limits>
#include <nce.h>
#include <os.h>
#include <mods/ips.h>
#include <mods/pchtxt.h>
#include <kernel/types/KProcess.h>
#include <boost/regex/v5/regex.hpp>
#include "nso.h"
#include <loader/startup_trace.h>
#include "zbic.h"

namespace skyline::loader {
    NsoLoader::NsoLoader(std::shared_ptr<vfs::Backing> pBacking) : backing(std::move(pBacking)) {
        u32 magic{backing->Read<u32>()};

        if (magic != util::MakeMagic<u32>("NSO0"))
            throw exception("Invalid NSO magic! 0x{0:X}", magic);
    }

    std::vector<u8> NsoLoader::GetSegment(const std::shared_ptr<vfs::Backing> &backing, const NsoSegmentHeader &segment, bool isCompressed, u32 compressedSize, bool useZbicCompression) {
        StartupTrace trace{fmt::format("NSO segment offset=0x{:X} size=0x{:X} codec={}", segment.fileOffset, segment.decompressedSize,
                                      isCompressed ? (useZbicCompression ? "ZBIC" : "LZ4") : "raw")};
        if (isCompressed && !compressedSize && segment.decompressedSize)
            throw exception("Invalid NSO segment compressed size: zero for a nonempty segment");

        if (isCompressed && !useZbicCompression &&
            (compressedSize > std::numeric_limits<int>::max() || segment.decompressedSize > std::numeric_limits<int>::max()))
            throw exception("NSO LZ4 segment exceeds signed size limit (0x{:X}/0x{:X})", compressedSize, segment.decompressedSize);

        const size_t fileSize{isCompressed ? compressedSize : segment.decompressedSize};
        if (segment.fileOffset > backing->size || fileSize > backing->size - segment.fileOffset)
            throw exception("Invalid NSO segment file range (offset=0x{:X}, size=0x{:X}, file=0x{:X})", segment.fileOffset, fileSize, backing->size);

        std::vector<u8> outputBuffer(segment.decompressedSize);
        if (isCompressed && compressedSize) {
            std::vector<u8> compressedBuffer(compressedSize);
            if (backing->Read(compressedBuffer, segment.fileOffset) != compressedBuffer.size())
                throw exception("NSO compressed segment short read at 0x{:X}", segment.fileOffset);

            if (useZbicCompression) {
                zbic::Decompress(compressedBuffer, outputBuffer);
            } else {
                const int result{LZ4_decompress_safe(reinterpret_cast<char *>(compressedBuffer.data()), reinterpret_cast<char *>(outputBuffer.data()), static_cast<int>(compressedSize), static_cast<int>(segment.decompressedSize))};
                if (result != static_cast<int>(segment.decompressedSize))
                    throw exception("NSO LZ4 decompression failed or size mismatch (result={}, expected=0x{:X})", result, segment.decompressedSize);
            }
        } else if (!outputBuffer.empty()) {
            if (backing->Read(outputBuffer, segment.fileOffset) != outputBuffer.size())
                throw exception("NSO segment short read at 0x{:X}", segment.fileOffset);
        }

        trace.Complete();
        return outputBuffer;
    }

    Loader::ExecutableLoadInfo NsoLoader::LoadNso(Loader *loader, const std::shared_ptr<vfs::Backing> &backing, const std::shared_ptr<kernel::type::KProcess> &process, const DeviceState &state, size_t offset, const std::string &name, bool dynamicallyLinked) {
        StartupTrace trace{fmt::format("NSO load {}", name.empty() ? "main" : name)};
        NsoHeader header{};
        if (backing->Read(span<u8>{reinterpret_cast<u8 *>(&header), sizeof(header)}) != sizeof(header))
            throw exception("NSO header short read");

        if (header.magic != util::MakeMagic<u32>("NSO0"))
            throw exception("Invalid NSO magic! 0x{0:X}", header.magic);

        Executable executable{};

        executable.text.contents = GetSegment(backing, header.text, header.flags.textCompressed, header.textCompressedSize, header.flags.useZbicCompression);
        executable.text.contents.resize(util::AlignUp(executable.text.contents.size(), constant::PageSize));
        executable.text.offset = header.text.memoryOffset;

        executable.ro.contents = GetSegment(backing, header.ro, header.flags.roCompressed, header.roCompressedSize, header.flags.useZbicCompression);
        executable.ro.contents.resize(util::AlignUp(executable.ro.contents.size(), constant::PageSize));
        executable.ro.offset = header.ro.memoryOffset;

        executable.data.contents = GetSegment(backing, header.data, header.flags.dataCompressed, header.dataCompressedSize, header.flags.useZbicCompression);
        executable.data.offset = header.data.memoryOffset;

        // Data and BSS are aligned together
        executable.bssSize = util::AlignUp(executable.data.contents.size() + header.bssSize, constant::PageSize) - executable.data.contents.size();

        auto FitsInRo = [&](const NsoRelativeSegmentHeader &range) {
            return range.offset <= header.ro.decompressedSize && range.size <= header.ro.decompressedSize - range.offset;
        };
        if (FitsInRo(header.dynsym) && FitsInRo(header.dynstr)) {
            executable.dynsym = {header.dynsym.offset, header.dynsym.size};
            executable.dynstr = {header.dynstr.offset, header.dynstr.size};
        }

        const u64 titleId{process->npdm.aci0.programId};
        if (titleId) {
            auto pchtxtPatches{mods::CollectPchtxtPatches(state.os->publicAppFilesPath, titleId, header.buildId)};
            auto ipsPatches{mods::CollectIpsPatches(state.os->publicAppFilesPath, titleId, header.buildId)};
            if (!pchtxtPatches.empty() || !ipsPatches.empty()) {
                const size_t programSize{std::max({
                    static_cast<size_t>(header.text.memoryOffset) + header.text.decompressedSize,
                    static_cast<size_t>(header.ro.memoryOffset) + header.ro.decompressedSize,
                    static_cast<size_t>(header.data.memoryOffset) + header.data.decompressedSize,
                })};
                std::vector<u8> image(sizeof(NsoHeader) + programSize);
                std::memcpy(image.data(), &header, sizeof(NsoHeader));
                std::memcpy(image.data() + sizeof(NsoHeader) + header.text.memoryOffset, executable.text.contents.data(), header.text.decompressedSize);
                std::memcpy(image.data() + sizeof(NsoHeader) + header.ro.memoryOffset, executable.ro.contents.data(), header.ro.decompressedSize);
                std::memcpy(image.data() + sizeof(NsoHeader) + header.data.memoryOffset, executable.data.contents.data(), header.data.decompressedSize);

                // PCHTXT and Atmosphere IPS/IPS32 share the same decompressed NSO
                // image. Apply both before LoadExecutable so NCE sees patched code.
                for (const auto &patch : pchtxtPatches) {
                    size_t applied{};
                    for (const auto &write : patch.writes) {
                        if (write.offset < sizeof(NsoHeader) || write.offset > image.size() || write.value.size() > image.size() - static_cast<size_t>(write.offset)) {
                            LOGW("PCHTXT: write outside NSO image in '{}' at 0x{:X}", patch.path.filename().string(), write.offset);
                            continue;
                        }

                        std::memcpy(image.data() + static_cast<size_t>(write.offset), write.value.data(), write.value.size());
                        applied++;
                    }
                    if (applied)
                        LOGI("PCHTXT: applied '{}' to {} ({} writes)", patch.path.filename().string(), name.empty() ? "NSO" : name, applied);
                }

                for (const auto &patch : ipsPatches) {
                    const char *format{patch.ips32 ? "IPS32" : "IPS"};
                    size_t applied{};
                    for (const auto &write : patch.writes) {
                        if (write.offset < sizeof(NsoHeader) || write.offset >= image.size()) {
                            LOGW("{}: write outside NSO image in '{}' at 0x{:X}", format, patch.path.filename().string(), write.offset);
                            continue;
                        }

                        const size_t imageOffset{static_cast<size_t>(write.offset)};
                        const size_t available{image.size() - imageOffset};
                        const size_t writeSize{std::min(write.value.size(), available)};
                        if (!writeSize)
                            continue;

                        std::memcpy(image.data() + imageOffset, write.value.data(), writeSize);
                        applied++;
                        if (writeSize != write.value.size())
                            LOGW("{}: truncated write in '{}' at 0x{:X} from 0x{:X} to 0x{:X} bytes", format, patch.path.filename().string(), write.offset, write.value.size(), writeSize);
                    }
                    if (applied)
                        LOGI("{}: applied '{}' to {} ({} writes)", format, patch.path.filename().string(), name.empty() ? "NSO" : name, applied);
                }

                std::memcpy(executable.text.contents.data(), image.data() + sizeof(NsoHeader) + header.text.memoryOffset, header.text.decompressedSize);
                std::memcpy(executable.ro.contents.data(), image.data() + sizeof(NsoHeader) + header.ro.memoryOffset, header.ro.decompressedSize);
                std::memcpy(executable.data.contents.data(), image.data() + sizeof(NsoHeader) + header.data.memoryOffset, header.data.decompressedSize);
            }
        }

        PrintRoContentsInfo(executable.ro.contents);

        auto loadInfo{loader->LoadExecutable(process, state, executable, offset, name, dynamicallyLinked)};
        trace.Complete();
        return loadInfo;
    }

    void *NsoLoader::LoadProcessData(const std::shared_ptr<kernel::type::KProcess> &process, const DeviceState &state) {
        state.process->npdm.meta.flags.is64Bit = true;
        state.process->memory.InitializeVmm(memory::AddressSpaceType::AddressSpace39Bit);
        auto loadInfo{LoadNso(this, backing, process, state)};
        state.process->memory.InitializeRegions(span<u8>{loadInfo.base, loadInfo.size});
        return loadInfo.entry;
    }

    void NsoLoader::PrintRoContentsInfo(const std::vector<u8> &contents) {
        const boost::regex moduleRegex(R"([a-z]:[\\/][ -~]{5,}\.nss)", boost::regex::icase);
        const boost::regex fsSdkRegex("sdk_version: ([0-9.]*)");
        const boost::regex sdkMwRegex("SDK MW[ -~]{0,256}");

        // Module path, SDK version, and SDK library markers all live near the start of .rodata in every
        // known NSO layout - scanning the full segment (which can be several MB) with unbounded regex
        // quantifiers against it is needlessly expensive and was observed to hang on real-world .rodata
        // sizes, so this is capped to a generous prefix that still comfortably covers those markers
        constexpr size_t MaxScanSize{0x40000}; // 256 KiB
        const size_t scanSize{std::min(contents.size(), MaxScanSize)};

        std::string contentsRaw(contents.begin(), contents.begin() + static_cast<ssize_t>(scanSize));
        std::string modulePath;
        if (contents.size() >= 2 * sizeof(u32) && memcmp(contents.data(), "\x00\x00\x00\x00", 4) == 0) {
            i32 length;
            std::memcpy(&length, &contents[4], sizeof(i32));

            constexpr size_t PathOffset{2 * sizeof(u32)};
            if (length > 0 && static_cast<size_t>(length) <= contents.size() - PathOffset) {
                const auto *path{reinterpret_cast<const char *>(contents.data() + PathOffset)};
                modulePath.assign(path, std::find(path, path + length, '\0'));
            }
        }

        if (modulePath.empty()) {
            boost::smatch moduleMatch;
            if (boost::regex_search(contentsRaw, moduleMatch, moduleRegex))
                modulePath = moduleMatch.str();
        }

        LOGI("Module Path: {}", modulePath);

        boost::smatch fsSdkMatch;
        if (boost::regex_search(contentsRaw, fsSdkMatch, fsSdkRegex))
            LOGI("SDK Version: {}", fsSdkMatch[1].str());

        boost::sregex_iterator sdkMwMatchesBegin(contentsRaw.begin(), contentsRaw.end(), sdkMwRegex);
        boost::sregex_iterator sdkMwMatchesEnd;

        if (sdkMwMatchesBegin != sdkMwMatchesEnd) {
            std::string libContent;

            while (sdkMwMatchesBegin != sdkMwMatchesEnd) {
                libContent += sdkMwMatchesBegin->str() + "\n";
                sdkMwMatchesBegin++;
            }

            LOGI("SDK Libraries: {}", libContent);
        }
    }
}
