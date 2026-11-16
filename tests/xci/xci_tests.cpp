// SPDX-License-Identifier: MPL-2.0
// Containers and NCAs are synthetic; no console keys or commercial game data.
#include <iostream>
#include <crypto/key_store.h>
#include <loader/xci.h>
#include <loader/nsp.h>
#include <loader/nca.h>
#include <vfs/patch_manager.h>

using namespace skyline;
using namespace skyline::vfs;

void Check(bool condition, const char *message) { if (!condition) throw std::runtime_error(message); }
class MemoryBacking : public Backing {
public:
    std::vector<u8> data;
    bool shortRead{};
    explicit MemoryBacking(size_t size) : Backing({true, false, false}, size), data(size) {}
    size_t ReadImpl(span<u8> output, size_t offset) override {
        Check(offset <= size && output.size() <= size - offset, "Fixture read outside backing");
        const size_t count{shortRead && !output.empty() ? output.size() - 1 : output.size()};
        std::memcpy(output.data(), data.data() + offset, count);
        return count;
    }
};
#include "../vfs/fixtures.h"

namespace skyline::crypto {
KeyStore::KeyStore(const std::string &) {}
void KeyStore::PopulateTitleKey(Key128 name, Key128 value) { titleKeys.emplace(name, value); }
}
namespace skyline::vfs {
PatchManager::PatchManager() = default;
std::shared_ptr<FileSystem> PatchManager::PatchExeFS(const DeviceState &, std::shared_ptr<FileSystem> fs, u64) { return fs; }
std::shared_ptr<Backing> PatchManager::PatchRomFS(const DeviceState &, std::shared_ptr<Backing> backing, u64) { return backing; }
}
namespace skyline::loader {
void *NcaLoader::LoadExeFs(Loader *, const std::shared_ptr<FileSystem> &, const std::shared_ptr<kernel::type::KProcess> &, const DeviceState &) {
    throw exception("Guest executable loading is outside container tests");
}
}

std::shared_ptr<MemoryBacking> Memory(std::vector<u8> bytes) {
    auto backing{std::make_shared<MemoryBacking>(bytes.size())};
    backing->data = std::move(bytes);
    return backing;
}

std::vector<u8> HfsBytes(const std::vector<std::pair<std::string, std::vector<u8>>> &files) {
    size_t names{}, dataSize{};
    for (const auto &[name, data] : files) { names += name.size() + 1; dataSize += data.size(); }
    const size_t strings{0x10 + files.size() * 0x40}, dataStart{strings + names};
    std::vector<u8> result(dataStart + dataSize);
    Put(result, 0, util::MakeMagic<u32>("HFS0"));
    Put(result, 4, static_cast<u32>(files.size()));
    Put(result, 8, static_cast<u32>(names));
    size_t nameOffset{}, dataOffset{};
    for (size_t i{}; i < files.size(); ++i) {
        const auto &[name, data]{files[i]};
        Put(result, 0x10 + i * 0x40, static_cast<u64>(dataOffset));
        Put(result, 0x18 + i * 0x40, static_cast<u64>(data.size()));
        Put(result, 0x20 + i * 0x40, static_cast<u32>(nameOffset));
        std::memcpy(result.data() + strings + nameOffset, name.c_str(), name.size() + 1);
        std::memcpy(result.data() + dataStart + dataOffset, data.data(), data.size());
        nameOffset += name.size() + 1;
        dataOffset += data.size();
    }
    return result;
}

std::shared_ptr<MemoryBacking> XciBytes(const std::vector<std::pair<std::string, std::vector<u8>>> &files) {
    auto root{HfsBytes({{"update", HfsBytes({})}, {"normal", HfsBytes({})}, {"secure", HfsBytes(files)}})};
    std::vector<u8> result(0x200 + root.size());
    Put(result, 0x100, util::MakeMagic<u32>("HEAD"));
    Put(result, 0x130, u64{0x200});
    // This field is the root HFS0 header hash span, in bytes, not root data size.
    Put(result, 0x138, u64{0x10 + 3 * 0x40 + sizeof("update") + sizeof("normal") + sizeof("secure")});
    std::copy(root.begin(), root.end(), result.begin() + 0x200);
    return Memory(std::move(result));
}

template<class F> void Reject(F action, const char *message) {
    bool rejected{};
    try { action(); } catch (const std::exception &) { rejected = true; }
    Check(rejected, message);
}

void TestSelectedProgram() {
    auto fixture{BaseFixture()}; fixture.Finalize();
    auto image{XciBytes({{"program.nca", fixture.backing->data}})};
    auto keys{std::make_shared<crypto::KeyStore>("")};
    loader::XciLoader game(image, keys);
    auto &common{static_cast<loader::Loader &>(game)};
    Check(common.programNca.has_value(), "Selected XCI Program was hidden from common Loader resolver");
    common.ResolveProgramContent({});
    Check(common.programContentResolved && common.processExeFs && common.processExeFs->FileExists("main.npdm"),
          "XCI did not resolve executable content through common resolver");
    Check(ReadBytes(common.currentProcessRomFs) == RomBytes(), "XCI resolved incorrect Program RomFS");
}

std::vector<u8> ProgramMetadata(u64 programId) {
    std::vector<u8> content(sizeof(PackagedContentMetaHeader) + sizeof(OptionalHeader) + 2 * sizeof(PackagedContentInfo));
    PackagedContentMetaHeader header{}; header.id = programId; header.version = 1;
    header.contentMetaType = ContentMetaType::Application; header.extendedHeaderSize = sizeof(OptionalHeader); header.contentCount = 2;
    Put(content, 0, header); Put(content, sizeof(header), OptionalHeader{programId, 0});
    for (u8 i{}; i < 2; ++i) {
        PackagedContentInfo record{}; record.contentId.fill(i + 1); record.contentType = ContentType::Program; record.idOffset = i;
        Put(content, sizeof(header) + sizeof(OptionalHeader) + i * sizeof(record), record);
    }
    NcaFixture metadata;
    metadata.header.contentType = NCAContentType::Meta;
    auto partition{PfsBytes({{"application.cnmt", content}})};
    metadata.Add(0, ExeHeader(partition.size()), WithPrefix(partition)); metadata.Finalize();
    return metadata.backing->data;
}

void TestUnselectedProgramDeferred() {
    auto base{BaseFixture()}; base.Finalize();
    auto other{BaseFixture()}; other.header.titleId += 1;
    // Valid section descriptors but malformed inner PFS0; only opening that
    // Program's content should fail. Header/CNMT selection remains possible.
    Put(other.backing->data, other.header.sectionTables[0].mediaOffset * 0x200ULL + 0x200, u32{});
    other.Finalize();
    const std::vector<std::pair<std::string, std::vector<u8>>> files{{"01010101010101010101010101010101.nca", base.backing->data},
                         {"02020202020202020202020202020202.nca", other.backing->data},
                         {"metadata.nca", ProgramMetadata(base.header.titleId)}};
    auto image{XciBytes(files)};
    auto keys{std::make_shared<crypto::KeyStore>("")};
    loader::XciLoader selected(image, keys, 0);
    selected.ResolveProgramContent({});
    Check(selected.processExeFs && ReadBytes(selected.currentProcessRomFs) == RomBytes(),
          "Unselected Program prevented selected XCI content resolution");
    loader::XciLoader invalid(image, keys, 1);
    Reject([&] { invalid.ResolveProgramContent({}); }, "Selected malformed XCI Program passed content validation");
    loader::NspLoader nsp(Memory(PfsBytes(files)), keys, "", loader::NspLoadMode::Full, 0);
    nsp.ResolveProgramContent({});
    Check(ReadBytes(nsp.currentProcessRomFs) == ReadBytes(selected.currentProcessRomFs),
          "NSP and XCI differed for the same selected Program with malformed unselected content");
    loader::NspLoader invalidNsp(Memory(PfsBytes(files)), keys, "", loader::NspLoadMode::Full, 1);
    Reject([&] { invalidNsp.ResolveProgramContent({}); }, "Selected malformed NSP Program passed content validation");
}

void TestContainerEquivalence() {
    auto keys{std::make_shared<crypto::KeyStore>("")};
    auto first{BaseFixture()}; first.Finalize();
    auto second{BaseFixture()}; second.header.titleId += 1;
    auto exe{ExeBytes(0x42)};
    std::copy(exe.begin(), exe.end(), second.backing->data.begin() + second.header.sectionTables[0].mediaOffset * 0x200ULL + 0x200);
    auto rom{RomBytes(0x1000, 0x53)};
    std::copy(rom.begin(), rom.end(), second.backing->data.begin() + second.header.sectionTables[1].mediaOffset * 0x200ULL + 0x200);
    second.Finalize();
    const std::vector<std::pair<std::string, std::vector<u8>>> files{
        {"01010101010101010101010101010101.nca", first.backing->data},
        {"02020202020202020202020202020202.nca", second.backing->data},
        {"metadata.nca", ProgramMetadata(first.header.titleId)}};
    for (u8 index{}; index < 2; ++index) {
        loader::XciLoader xci(XciBytes(files), keys, index);
        loader::NspLoader nsp(Memory(PfsBytes(files)), keys, "", loader::NspLoadMode::Full, index);
        xci.ResolveProgramContent({}); nsp.ResolveProgramContent({});
        Check(xci.programNca && nsp.programNca && xci.programNca->header.titleId == first.header.titleId + index &&
              xci.programNca->header.titleId == nsp.programNca->header.titleId,
              "XCI and NSP selected different Program IDs");
        const auto xciMain{ReadBytes(xci.processExeFs->OpenFile("main"))};
        Check(xciMain == ReadBytes(nsp.processExeFs->OpenFile("main")) && xciMain == std::vector<u8>{static_cast<u8>(0x41 + index)},
              "XCI and NSP selected different ExeFS content");
        Check(ReadBytes(xci.currentProcessRomFs) == ReadBytes(nsp.currentProcessRomFs) &&
              ReadBytes(xci.currentProcessRomFs) == RomBytes(0x1000, index == 0 ? 0x51 : 0x53),
              "XCI and NSP selected different RomFS content");
    }
}

void TestPartitionBounds(bool hashed) {
    const auto make = [&](auto files) { return hashed ? HfsBytes(files) : PfsBytes(files); };
    const std::vector<std::pair<std::string, std::vector<u8>>> files{{"first", {1, 2, 3}}, {"last", {4, 5}}};
    auto valid{make(files)};
    PartitionFileSystem fs(Memory(valid));
    Check(ReadBytes(fs.OpenFile("last")) == std::vector<u8>({4, 5}), "Partition lost its final file entry");
    auto invalid{valid}; Put(invalid, 0x10, std::numeric_limits<u64>::max());
    Reject([&] { PartitionFileSystem bad(Memory(invalid)); }, "Partition accepted overflowing data offset");
    invalid = valid; Put(invalid, 0x18, std::numeric_limits<u64>::max());
    Reject([&] { PartitionFileSystem bad(Memory(invalid)); }, "Partition accepted file outside data span");
    invalid = valid; Put(invalid, 0x20, std::numeric_limits<u32>::max());
    Reject([&] { PartitionFileSystem bad(Memory(invalid)); }, "Partition accepted name offset outside string table");
    invalid = valid;
    const size_t stringStart{0x10 + files.size() * (hashed ? 0x40 : 0x18)};
    std::fill(invalid.begin() + stringStart, invalid.begin() + stringStart + sizeof("first") + sizeof("last"), 'x');
    Reject([&] { PartitionFileSystem bad(Memory(invalid)); }, "Partition accepted unterminated string table name");
    invalid = valid; Put(invalid, 4, std::numeric_limits<u32>::max());
    Reject([&] { PartitionFileSystem bad(Memory(invalid)); }, "Partition accepted impossible entry table");
    auto shortBacking{Memory(valid)}; shortBacking->shortRead = true;
    Reject([&] { PartitionFileSystem bad(shortBacking); }, "Partition accepted short header/table reads");
    if (hashed) {
        invalid = valid; Put(invalid, 0x24, u32{4});
        Reject([&] { PartitionFileSystem bad(Memory(invalid)); }, "HFS0 accepted hashed span outside file");
    }
}

void TestXciBounds() {
    auto keys{std::make_shared<crypto::KeyStore>("")};
    auto image{XciBytes({})}; Put(image->data, 0x138, std::numeric_limits<u64>::max());
    Reject([&] { loader::XciLoader bad(image, keys); }, "XCI accepted header hash span outside root partition");
    image = XciBytes({}); image->shortRead = true;
    Reject([&] { loader::XciLoader bad(image, keys); }, "XCI accepted a short card header read");
    image = XciBytes({}); Put(image->data, 0x130, std::numeric_limits<u64>::max());
    Reject([&] { loader::XciLoader bad(image, keys); }, "XCI accepted root partition offset beyond image");
}

void TestKeyGeneration(bool title, u8 generation) {
    const std::array<NCAKeyAreaEncryptionKeyType, 3> types{NCAKeyAreaEncryptionKeyType::Application,
        NCAKeyAreaEncryptionKeyType::Ocean, NCAKeyAreaEncryptionKeyType::System};
    for (const auto type : types) {
        auto keys{std::make_shared<crypto::KeyStore>("")};
        auto fixture{BaseFixture()};
        fixture.header.cryptoType2 = generation;
        fixture.header.keyIndex = type;
        if (title) {
            fixture.header.rightsId.fill(0x52);
            keys->PopulateTitleKey(fixture.header.rightsId, crypto::KeyStore::Key128{});
        }
        fixture.Finalize(true, keys);
        bool rejected{};
        try { NCA nca(fixture.backing, keys, false); }
        catch (const loader::loader_exception &error) {
            rejected = error.error == (title ? loader::LoaderResult::MissingTitleKek : loader::LoaderResult::MissingKeyArea);
        }
        Check(rejected, "Out-of-range key generation did not retain missing-key LoaderResult");
    }
}

int main(int argc, char **argv) {
    int failures{};
    auto run = [&](const char *name, auto test) {
        if (argc > 1 && std::string_view(name).find(argv[1]) == std::string_view::npos) return;
        try { test(); std::cout << "PASS " << name << std::endl; }
        catch (const std::exception &error) { ++failures; std::cout << "FAIL " << name << ": " << error.what() << std::endl; }
    };
    run("XCI selected Program uses common resolver", TestSelectedProgram);
    run("XCI unselected Program defers content opening", TestUnselectedProgramDeferred);
    run("XCI/NSP equivalent ProgramIndex 0 and 1", TestContainerEquivalence);
    run("PFS0 structural bounds", [] { TestPartitionBounds(false); });
    run("HFS0 structural bounds", [] { TestPartitionBounds(true); });
    run("XCI header/root bounds", TestXciBounds);
    run("key area generation 0x21", [] { TestKeyGeneration(false, 0x21); });
    run("key area generation 0xff", [] { TestKeyGeneration(false, 0xff); });
    run("title kek generation 0x21", [] { TestKeyGeneration(true, 0x21); });
    run("title kek generation 0xff", [] { TestKeyGeneration(true, 0xff); });
    return failures ? 1 : 0;
}
