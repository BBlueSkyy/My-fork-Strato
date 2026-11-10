// SPDX-License-Identifier: MPL-2.0
#include <iostream>
#include <functional>
#include <vfs/npdm.h>
using namespace skyline;
namespace {
class MemoryBacking : public vfs::Backing {
    std::vector<u8> bytes;
    size_t shortAt;
    size_t ReadImpl(span<u8> out, size_t offset) override {
        const auto count = offset == shortAt && !out.empty() ? out.size() - 1 : out.size();
        std::memcpy(out.data(), bytes.data() + offset, count);
        return count;
    }
public:
    explicit MemoryBacking(std::vector<u8> data, size_t shortAt = SIZE_MAX) : Backing({true, false, false}, data.size()), bytes(std::move(data)), shortAt(shortAt) {}
};
struct Fixture {
    vfs::NPDM::NpdmMeta meta{};
    vfs::NPDM::NpdmAci0 aci{};
    std::vector<u32> caps{7u | (59u << 4) | (16u << 10) | (2u << 24),
                          0x3fffu | (3u << 15) | (19u << 19)};
    Fixture() {
        meta.magic = util::MakeMagic<u32>("META");
        meta.flags.raw = 0x77; // 64-bit, 39-bit address space, modern flag bits 4,5,6.
        meta.mainThreadPriority = 44;
        meta.idealCore = 1;
        meta.mainThreadStackSize = 0x200000;
        meta.aci0.offset = 0x80;
        meta.aci0.size = 0x60;
        aci.magic = util::MakeMagic<u32>("ACI0");
        aci.programId = 0x0100123456789000ull;
        aci.fsAccessControl.offset = aci.srvAccessControl.offset = 0x40;
        aci.kernelCapability.offset = 0x40;
        aci.kernelCapability.size = caps.size() * sizeof(u32);
    }
    std::shared_ptr<MemoryBacking> Backing(size_t shortAt = SIZE_MAX) const {
        std::vector<u8> bytes(0x180);
        std::memcpy(bytes.data(), &meta, sizeof(meta));
        std::memcpy(bytes.data() + 0x80, &aci, sizeof(aci));
        std::memcpy(bytes.data() + 0x80 + aci.kernelCapability.offset, caps.data(), caps.size() * sizeof(u32));
        return std::make_shared<MemoryBacking>(std::move(bytes), shortAt);
    }
};
int failed{};
void Check(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
void Test(const char *name, const std::function<void()> &test) {
    try { test(); std::cout << "PASS " << name << '\n'; }
    catch (const std::exception &e) { failed++; std::cerr << "FAIL " << name << ": " << e.what() << '\n'; }
}
void Reject(const Fixture &f, size_t shortAt = SIZE_MAX) {
    try { vfs::NPDM parsed(f.Backing(shortAt)); }
    catch (const skyline::exception &) { return; }
    throw std::runtime_error("malformed NPDM was accepted");
}
}
int main() {
    Test("valid META/ACI0 priority, affinity, version and flag decoding", [] {
        Fixture f;
        vfs::NPDM parsed(f.Backing());
        Check(parsed.aci0.programId == f.aci.programId, "program id changed");
        Check(parsed.threadInfo.priority.min == 16 && parsed.threadInfo.priority.max == 59, "priority bit ordering wrong");
        Check(parsed.threadInfo.coreMask.to_ulong() == 7, "core range wrong");
        Check(parsed.kernelVersion.majorVersion == 19 && parsed.kernelVersion.minorVersion == 3, "kernel version wrong");
        Check(parsed.meta.flags.is64Bit && parsed.meta.flags.type == memory::AddressSpaceType::AddressSpace39Bit && parsed.meta.flags.enableAliasRegionExtraSize, "META flags decoded incorrectly");
    });
    Test("ACI0 extent cannot escape file", [] { Fixture f; f.meta.aci0.size = 0x1000; Reject(f); });
    Test("KAC extent cannot escape ACI0 into trailing file bytes", [] { Fixture f; f.meta.aci0.size = 0x40; Reject(f); });
    Test("KAC cannot overlap ACI0 header", [] { Fixture f; f.aci.kernelCapability.offset = 0x3c; Reject(f); });
    Test("duplicate thread descriptors rejected", [] { Fixture f; f.caps[1] = f.caps[0]; Reject(f); });
    Test("reversed priority range rejected", [] { Fixture f; f.caps[0] = 7u | (16u << 4) | (59u << 10) | (2u << 24); Reject(f); });
    Test("reversed core range rejected", [] { Fixture f; f.caps[0] |= 3u << 16; Reject(f); });
    Test("core range beyond emulator cores rejected as NPDM error", [] { Fixture f; f.caps[0] |= 4u << 24; Reject(f); });
    Test("main ideal core beyond emulator cores rejected as NPDM error", [] { Fixture f; f.meta.idealCore = 255; Reject(f); });
    Test("main priority outside capability rejected", [] { Fixture f; f.meta.mainThreadPriority = 60; Reject(f); });
    Test("main ideal core outside capability rejected", [] { Fixture f; f.meta.idealCore = 3; Reject(f); });
    Test("short META read rejected", [] { Fixture f; Reject(f, 0); });
    Test("short ACI0 read rejected", [] { Fixture f; Reject(f, 0x80); });
    Test("short KAC read rejected", [] { Fixture f; Reject(f, 0xc0); });
    Test("filesystem control cannot escape ACI0", [] { Fixture f; f.aci.fsAccessControl.size = 0x100; Reject(f); });
    Test("service control cannot escape ACI0", [] { Fixture f; f.aci.srvAccessControl.offset = 0x100; Reject(f); });
    Test("missing thread descriptor rejected", [] { Fixture f; f.caps[0] = 0xffffffff; Reject(f); });
    return failed ? 1 : 0;
}
