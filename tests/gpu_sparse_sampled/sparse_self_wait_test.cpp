// SPDX-License-Identifier: MPL-2.0
// Regression for the Oceanhorn 2 startup freeze: a sparse texture upload read trapped guest pages on the GPFIFO thread during
// CommandExecutor::SubmitInternal, faulting into TrapManager::HandleTrap whose lock callback waited on the submission being recorded.
// Here a SIGSEGV on a PROT_NONE page stands for that trap: any fault is a would-be self-wait.
#include <gpu/texture/sparse_resident_copy.h>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/mman.h>
#include <unistd.h>

using namespace skyline::gpu::texture;

namespace {
    constexpr size_t Page{4096};
    constexpr size_t Pages{8};
    volatile sig_atomic_t faults{};
    uint8_t *guestBase;

    void OnFault(int, siginfo_t *info, void *) {
        faults = faults + 1;
        // Model "trap callbacks ran, access allowed" so the control case terminates instead of deadlocking
        auto page{reinterpret_cast<uintptr_t>(info->si_addr) & ~(Page - 1)};
        mprotect(reinterpret_cast<void *>(page), Page, PROT_READ | PROT_WRITE);
    }

    void Check(bool condition, const std::string &message) {
        if (!condition) {
            std::fprintf(stderr, "FAIL: %s\n", message.c_str());
            std::exit(1);
        }
    }

    // Same mechanism as MemoryManager::CreateMirror: an unprotected alias of a shared guest mapping
    struct MremapMapper {
        int maps{}, unmaps{};
        uint8_t *Map(uint8_t *alignedBegin, size_t alignedSize) {
            maps++;
            void *mirror{mremap(alignedBegin, 0, alignedSize, MREMAP_MAYMOVE)};
            Check(mirror != MAP_FAILED, "mirror creation");
            mprotect(mirror, alignedSize, PROT_READ | PROT_WRITE);
            return static_cast<uint8_t *>(mirror);
        }
        void Unmap(uint8_t *mirror, size_t alignedSize) {
            unmaps++;
            munmap(mirror, alignedSize);
        }
    };

    uint8_t Pattern(size_t index) { return static_cast<uint8_t>((index * 131 + 7) & 0xFF) | 1; } // never zero, so holes are distinguishable
}

int main() {
    struct sigaction action{};
    action.sa_sigaction = OnFault;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    sigaction(SIGSEGV, &action, nullptr);

    int fd{static_cast<int>(memfd_create("guest", 0))};
    Check(fd >= 0 && ftruncate(fd, Page * Pages) == 0, "memfd");
    guestBase = static_cast<uint8_t *>(mmap(nullptr, Page * Pages, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    Check(guestBase != MAP_FAILED, "guest mapping");
    for (size_t i{}; i < Page * Pages; i++)
        guestBase[i] = Pattern(i);

    // Backing of 5 pages: [0x0100,0x0900) resident (unaligned, one page boundary), hole, [0x2F00,0x3100) resident straddling two pages, hole.
    constexpr size_t BackingSize{Page * 5};
    const SparseSegment segments[]{
        {0x0100, guestBase + 0x1100, 0x0800},
        {0x2F00, guestBase + 0x5F00, 0x0200},
    };
    auto protect{[] { mprotect(guestBase, Page * Pages, PROT_NONE); }};

    // Control: the hazard exists, a direct read of the guest span faults (in production: deadlock)
    protect();
    faults = 0;
    volatile uint8_t sink{*static_cast<volatile uint8_t *>(segments[0].cpu)};
    (void)sink;
    Check(faults > 0, "control: reading a trapped guest page directly must fault, otherwise this test proves nothing");

    // Production helper: must complete with zero faults while every guest page is protected
    protect();
    faults = 0;
    std::vector<uint8_t> out(BackingSize, 0xCC);
    MremapMapper mapper;
    CopySparseSegments(out.data(), out.size(), segments, std::size(segments), mapper, Page);
    Check(faults == 0, "CopySparseSegments touched a protected guest page (would self-wait in SubmitInternal)");
    Check(mapper.maps == 2 && mapper.maps == mapper.unmaps, "every mirror must be released");

    for (size_t i{}; i < BackingSize; i++) {
        bool resident{false};
        for (const auto &segment : segments)
            if (i >= segment.offset && i < segment.offset + segment.size) {
                Check(out[i] == Pattern(static_cast<size_t>(segment.cpu - guestBase) + (i - segment.offset)), "resident byte " + std::to_string(i));
                resident = true;
            }
        if (!resident)
            Check(out[i] == 0, "sparse hole must read as zero at " + std::to_string(i));
    }

    // Out-of-range segments are rejected without leaking a mirror
    MremapMapper leakCheck;
    const SparseSegment bad[]{{BackingSize - 4, guestBase + Page, 64}};
    bool threw{};
    try { CopySparseSegments(out.data(), out.size(), bad, 1, leakCheck, Page); } catch (const std::out_of_range &) { threw = true; }
    Check(threw && leakCheck.maps == leakCheck.unmaps, "out-of-range segment must throw before mapping");

    // Adjacent runs that are contiguous both in the backing and on the host merge; a host gap does not
    std::vector<SparseSegment> merged;
    AppendSparseSegment(merged, 0, guestBase, 16);
    AppendSparseSegment(merged, 16, guestBase + 16, 16);
    AppendSparseSegment(merged, 32, guestBase + 4096, 16);
    Check(merged.size() == 2 && merged[0].size == 32, "segment merging");

    std::puts("PASS: sparse resident reads go through mirrors and never fault on trapped guest pages");
    return 0;
}
