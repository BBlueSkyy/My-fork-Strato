// SPDX-License-Identifier: MPL-2.0
#include <iostream>
#include <vfs/bktr.h>
#include <vfs/nca.h>
#include <crypto/key_store.h>
#include <loader/loader.h>
#include <vfs/patch_manager.h>

using namespace skyline;
using namespace skyline::vfs;
namespace skyline::crypto {
// Test keys are populated in memory; no files, console keys or game data are used.
KeyStore::KeyStore(const std::string &) {}
}
namespace skyline::vfs {
// Mod discovery is Android/OS dependent and outside these storage tests.
// The production resolver runs unchanged; fixtures install no user mods.
size_t modResolutionCount{};
PatchManager::PatchManager() = default;
std::shared_ptr<FileSystem> PatchManager::PatchExeFS(const DeviceState &, std::shared_ptr<FileSystem> fs, u64) {
    ++modResolutionCount; return fs;
}
std::shared_ptr<Backing> PatchManager::PatchRomFS(const DeviceState &, std::shared_ptr<Backing> data, u64) {
    ++modResolutionCount; return data;
}
}
class MemoryBacking : public Backing {
public:
    std::vector<u8> data;
    explicit MemoryBacking(size_t size, u8 value = 0) : Backing({true, false, false}, size), data(size, value) {}
    size_t ReadImpl(span<u8> output, size_t offset) override {
        if (offset > size || output.size() > size - offset) throw exception("Test read out of range");
        std::memcpy(output.data(), data.data() + offset, output.size());
        return output.size();
    }
};
void Check(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }

void TestBktrPartition() {
    auto base = std::make_shared<MemoryBacking>(32, 0xBB);
    auto patch = std::make_shared<MemoryBacking>(32, 0xAA);
    RelocationBlock reloc{0, 1, 32, {0}};
    std::vector<RelocationBucket> buckets{{2, 32, {{0, 0, 1}, {16, 0, 0}}}};
    SubsectionBlock sub{0, 1, 32, {0}};
    BKTR bktr(base, patch, reloc, buckets, sub, {{1, 32, {{0, {}, 0}, {32, {}, 0}}}}, false, {}, 0, 0, {});
    std::array<u8, 32> output{};
    Check(bktr.Read(output) == output.size(), "BKTR partition returned the wrong byte count");
    Check(std::all_of(output.begin(), output.begin() + 16, [](u8 v) { return v == 0xAA; }), "Patch bytes differ");
    Check(std::all_of(output.begin() + 16, output.end(), [](u8 v) { return v == 0xBB; }), "Base bytes overwritten across a relocation");
}

class LargeBacking : public Backing {
public:
    size_t lastOffset{};
    LargeBacking() : Backing({true, false, false}, 0x100001000ULL) {}
    size_t ReadImpl(span<u8> output, size_t offset) override {
        lastOffset = offset;
        std::fill(output.begin(), output.end(), 0);
        return output.size();
    }
};
void TestBktrLargeOffset() {
    auto base = std::make_shared<MemoryBacking>(32);
    auto patch = std::make_shared<LargeBacking>();
    BKTR bktr(base, patch, {0, 1, 32, {0}}, {{1, 32, {{0, 0x100000000ULL, 1}}}},
              {0, 1, patch->size, {0}}, {{1, patch->size, {{0, {}, 1}, {patch->size, {}, 0}}}}, true, {}, 0, 0, {});
    std::array<u8, 3> output{};
    bktr.Read(output, 1);
    Check(patch->lastOffset == 0x100000000ULL, "BKTR truncated an unaligned physical offset above 4 GiB");
}
#include "fixtures.h"

void TestBaseAndSectionHoles() {
    auto keys{std::make_shared<crypto::KeyStore>("")};
    auto fixture{BaseFixture(true, 4)};
    // Put RomFS at FS index 3, leaving a real hole at indices 1 and 2.
    fixture.sections[3] = fixture.sections[1]; fixture.sections[1] = {};
    fixture.header.sectionTables[3] = fixture.header.sectionTables[1]; fixture.header.sectionTables[1] = {};
    fixture.Finalize();
    NCA base(fixture.backing, keys);
    Check(ReadBytes(base.romFs) == RomBytes(), "Base RomFS or IVFC data-level selection changed");
    Check(base.exeFs->FileExists("main.npdm") && base.exeFs->FileExists("sdk"), "Base ExeFS incomplete");
    Check(!base.HasBktrSection(), "Base incorrectly classified as patch");
}
void TestReplacementUpdate() {
    auto keys{std::make_shared<crypto::KeyStore>("")};
    auto baseFixture{BaseFixture()}; baseFixture.Finalize();
    auto updateFixture{BaseFixture()};
    auto exe{ExeBytes(0x42)};
    // Replacement Program update with no indirect table.
    updateFixture = NcaFixture{};
    updateFixture.Add(0, ExeHeader(exe.size()), WithPrefix(exe));
    updateFixture.Add(1, RomHeader(0x200, 0x1000), WithPrefix(RomBytes(0x1000, 0x63)));
    updateFixture.Finalize();
    NCA base(baseFixture.backing, keys), patch(updateFixture.backing, keys);
    auto resolved{patch.OpenExeFsWithPatch(base)};
    for (const auto *name : {"main", "main.npdm", "sdk", "rtld"})
        Check(ReadBytes(resolved->OpenFile(name)) == std::vector<u8>{0x42}, "NPDM/NSOs selected from different layers");
    Check(ReadBytes(patch.OpenRomFsWithPatch(base)) == RomBytes(0x1000, 0x63), "Replacement update data differs");
}
void TestBktrNca(bool encrypted) {
    auto keys{std::make_shared<crypto::KeyStore>("")};
    auto baseFixture{BaseFixture()}; baseFixture.Finalize(encrypted, keys);
    auto patchFixture{PatchFixture()}; patchFixture.Finalize(encrypted, keys);
    NCA base(baseFixture.backing, keys), patch(patchFixture.backing, keys);
    Check(!patch.romFs && patch.HasBktrSection(), "Unresolved patch exposed physical storage as RomFS");
    auto result{patch.OpenRomFsWithPatch(base)};
    auto expected{RomBytes()}; std::fill(expected.begin() + 0x800, expected.end(), 0x62);
    Check(ReadBytes(result) == expected, "Layered RomFS differs byte-for-byte");
    auto raw{patch.OpenRawStorageWithPatch(base, 1)};
    std::array<u8, 32> hashArea{};
    Check(raw->Read(hashArea) == hashArea.size(), "Base hash area inaccessible through indirect storage");
    std::array<u8, 0x803> unaligned{};
    result->Read(unaligned, 0x701);
    Check(std::equal(unaligned.begin(), unaligned.end(), expected.begin() + 0x701), "Unaligned read crossing CTR-Ex/relocation boundary differs");
    Check(ReadBytes(patch.OpenExeFsWithPatch(base)->OpenFile("main.npdm")) == std::vector<u8>{0x42}, "Patch ExeFS not selected");
}
void TestExeFsFallback() {
    auto keys{std::make_shared<crypto::KeyStore>("")};
    auto bf{BaseFixture()}; bf.Finalize();
    auto pf{PatchFixture(false)};
    auto logo{ExeBytes(0x43, false)}; pf.Add(2, ExeHeader(logo.size()), WithPrefix(logo)); pf.Finalize();
    NCA base(bf.backing, keys), patch(pf.backing, keys);
    Check(patch.OpenExeFsWithPatch(base) == base.exeFs, "Missing patch executable did not fall back to the complete base ExeFS");
    Check(patch.OpenRomFsWithPatch(base) != nullptr, "RomFS-only patch was discarded");
}
void TestSparseNca() {
    auto keys{std::make_shared<crypto::KeyStore>("")};
    NcaFixture f;
    auto section{RomHeader(0x200, 0x1e00)};
    section.raw.sparseInfo = {{0x2000, 0x8000, {util::MakeMagic<u32>("BKTR"), 1, 2, 0}}, 0xc00, 1, {}};
    std::vector<u8> body(0xa000);
    auto expected{RomBytes(0x1e00)};
    std::copy(expected.begin(), expected.end(), body.begin() + 0x200);
    Put(body, 0x2000, RelocationBlock{0, 1, 0x2000, {0}});
    RelocationBucketRaw bucket{}; bucket.numberEntries = 2; bucket.endOffset = 0x2000;
    bucket.relocationEntries[0] = {0, 0, 0}; bucket.relocationEntries[1] = {0x1800, 0, 1};
    Put(body, 0x6000, bucket);
    f.Add(1, section, body);
    f.header.sectionTables[1].mediaOffset = 0x10000 / 0x200;
    f.header.sectionTables[1].mediaEndOffset = 0x12000 / 0x200;
    f.Finalize();
    NCA nca(f.backing, keys);
    std::fill(expected.begin() + 0x1600, expected.end(), 0);
    Check(ReadBytes(nca.romFs) == expected, "Sparse remapping/zero range regressed");
}
NcaFixture CompressedFixture() {
    NcaFixture f;
    auto section{RomHeader(0x200, 0x9000)};
    section.raw.compressionInfo.bucket = {0x1000, 0x8000, {util::MakeMagic<u32>("BKTR"), 1, 1, 0}};
    auto rom{RomBytes()};
    std::vector<u8> body(0x9200);
    const int compressedSize{LZ4_compress_default(reinterpret_cast<const char *>(rom.data()), reinterpret_cast<char *>(body.data() + 0x200), rom.size(), 0x1000)};
    Check(compressedSize > 0, "Fixture compression failed");
    Put(body, 0x1200, RelocationBlock{0, 1, 0x1000, {0}});
    CompressedBucketRaw bucket{}; bucket.numberEntries = 1; bucket.endOffset = 0x1000;
    bucket.entries[0] = {0, 0, NCACompressionType::Lz4, 0, {}, static_cast<u32>(compressedSize)};
    Put(body, 0x5200, bucket);
    f.Add(1, section, body);
    return f;
}
void TestCompressedNca() {
    auto keys{std::make_shared<crypto::KeyStore>("")};
    auto f{CompressedFixture()}; f.Finalize();
    NCA nca(f.backing, keys);
    Check(ReadBytes(nca.romFs) == RomBytes(), "Compressed RomFS regressed");
    Check(nca.rawRomFs->size == 0x9000 && nca.romFs->size == 0x1000, "Compression applied at wrong level");
    // An indirect patch reuses the compressed base section, then decompresses once at the end.
    NcaFixture p;
    auto section{f.sections[1]};
    section.bktr.relocation = {0x200, 0x8000, util::MakeMagic<u32>("BKTR"), 1, 1, 0};
    std::vector<u8> body(0x8200);
    Put(body, 0x200, RelocationBlock{0, 1, 0x9200, {0}});
    RelocationBucketRaw bucket{}; bucket.numberEntries = 1; bucket.endOffset = 0x9200;
    bucket.relocationEntries[0] = {0, 0, 0}; Put(body, 0x4200, bucket);
    p.Add(1, section, body); p.Finalize();
    NCA patch(p.backing, keys);
    Check(ReadBytes(patch.OpenRomFsWithPatch(nca)) == RomBytes(), "BKTR over compressed base was decompressed too early");
}
void TestInvalidMetadata() {
    auto keys{std::make_shared<crypto::KeyStore>("")};
    auto expectFailure = [&](NcaFixture f) {
        f.Finalize(); bool failed{};
        try { NCA nca(f.backing, keys); } catch (const std::exception &) { failed = true; }
        Check(failed, "Malformed NCA metadata was accepted");
    };
    auto f{BaseFixture()}; f.sections[1].romfs.ivfc.levelCount = 0x07000000; expectFailure(f);
    f = BaseFixture(); f.sections[1].romfs.ivfc.levels[0].size = std::numeric_limits<u64>::max(); expectFailure(f);
    f = BaseFixture();
    Put(f.backing->data, f.header.sectionTables[0].mediaOffset * 0x200ULL + 0x200 + 0x18, std::numeric_limits<u64>::max());
    expectFailure(f);
    f = BaseFixture();
    const size_t start{f.header.sectionTables[1].mediaOffset * 0x200ULL};
    Put(f.backing->data, start + 0x200 + 8, std::numeric_limits<u64>::max()); expectFailure(f);
    auto bf{BaseFixture()}; bf.Finalize(); NCA base(bf.backing, keys);
    auto pf{PatchFixture()};
    const size_t ps{pf.header.sectionTables[1].mediaOffset * 0x200ULL};
    Put(pf.backing->data, ps + 0x5000 + 0x10 + 8, std::numeric_limits<u64>::max()); pf.Finalize();
    NCA patch(pf.backing, keys); bool failed{};
    try { patch.OpenRomFsWithPatch(base); } catch (const std::exception &) { failed = true; }
    Check(failed, "Invalid base physical relocation was accepted");
}
void TestDataNca() {
    auto keys{std::make_shared<crypto::KeyStore>("")};
    auto f{BaseFixture(false)}; f.header.contentType = NCAContentType::PublicData; f.Finalize();
    NCA dlc(f.backing, keys);
    Check(!dlc.exeFs && ReadBytes(dlc.romFs) == RomBytes(), "Standalone DLC/PublicData RomFS requires an unrelated Program base");
}

CNMT MakeProgramMeta(u64 programId, u8 contentByte, ContentMetaType type, u32 version = 1) {
    std::vector<u8> bytes(sizeof(PackagedContentMetaHeader) + sizeof(OptionalHeader) + sizeof(PackagedContentInfo));
    PackagedContentMetaHeader header{};
    header.id = type == ContentMetaType::Patch ? programId + 1 : programId;
    header.contentMetaType = type;
    header.version = version;
    header.extendedHeaderSize = sizeof(OptionalHeader); header.contentCount = 1;
    Put(bytes, 0, header);
    Put(bytes, sizeof(header), OptionalHeader{programId, 0});
    PackagedContentInfo info{}; info.contentId.fill(contentByte); info.contentType = ContentType::Program;
    Put(bytes, sizeof(header) + sizeof(OptionalHeader), info);
    auto pfsBytes{PfsBytes({{"fixture.cnmt", bytes}})};
    auto pfs{std::make_shared<MemoryBacking>(pfsBytes.size())}; pfs->data = pfsBytes;
    return CNMT(std::make_shared<PartitionFileSystem>(pfs));
}

void TestProgramSelection() {
    auto keys{std::make_shared<crypto::KeyStore>("")};
    NcaFixture bf; auto exe{ExeBytes(0x41)}; bf.Add(0, ExeHeader(exe.size()), WithPrefix(exe)); bf.Finalize();
    auto pf{PatchFixture(false)}; pf.Finalize();
    NCA base(bf.backing, keys), patch(pf.backing, keys);
    std::vector<loader::ProgramNcaCandidate> candidates{{std::string(32, '1') + ".nca", base}, {std::string(32, '2') + ".nca", patch}};
    auto app{MakeProgramMeta(base.header.titleId, 0x11, ContentMetaType::Application)};
    auto update{MakeProgramMeta(base.header.titleId, 0x22, ContentMetaType::Patch, 2)};
    auto selection{loader::SelectProgramNcas(candidates, {update, app})};
    Check(selection.base && selection.base->exeFs && !selection.base->romFs, "ExeFS-only base was discarded");
    Check(selection.patch && !selection.patch->exeFs && selection.patch->HasBktrSection(), "Unresolved Program patch was discarded");
    std::reverse(candidates.begin(), candidates.end());
    selection = loader::SelectProgramNcas(candidates, {app, update});
    Check(selection.base && selection.patch && selection.metadata->header.contentMetaType == ContentMetaType::Application, "Container order affects Program selection");
    selection = loader::SelectProgramNcas({candidates[0]}, {update});
    Check(!selection.base && selection.patch, "External update-only container lost its candidate");
    bool rejected{};
    try { loader::SelectProgramNcas(candidates, {app, MakeProgramMeta(base.header.titleId + 5, 0x22, ContentMetaType::Patch)}); }
    catch (const std::exception &) { rejected = true; }
    Check(rejected, "Mismatched Program update was accepted");
}

class FixtureLoader : public loader::Loader {
public:
    void *LoadProcessData(const std::shared_ptr<kernel::type::KProcess> &, const DeviceState &) override { return nullptr; }
};
void TestPersistentResolution() {
    auto keys{std::make_shared<crypto::KeyStore>("")};
    auto bf{BaseFixture()}; bf.Finalize();
    auto pf{PatchFixture()}; pf.Finalize();
    FixtureLoader base;
    base.programNca.emplace(bf.backing, keys); base.romFs = base.programNca->romFs;
    DeviceState state;
    state.updateLoader = std::make_shared<FixtureLoader>();
    state.updateLoader->programPatchNca.emplace(pf.backing, keys);
    const size_t before{modResolutionCount};
    base.ResolveProgramContent(state);
    auto mounted{base.currentProcessRomFs};
    Check(mounted && mounted == base.patchDataRomFs && base.programUpdateApplied, "Current/patch storage views differ");
    Check(base.currentProcessRomFsIdentity.find("size=0x1000, first16=5000000000000000") != std::string::npos, "Fingerprint does not describe served storage");
    auto expected{RomBytes()}; std::fill(expected.begin()+0x800, expected.end(), 0x62);
    state.updateLoader.reset(); base.programNca.reset(); bf.backing.reset(); pf.backing.reset();
    base.ResolveProgramContent(state);
    Check(base.currentProcessRomFs == mounted && ReadBytes(mounted) == expected, "Resolved backing lost its layers or was rebuilt");
    Check(modResolutionCount == before + 2, "Content/mods were resolved more than once");
    for (const auto *name : {"main", "main.npdm", "sdk", "rtld"})
        Check(ReadBytes(base.processExeFs->OpenFile(name)) == std::vector<u8>{0x42}, "Persistent ExeFS lifetime/coherence failed");
}
void TestPatchDataAbsence() {
    auto keys{std::make_shared<crypto::KeyStore>("")};
    auto bf{BaseFixture()}; bf.Finalize();
    FixtureLoader base; base.programNca.emplace(bf.backing, keys);
    DeviceState state; base.ResolveProgramContent(state);
    Check(base.currentProcessRomFs && !base.patchDataRomFs, "Base-only process fabricated patch data");
    NcaFixture pf; auto exe{ExeBytes(0x42)}; pf.Add(0, ExeHeader(exe.size()), WithPrefix(exe)); pf.Finalize();
    FixtureLoader other; other.programNca.emplace(bf.backing, keys);
    state.updateLoader = std::make_shared<FixtureLoader>();
    state.updateLoader->programPatchNca.emplace(pf.backing, keys);
    other.ResolveProgramContent(state);
    Check(ReadBytes(other.currentProcessRomFs) == RomBytes() && !other.patchDataRomFs, "ExeFS-only update fabricated a patch RomFS");
}

int main() {
    int failures{};
    auto run = [&](const char *name, auto test) {
        try { test(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception &e) { ++failures; std::cout << "FAIL " << name << ": " << e.what() << '\n'; }
    };
    run("BKTR relocation boundary", TestBktrPartition);
    run("BKTR physical offset >4 GiB", TestBktrLargeOffset);
    run("base, variable IVFC levels, section holes", TestBaseAndSectionHoles);
    run("ordinary replacement update, coherent ExeFS", TestReplacementUpdate);
    run("decrypted NCA indirect layering", [] { TestBktrNca(false); });
    run("encrypted NCA AES-CTR-Ex and indirect metadata", [] { TestBktrNca(true); });
    run("RomFS-only patch, ExeFS fallback", TestExeFsFallback);
    run("sparse NCA physical mapping and zero range", TestSparseNca);
    run("compressed NCA and BKTR before compression", TestCompressedNca);
    run("reject malformed IVFC/RomFS/relocations", TestInvalidMetadata);
    run("DLC/PublicData storage independence", TestDataNca);
    run("CNMT selection, container order, incomplete candidates", TestProgramSelection);
    run("persistent resolution, lifetime, fingerprints", TestPersistentResolution);
    run("base-only/ExeFS-only update patch-data absence", TestPatchDataAbsence);
    return failures ? 1 : 0;
}
