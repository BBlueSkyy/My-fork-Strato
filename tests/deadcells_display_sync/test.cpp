// SPDX-License-Identifier: MPL-2.0
// Native, Vulkan-free checks for two premises used by the Dead Cells display-surface audit.
//
// (1) Texture::SynchronizeHostImpl() reads guest bytes through `Texture::mirror`
//     (kernel/memory.cpp MemoryManager::CreateMirrors: mremap(old_size = 0) of the guest
//     pages, then PROT_READ|PROT_WRITE). Guest-view protection traps (what Buffer::ReadTrap
//     / Texture::ReadTrap rely on) therefore are NOT observable through the mirror.
// (2) The first 64 bytes of the presented region (sample_7_guest.bin, from the capture
//     report) decode as guest pointers consistent with the allocation, not as pixel noise.
//
// These tests establish mechanism and data shape only. They do NOT prove who should have
// written the pixels; see docs/investigations/deadcells-display-surface-audit-2026-11-07.md.
#include <array>
#include <cassert>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

namespace {
    volatile std::sig_atomic_t trapHits{};
    void *protectedBase{};
    size_t protectedSize{};

    void Handler(int, siginfo_t *, void *) {
        trapHits = trapHits + 1;
        // Emulate the trap handler: lift protection so the faulting access can retry.
        mprotect(protectedBase, protectedSize, PROT_READ | PROT_WRITE);
    }

    void TestMirrorBypassesGuestViewProtection() {
        const size_t size{static_cast<size_t>(sysconf(_SC_PAGESIZE)) * 4};
        const int fd{memfd_create("guest", 0)};
        assert(fd >= 0);
        assert(ftruncate(fd, static_cast<off_t>(size)) == 0);
        auto *guest{static_cast<uint8_t *>(mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0))};
        assert(guest != MAP_FAILED);

        // Same construction as MemoryManager::CreateMirrors().
        void *reserve{mmap(nullptr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)};
        assert(reserve != MAP_FAILED);
        auto *mirror{static_cast<uint8_t *>(mremap(guest, 0, size, MREMAP_FIXED | MREMAP_MAYMOVE, reserve))};
        assert(mirror != MAP_FAILED);
        assert(mprotect(mirror, size, PROT_READ | PROT_WRITE) == 0);
        assert(mirror != guest);

        struct sigaction action{};
        action.sa_sigaction = Handler;
        action.sa_flags = SA_SIGINFO;
        sigemptyset(&action.sa_mask);
        assert(sigaction(SIGSEGV, &action, nullptr) == 0);

        protectedBase = guest;
        protectedSize = size;

        // Trap the guest view (mirrors Texture/Buffer TrapRegions(..., true)).
        assert(mprotect(guest, size, PROT_NONE) == 0);

        // A GPU-dirty object would be flushed from its trap callback on a guest-view access.
        // An access through the mirror must neither fault nor reach that callback.
        const uint8_t seen{mirror[0]};
        mirror[1] = 0x5A;
        (void)seen;
        assert(trapHits == 0);

        // Control: the same access through the guest view does fault.
        const uint8_t viaGuest{guest[1]};
        assert(trapHits == 1);
        assert(viaGuest == 0x5A); // Both views alias the same physical pages.

        std::puts("PASS: mirror reads/writes bypass guest-view traps; guest view faults (control)");
    }

    // 64-byte header of the region presented at 0x1817120000 (report, sample_7_guest.bin).
    constexpr uint64_t Base{0x1817120000ULL};
    constexpr std::array<uint64_t, 8> Header{
        0x200400000ULL, 0x10000004000ULL, 0x100000001ULL, 0ULL,
        0x1817120038ULL, 0x180a6195ccULL, 0x1811120000ULL, 0xc200ULL,
    };
    constexpr uint64_t HeapBase{0x1809000000ULL};
    constexpr uint64_t HeapSize{4007657472ULL};

    constexpr bool InHeap(uint64_t value) { return value >= HeapBase && value < HeapBase + HeapSize; }

    void TestHeaderIsStructuredNotPixels() {
        // +0x20 points at +0x38 of this very allocation, and +0x38 holds a small integer.
        assert(Header[4] == Base + 0x38);
        assert(Header[7] == 0xc200);
        // +0x28 points into the guest heap mapping; +0x30 is the allocation base minus 0x6000000 (96 MiB).
        assert(InHeap(Header[5]));
        assert(Header[6] == Base - 0x6000000);

        // Interpreted as RGBA8 (what the presentation path does), the 16 pixels are not a
        // plausible opaque image: the pointer words carry alpha bytes of 0x00.
        uint8_t bytes[64];
        std::memcpy(bytes, Header.data(), sizeof bytes);
        int opaque{};
        for (int pixel{}; pixel < 16; ++pixel)
            opaque += bytes[pixel * 4 + 3] == 0xFF;
        assert(opaque == 0);

        std::puts("PASS: header decodes as in-allocation/in-heap guest pointers (structured memory, not pixels)");
    }
}

int main() {
    TestMirrorBypassesGuestViewProtection();
    TestHeaderIsStructuredNotPixels();
    return 0;
}
