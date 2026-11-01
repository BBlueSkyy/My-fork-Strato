#include "runtime.h"
#include <random>
static void Protection() {
    Page page;
    TrapManager trap;
    std::array<span<u8>, 1> mapping{span<u8>{page.data, 4096}};
    unsigned reads{}, writes{};
    auto handle = trap.CreateTrap(
        mapping, [] {},
        [&] {
            ++reads;
            return true;
        },
        [&] {
            ++writes;
            return true;
        });
    trap.TrapRegions(handle, false);
    Check(trap.HandleTrap(page.data, false), "read fault not handled");
    Check(reads == 1 && writes == 0, "read fault used write callback");
    Check(!(lastProtection & PROT_WRITE), "GPU readback removed the next CPU-write trap");
    trap.HandleTrap(page.data + 8, true);
    Check(writes == 1 && (lastProtection & PROT_WRITE), "CPU write did not invoke its callback");
    trap.DeleteTrap(handle);
    std::puts("PASS readback retains write trap; real write grants it afterward");
}
static void Routing() {
    for (bool host : {false, true})
        for (uint64_t fsc : {7ULL, 15ULL}) {
            Page page;
            TrapManager trap;
            trap.InstallStaticInstance();
            std::array<span<u8>, 1> mapping{span<u8>{page.data, 4096}};
            unsigned reads{}, writes{};
            auto h = trap.CreateTrap(
                mapping, [] {},
                [&] {
                    ++reads;
                    return true;
                },
                [&] {
                    ++writes;
                    return true;
                });
            trap.TrapRegions(h, false);
            ArmContext ctx;
            const uint32_t magic = 0x45535201, size = 16;
            const uint64_t esr = (0x24ULL << 26) | fsc;
            std::memcpy(ctx.uc_mcontext.__reserved, &magic, 4);
            std::memcpy(ctx.uc_mcontext.__reserved + 4, &size, 4);
            std::memcpy(ctx.uc_mcontext.__reserved + 8, &esr, 8);
            siginfo_t info{};
            info.si_code = SEGV_ACCERR;
            info.si_addr = page.data;
            const bool handled = host ? RouteHost(SIGSEGV, &info, &ctx) : RouteGuest(SIGSEGV, &info, &ctx);
            trap.DeleteTrap(h);
            trap.UninstallStaticInstance();
            Check(handled && reads == 1 && writes == 0, "NCE permission read routed as a CPU write");
        }
    std::puts("PASS both production NCE call expressions select readback for permission reads");
}
static void OverlapAndRetry() {
    Page page;
    TrapManager trap;
    std::array<span<u8>, 1> a{span<u8>{page.data, 128}}, b{span<u8>{page.data + 64, 256}};
    unsigned ar{}, br{}, aw{}, bw{}, prepares{};
    bool ready{};
    auto ah = trap.CreateTrap(
        a, [] {},
        [&] {
            ++ar;
            return true;
        },
        [&] {
            ++aw;
            return true;
        });
    auto bh = trap.CreateTrap(
        b,
        [&] {
            ++prepares;
            ready = true;
        },
        [&] {
            ++br;
            return ready;
        },
        [&] {
            ++bw;
            return true;
        });
    trap.TrapRegions(ah, false);
    trap.TrapRegions(bh, false);
    Check(trap.HandleTrap(page.data + 90, false), "overlap read not handled");
    Check(ar == 1 && br == 2 && prepares == 1, "retry repeated completed callback or missed alias");
    Check(!(lastProtection & PROT_WRITE), "alias retry lost write protection");
    trap.DeleteTrap(ah);
    Check(!(lastProtection & PROT_WRITE), "deleting alias unprotected surviving writer");
    Check(trap.HandleTrap(page.data + 100, true) && aw == 0 && bw == 1, "surviving alias store callback lost");
    trap.DeleteTrap(bh);
    Check(!trap.HandleTrap(page.data, false), "deleted traps still callable");
    std::puts("PASS partial overlaps, callback retry, deleted alias, surviving write trap");
}
static void Parser() {
    using fault_access::Access;
    std::array<std::byte, 4096> frame{};
    auto record = [&](size_t at, uint32_t magic, uint32_t size, uint64_t esr) {
        std::memcpy(frame.data() + at, &magic, 4);
        std::memcpy(frame.data() + at + 4, &size, 4);
        std::memcpy(frame.data() + at + 8, &esr, 8);
    };
    auto reset = [&](uint64_t esr) {
        frame.fill({});
        record(0, 0x45535201, 16, esr);
    };
    const uint64_t read = (0x24ULL << 26) | 0x0f;
    reset(read);
    Check(fault_access::Classify(frame) == Access::Read, "valid permission load not classified");
    for (uint64_t fsc = 4; fsc < 8; ++fsc) {
        reset((0x24ULL << 26) | fsc);
        Check(fault_access::Classify(frame) == Access::Read, "PROT_NONE translation load not classified");
        Check(!fault_access::MayWrite(SIGSEGV, SEGV_ACCERR, frame), "PROT_NONE ACCERR routed as write");
        Check(fault_access::MayWrite(SIGSEGV, SEGV_MAPERR, frame), "unmapped translation fault routed as read");
        reset((0x24ULL << 26) | fsc | 64);
        Check(fault_access::Classify(frame) == Access::Write, "translation store not classified");
    }
    reset(read | 64);
    Check(fault_access::Classify(frame) == Access::Write, "store/RMW not classified as write");
    for (auto esr : {read | (1ULL << 8), read | (1ULL << 7), read | (1ULL << 10), (0x25ULL << 26) | 0x0f,
                     (0x20ULL << 26) | 0x0f, (0x24ULL << 26) | 8}) {
        reset(esr);
        Check(fault_access::Classify(frame) == Access::Unknown, "ambiguous syndrome accepted as data load");
    }
    reset(read);
    Check(fault_access::MayWrite(SIGBUS, SEGV_ACCERR, frame), "non-SEGV classified as read");
    Check(fault_access::MayWrite(SIGSEGV, SI_USER, frame), "user signal classified as read");
    Check(fault_access::MayWrite(SIGSEGV, SEGV_MAPERR, frame), "mapping error classified as read");
    reset(read);
    record(16, 0x45535201, 16, read);
    Check(fault_access::Classify(frame) == Access::Unknown, "duplicate ESR accepted");
    reset(read);
    record(16, 123, 8, 0);
    Check(fault_access::Classify(frame) == Access::Unknown, "malformed suffix accepted");
    reset(read);
    Check(fault_access::Classify(std::span(frame).first(16)) == Access::Unknown, "unterminated chain accepted");
    for (uint32_t size : {0U, 8U, 15U, 17U, 32U, UINT32_MAX}) {
        reset(read);
        std::memcpy(frame.data() + 4, &size, 4);
        Check(fault_access::Classify(frame) == Access::Unknown, "bad ESR size accepted");
    }
    frame.fill({});
    record(0, 0x46508001, 528, 0);
    record(528, 0x45535201, 16, read);
    record(544, 0x45585401, 32, UINT64_MAX); // extra_context datap must never be followed.
    Check(fault_access::Classify(frame) == Access::Read, "ordinary FPSIMD/ESR/extra chain rejected");
    Check(fault_access::Classify({}) == Access::Unknown, "empty records accepted");
    std::mt19937 random(45);
    for (unsigned i{}; i < 20000; ++i) {
        reset(read);
        uint32_t size = (random() % 4096) * 16 + 4096;
        std::memcpy(frame.data() + 4, &size, 4);
        Check(fault_access::Classify(frame) == Access::Unknown, "out-of-bounds record accepted");
    }
    std::puts("PASS bounded ESR parser: permission read/write, unknown fallback, malformed/duplicate/unterminated "
              "contexts, extra pointer and 20000 bound checks");
}
int main(int argc, char **argv) {
    try {
        const auto mode = argc > 1 ? std::string_view(argv[1]) : std::string_view{};
        if (mode.empty() || mode == "protection")
            Protection();
        if (mode.empty() || mode == "routing")
            Routing();
        if (mode.empty() || mode == "parser")
            Parser();
        if (mode.empty() || mode == "overlap")
            OverlapAndRetry();
    } catch (const std::exception &e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}
