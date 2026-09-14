// SPDX-License-Identifier: MPL-2.0
// Synthetic format fixtures. All key material below is generated test data, never console keys.
#pragma once
#include <lz4.h>
#include <mbedtls/cipher.h>
#include <vfs/rom_filesystem.h>
#include <vfs/partition_filesystem.h>
#include <loader/program_content.h>

template<class T> void Put(std::vector<u8> &bytes, size_t offset, const T &value) {
    Check(offset <= bytes.size() && sizeof(T) <= bytes.size() - offset, "Fixture write outside bounds");
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}
inline size_t Align(size_t n, size_t alignment = 0x200) { return (n + alignment - 1) & ~(alignment - 1); }
inline std::vector<u8> RomBytes(size_t size = 0x1000, u8 marker = 0x51) {
    std::vector<u8> data(size, marker);
    Put(data, 0, RomFileSystem::RomFsHeader{0x50, 0x50, 4, 0x54, 0x18, 0x6c, 4, 0x70, 0, 0x200});
    return data;
}
inline NCASectionHeader RomHeader(size_t offset, size_t size, u32 count = 2) {
    NCASectionHeader section{};
    section.raw.header = {2, NcaSectionFsType::RomFs, NcaSectionHashType::HierarchicalIntegrity, NcaSectionEncryptionType::None, 0, {}};
    section.romfs.ivfc.magic = util::MakeMagic<u32>("IVFC");
    section.romfs.ivfc.magicNumber = 0x20000;
    section.romfs.ivfc.masterHashSize = 0x20;
    section.romfs.ivfc.levelCount = count;
    for (size_t i{}; i < count - 2; ++i)
        section.romfs.ivfc.levels[i] = {i * 0x20, 0x20, 5, 0};
    section.romfs.ivfc.levels[count - 2] = {offset, size, 12, 0};
    return section;
}
inline std::vector<u8> PfsBytes(const std::vector<std::pair<std::string, std::vector<u8>>> &files) {
    size_t stringSize{}, totalData{};
    for (const auto &[name, bytes] : files) { stringSize += name.size() + 1; totalData += bytes.size(); }
    const size_t stringStart{0x10 + files.size() * 0x18};
    const size_t dataStart{stringStart + stringSize};
    std::vector<u8> result(dataStart + totalData);
    Put(result, 0, util::MakeMagic<u32>("PFS0"));
    Put(result, 4, static_cast<u32>(files.size()));
    Put(result, 8, static_cast<u32>(stringSize));
    size_t nameOffset{}, dataOffset{};
    for (size_t i{}; i < files.size(); ++i) {
        const auto &[name, data] = files[i];
        Put(result, 0x10 + i * 0x18, static_cast<u64>(dataOffset));
        Put(result, 0x18 + i * 0x18, static_cast<u64>(data.size()));
        Put(result, 0x20 + i * 0x18, static_cast<u32>(nameOffset));
        std::memcpy(result.data() + stringStart + nameOffset, name.c_str(), name.size() + 1);
        std::memcpy(result.data() + dataStart + dataOffset, data.data(), data.size());
        nameOffset += name.size() + 1;
        dataOffset += data.size();
    }
    return result;
}
inline std::vector<u8> ExeBytes(u8 marker, bool executable = true) {
    if (!executable)
        return PfsBytes({{"NintendoLogo.png", {marker}}, {"StartupMovie.gif", {marker}}});
    return PfsBytes({{"main", {marker}}, {"main.npdm", {marker}}, {"sdk", {marker}}, {"rtld", {marker}}});
}
inline NCASectionHeader ExeHeader(size_t size) {
    NCASectionHeader section{};
    section.raw.header = {2, NcaSectionFsType::PFS0, NcaSectionHashType::HierarchicalSha256, NcaSectionEncryptionType::None, 0, {}};
    section.pfs0.layerCount = 2;
    section.pfs0.hashTableSize = 0x20;
    section.pfs0.pfs0HeaderOffset = 0x200;
    section.pfs0.pfs0Size = size;
    return section;
}
inline std::vector<u8> WithPrefix(const std::vector<u8> &data, size_t offset = 0x200) {
    std::vector<u8> result(Align(offset + data.size()));
    std::memcpy(result.data() + offset, data.data(), data.size());
    return result;
}
inline std::vector<u8> Crypt(const std::vector<u8> &data, span<u8> key, mbedtls_cipher_type_t type, std::array<u8, 16> iv = {}) {
    mbedtls_cipher_context_t ctx;
    mbedtls_cipher_init(&ctx);
    Check(mbedtls_cipher_setup(&ctx, mbedtls_cipher_info_from_type(type)) == 0, "Fixture cipher setup failed");
    Check(mbedtls_cipher_setkey(&ctx, key.data(), key.size() * 8, MBEDTLS_ENCRYPT) == 0, "Fixture cipher key failed");
    if (type != MBEDTLS_CIPHER_AES_128_ECB)
        Check(mbedtls_cipher_set_iv(&ctx, iv.data(), iv.size()) == 0, "Fixture IV failed");
    Check(mbedtls_cipher_reset(&ctx) == 0, "Fixture cipher reset failed");
    std::vector<u8> output(data.size() + 16);
    size_t count{};
    Check(mbedtls_cipher_update(&ctx, data.data(), data.size(), output.data(), &count) == 0 && count == data.size(), "Fixture cipher update failed");
    output.resize(count);
    mbedtls_cipher_free(&ctx);
    return output;
}
struct NcaFixture {
    std::shared_ptr<MemoryBacking> backing = std::make_shared<MemoryBacking>(0xc00);
    NCAHeader header{};
    std::array<NCASectionHeader, 4> sections{};
    std::array<std::vector<SubsectionEntry>, 4> counterEntries{};
    NcaFixture() {
        header.magic = util::MakeMagic<u32>("NCA3");
        header.contentType = NCAContentType::Program;
        header.titleId = 0x100000000ULL; // Arbitrary synthetic ID; selection must also work with other IDs.
    }
    void Add(size_t index, NCASectionHeader section, const std::vector<u8> &data) {
        const size_t start{backing->data.size()}, end{Align(start + data.size())};
        backing->data.resize(end); backing->size = end;
        std::memcpy(backing->data.data() + start, data.data(), data.size());
        header.sectionTables[index].mediaOffset = start / 0x200;
        header.sectionTables[index].mediaEndOffset = end / 0x200;
        sections[index] = section;
    }
    void Finalize(bool encrypt = false, const std::shared_ptr<crypto::KeyStore> &keys = nullptr) {
        header.size = backing->size;
        if (encrypt) {
            crypto::KeyStore::Key128 dataKey{}, kek{};
            crypto::KeyStore::Key256 headerKey{};
            for (size_t i{}; i < 16; ++i) { dataKey[i] = 0x21 + i; kek[i] = 0x71 + i; }
            for (size_t i{}; i < 32; ++i) headerKey[i] = 0x11 + i;
            keys->headerKey = headerKey;
            keys->areaKeyApplication[0] = kek;
            auto wrapped{Crypt({dataKey.begin(), dataKey.end()}, kek, MBEDTLS_CIPHER_AES_128_ECB)};
            std::copy(wrapped.begin(), wrapped.end(), header.keyArea[2].begin());
            for (size_t index{}; index < sections.size(); ++index) {
                auto &section{sections[index]};
                if (header.sectionTables[index].mediaOffset == 0) continue;
                section.raw.header.encryptionType = counterEntries[index].empty() ? NcaSectionEncryptionType::CTR : NcaSectionEncryptionType::BKTR;
                const u32 generation{17}, secure{0x12345678};
                std::memcpy(section.raw.sectionCtr.data(), &generation, 4);
                std::memcpy(section.raw.sectionCtr.data() + 4, &secure, 4);
                const size_t start{header.sectionTables[index].mediaOffset * 0x200ULL};
                const size_t end{header.sectionTables[index].mediaEndOffset * 0x200ULL};
                std::vector<u8> raw(backing->data.begin() + start, backing->data.begin() + end);
                auto cryptRange = [&](size_t begin, size_t limit, u32 gen, bool clear) {
                    std::array<u8, 16> iv{};
                    for (int i{}; i < 4; ++i) { iv[3-i] = secure >> (8*i); iv[7-i] = gen >> (8*i); }
                    const u64 block{(start + begin) >> 4};
                    for (int i{}; i < 8; ++i) iv[15-i] = block >> (8*i);
                    std::vector<u8> bytes(raw.begin() + begin, raw.begin() + limit);
                    if (!clear) bytes = Crypt(bytes, dataKey, MBEDTLS_CIPHER_AES_128_CTR, iv);
                    std::copy(bytes.begin(), bytes.end(), backing->data.begin() + start + begin);
                };
                cryptRange(0, raw.size(), generation, false);
                for (size_t i{}; i < counterEntries[index].size(); ++i) {
                    const auto &entry{counterEntries[index][i]};
                    const size_t limit{i+1 < counterEntries[index].size() ? counterEntries[index][i+1].addressPatch : section.bktr.subsection.offset};
                    cryptRange(entry.addressPatch, limit, entry.ctr, entry._pad0_[0] != 0);
                }
            }
        }
        Put(backing->data, 0, header);
        Put(backing->data, 0x400, sections);
        if (encrypt) {
            for (size_t i{}; i < 6; ++i) {
                // NCA's sector number occupies the low end of the big-endian 128-bit tweak.
                std::array<u8, 16> tweak{}; tweak[15] = i;
                std::vector<u8> block(backing->data.begin() + i*0x200, backing->data.begin() + (i+1)*0x200);
                auto encrypted{Crypt(block, *keys->headerKey, MBEDTLS_CIPHER_AES_128_XTS, tweak)};
                std::copy(encrypted.begin(), encrypted.end(), backing->data.begin() + i*0x200);
            }
        }
    }
};
inline NcaFixture BaseFixture(bool executable = true, u32 levelCount = 2) {
    NcaFixture f;
    if (executable) { auto exe{ExeBytes(0x41)}; f.Add(0, ExeHeader(exe.size()), WithPrefix(exe)); }
    f.Add(1, RomHeader(0x200, 0x1000, levelCount), WithPrefix(RomBytes()));
    return f;
}
inline NcaFixture PatchFixture(bool executable = true) {
    NcaFixture f;
    if (executable) { auto exe{ExeBytes(0x42)}; f.Add(0, ExeHeader(exe.size()), WithPrefix(exe)); }
    auto section{RomHeader(0x400, 0x1000)};
    section.bktr.relocation = {0x1000, 0x8000, util::MakeMagic<u32>("BKTR"), 1, 3, 0};
    section.bktr.subsection = {0x9000, 0x8000, util::MakeMagic<u32>("BKTR"), 1, 3, 0};
    std::vector<u8> body(0x11000, 0x62);
    Put(body, 0x1000, RelocationBlock{0, 1, 0x1400, {0}});
    RelocationBucketRaw reloc{};
    reloc.numberEntries = 3; reloc.endOffset = 0x1400;
    reloc.relocationEntries[0] = {0, 0, 0};
    reloc.relocationEntries[1] = {0x400, 0x200, 0};
    reloc.relocationEntries[2] = {0xc00, 0x100, 1};
    Put(body, 0x5000, reloc);
    Put(body, 0x9000, SubsectionBlock{0, 1, 0x9000, {0}});
    SubsectionBucketRaw subsection{};
    subsection.numberEntries = 3; subsection.endOffset = 0x9000;
    subsection.subsectionEntries[0] = {0, {}, 9};
    subsection.subsectionEntries[1] = {0x800, {}, 10};
    subsection.subsectionEntries[2] = {0x1000, {}, 42}; // Indirect metadata uses CTR-Ex, not header generation 17.
    Put(body, 0xd000, subsection);
    f.Add(1, section, body);
    f.counterEntries[1] = {subsection.subsectionEntries.begin(), subsection.subsectionEntries.begin() + 3};
    return f;
}
inline std::vector<u8> ReadBytes(const std::shared_ptr<Backing> &backing) {
    Check(backing != nullptr, "Null backing");
    std::vector<u8> result(backing->size);
    Check(backing->Read(result) == result.size(), "Short fixture result read");
    return result;
}
