// SPDX-License-Identifier: MPL-2.0
// Regression tests for stale TrapManager page protections (Oceanhorn 2 "NEW GAME" SIGSEGV).
//
// The crash: a guest memcpy faulted on a page that was PROT_READ|PROT_EXEC although no trap entry covered it any more, so
// TrapManager::HandleTrap found nothing to handle and the fault became a fatal SIGSEGV. These tests drive the production
// TrapManager/IntervalMap (compiled from the repository sources) against real mprotect'ed pages and read the real page
// permissions back from /proc/self/maps.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <sys/mman.h>
#include <unistd.h>
#include "trap_manager.h"

using namespace skyline;

namespace {
    constexpr size_t Page{constant::PageSize};
    int failures{};

    void Check(bool condition, const std::string &message) {
        if (!condition) {
            std::fprintf(stderr, "FAIL: %s\n", message.c_str());
            failures++;
        }
    }

    // Real permissions of the page containing `address` ("---p", "r-xp", "rwxp", ...), read from the kernel
    std::string Perms(const void *address) {
        std::ifstream maps("/proc/self/maps");
        const auto target{reinterpret_cast<uintptr_t>(address)};
        for (std::string line; std::getline(maps, line);) {
            uintptr_t begin{}, end{};
            char perms[8]{};
            if (std::sscanf(line.c_str(), "%lx-%lx %7s", &begin, &end, perms) == 3 && target >= begin && target < end)
                return perms;
        }
        return "unmapped";
    }

    constexpr const char *RWX{"rwxp"}, *RX{"r-xp"}, *NONE{"---p"};

    struct Arena {
        u8 *base;
        size_t pages;
        explicit Arena(size_t pageCount) : pages{pageCount} {
            void *memory{mmap(nullptr, pageCount * Page, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)};
            if (memory == MAP_FAILED) {
                std::perror("mmap");
                std::exit(2);
            }
            base = static_cast<u8 *>(memory);
        }
        ~Arena() { munmap(base, pages * Page); }
        u8 *PageAt(size_t index) const { return base + index * Page; }
        span<u8> Range(size_t offset, size_t size) const { return span<u8>{base + offset, size}; }
    };

    void ExpectPages(const Arena &arena, size_t first, size_t count, const char *expected, const std::string &context) {
        for (size_t i{first}; i < first + count; i++) {
            const auto actual{Perms(arena.PageAt(i))};
            Check(actual == expected, context + ": page " + std::to_string(i) + " is " + actual + ", expected " + expected);
        }
    }

    TrapHandle Create(TrapManager &trap, span<u8> region, bool *wrote = nullptr) {
        std::vector<span<u8>> regions{region};
        return trap.CreateTrap(span<span<u8>>{regions}, [] {}, [] { return true; }, [wrote] {
            if (wrote) *wrote = true;
            return true;
        });
    }

    // ---- scenarios ----------------------------------------------------------------------------------------------------

    void WriteOnlyInstallsRxAndLastDeleteRestoresRwx() {
        Arena arena{16};
        TrapManager trap;
        auto handle{Create(trap, arena.Range(0, 6 * Page))};
        trap.TrapRegions(handle, true);
        ExpectPages(arena, 0, 6, RX, "write-only trap");
        ExpectPages(arena, 6, 2, RWX, "write-only trap neighbours");
        trap.DeleteTrap(handle);
        ExpectPages(arena, 0, 8, RWX, "after deleting the last trap");
        Check(!trap.HandleTrap(arena.PageAt(0), true), "no entry must remain after the last delete");
    }

    void ReadWriteTrapRemovesAllAccess() {
        Arena arena{8};
        TrapManager trap;
        auto handle{Create(trap, arena.Range(Page, 2 * Page))};
        trap.TrapRegions(handle, false);
        ExpectPages(arena, 1, 2, NONE, "read-write trap");
        trap.RemoveTrap(handle);
        ExpectPages(arena, 1, 2, RWX, "RemoveTrap of the only trap");
        trap.DeleteTrap(handle);
        ExpectPages(arena, 0, 8, RWX, "delete after RemoveTrap");
    }

    // The real crash: A covers 0x6000, B only part of it. Deleting A must leave only B's pages protected, and deleting B must
    // then leave nothing protected, otherwise pages without any entry stay RX and fault with no handler.
    void PartialOverlapDeleteLeavesNoStaleProtection() {
        Arena arena{16};
        TrapManager trap;
        auto a{Create(trap, arena.Range(0, 6 * Page))};
        auto b{Create(trap, arena.Range(2 * Page + 0x100, 0x80))}; // sub-page trap inside page 2
        trap.TrapRegions(a, true);
        trap.TrapRegions(b, true);
        ExpectPages(arena, 0, 6, RX, "A and B write-only");

        trap.DeleteTrap(a);
        ExpectPages(arena, 2, 1, RX, "after deleting A: B's page stays write-protected");
        ExpectPages(arena, 0, 2, RWX, "after deleting A: pages only A covered (0,1)");
        ExpectPages(arena, 3, 3, RWX, "after deleting A: pages only A covered (3..5)");

        trap.DeleteTrap(b);
        ExpectPages(arena, 0, 8, RWX, "after deleting B");
        for (size_t i{}; i < 6; i++) {
            Check(!trap.HandleTrap(arena.PageAt(i), true), "page " + std::to_string(i) + " must have no entry after both deletes");
            Check(Perms(arena.PageAt(i)) == RWX, "page " + std::to_string(i) + " must be writable once nothing traps it");
        }
    }

    // Same, with the delete order reversed and a read-write B (stronger than A)
    void OverlappingTrapsKeepMostRestrictiveProtection() {
        Arena arena{16};
        TrapManager trap;
        auto a{Create(trap, arena.Range(0, 6 * Page))};
        auto b{Create(trap, arena.Range(3 * Page, 2 * Page))}; // pages 3,4
        trap.TrapRegions(a, true);
        trap.TrapRegions(b, false);
        ExpectPages(arena, 0, 3, RX, "A only pages");
        ExpectPages(arena, 3, 2, NONE, "pages shared with a read-write trap");
        ExpectPages(arena, 5, 1, RX, "A only tail page");

        trap.DeleteTrap(b);
        ExpectPages(arena, 0, 6, RX, "B deleted, A still write-protects everything it covers");

        trap.DeleteTrap(a);
        ExpectPages(arena, 0, 8, RWX, "A deleted");
    }

    void RemoveTrapRecomputesPerPage() {
        Arena arena{16};
        TrapManager trap;
        auto a{Create(trap, arena.Range(0, 6 * Page))};
        auto b{Create(trap, arena.Range(4 * Page, 4 * Page))}; // pages 4..7, overlaps A on 4,5
        trap.TrapRegions(a, true);
        trap.TrapRegions(b, true);
        trap.RemoveTrap(a);
        ExpectPages(arena, 0, 4, RWX, "RemoveTrap(A): pages only A covered");
        ExpectPages(arena, 4, 4, RX, "RemoveTrap(A): B's pages remain protected");
        trap.DeleteTrap(a);
        trap.DeleteTrap(b);
        ExpectPages(arena, 0, 16, RWX, "all deleted");
    }

    // Traps whose edges are not page aligned share their edge pages
    void UnalignedEdgesShareBoundaryPages() {
        Arena arena{8};
        TrapManager trap;
        auto a{Create(trap, arena.Range(0x10, Page))};           // pages 0 and 1 (straddles)
        auto b{Create(trap, arena.Range(Page + 0x800, Page))};   // pages 1 and 2
        trap.TrapRegions(a, true);
        trap.TrapRegions(b, true);
        ExpectPages(arena, 0, 3, RX, "two unaligned traps");
        trap.DeleteTrap(a);
        ExpectPages(arena, 0, 1, RWX, "page 0 belonged only to A");
        ExpectPages(arena, 1, 2, RX, "pages 1,2 still belong to B");
        trap.DeleteTrap(b);
        ExpectPages(arena, 0, 8, RWX, "all unaligned traps deleted");
    }

    // A multi-interval group (a texture with several guest mappings) protects each of its intervals
    void MultiIntervalGroups() {
        Arena arena{16};
        TrapManager trap;
        std::vector<span<u8>> regions{arena.Range(0, 2 * Page), arena.Range(8 * Page, 2 * Page)};
        auto group{trap.CreateTrap(span<span<u8>>{regions}, [] {}, [] { return true; }, [] { return true; })};
        auto single{Create(trap, arena.Range(Page, 8 * Page))}; // bridges the gap, overlaps pages 1..8
        trap.TrapRegions(group, true);
        trap.TrapRegions(single, true);
        ExpectPages(arena, 0, 10, RX, "group + bridge");
        trap.DeleteTrap(single);
        ExpectPages(arena, 0, 2, RX, "group first interval");
        ExpectPages(arena, 2, 6, RWX, "gap between the group's intervals");
        ExpectPages(arena, 8, 2, RX, "group second interval");
        trap.DeleteTrap(group);
        ExpectPages(arena, 0, 16, RWX, "all deleted");
    }

    void HandledWriteReleasesAndLaterDeleteIsClean() {
        Arena arena{8};
        TrapManager trap;
        bool wrote{};
        auto handle{Create(trap, arena.Range(0, 3 * Page), &wrote)};
        trap.TrapRegions(handle, true);
        Check(trap.HandleTrap(arena.PageAt(1), true) && wrote, "write fault must be handled by the trap");
        ExpectPages(arena, 0, 3, RWX, "after a handled write fault");
        trap.TrapRegions(handle, true);
        ExpectPages(arena, 0, 3, RX, "re-trapped");
        trap.DeleteTrap(handle);
        ExpectPages(arena, 0, 8, RWX, "deleted after being re-trapped");
    }

    // ---- IntervalMap lookups against a linear oracle ------------------------------------------------------------------

    void FuzzIntervalMapLookups(uint32_t seed) {
        constexpr size_t Span{64 * Page};
        Arena arena{Span / Page + 1}; // page aligned, like guest memory: the fault lookup aligns absolute addresses
        u8 *base{arena.base};
        std::mt19937 rng{seed};

        IntervalMap<u8 *, int> map;
        struct Model { IntervalMap<u8 *, int>::GroupHandle handle; int id; std::vector<std::pair<size_t, size_t>> intervals; };
        std::vector<Model> models;
        int nextId{};

        auto overlapping{[&](size_t begin, size_t end) {
            std::set<int> ids;
            for (const auto &model : models)
                for (const auto &[b, e] : model.intervals)
                    if (b < end && e > begin)
                        ids.insert(model.id);
            return ids;
        }};

        for (int step{}; step < 300; step++) {
            if (rng() % 4 != 0 || models.empty()) {
                std::vector<span<u8>> regions;
                Model model;
                model.id = nextId++;
                for (size_t i{}, count{1 + rng() % 3}; i < count; i++) {
                    const size_t begin{rng() % (Span - 8)};
                    const size_t size{1 + rng() % std::min<size_t>(20 * Page, Span - begin)};
                    regions.push_back(span<u8>{base + begin, size});
                    model.intervals.emplace_back(begin, begin + size);
                }
                model.handle = map.Insert(span<span<u8>>{regions}, model.id);
                models.push_back(std::move(model));
            } else {
                const auto index{rng() % models.size()};
                map.Remove(models[index].handle);
                models.erase(models.begin() + static_cast<ptrdiff_t>(index));
            }

            for (int probe{}; probe < 8; probe++) {
                const size_t begin{rng() % Span};
                const size_t end{begin + 1 + rng() % (4 * Page)};

                std::set<int> got;
                for (const auto &entry : map.GetRange({base + begin, base + end}))
                    got.insert(entry.get());
                Check(got == overlapping(begin, end), "IntervalMap::GetRange differs from the oracle (seed " + std::to_string(seed) + ", step " + std::to_string(step) + ")");

                const bool any{!overlapping(begin, begin + 1).empty()};
                Check((map.Get(base + begin) != nullptr) == any, "IntervalMap::Get differs from the oracle (seed " + std::to_string(seed) + ", step " + std::to_string(step) + ")");

                // The fault lookup must return every entry that overlaps the faulting page, otherwise a protected page looks entry-less
                const size_t pageBegin{begin / Page * Page};
                std::set<int> faultEntries;
                for (const auto &entry : map.GetAlignedRecursiveRange<Page>(base + begin).first)
                    faultEntries.insert(entry.get());
                const auto required{overlapping(pageBegin, pageBegin + Page)};
                Check(std::includes(faultEntries.begin(), faultEntries.end(), required.begin(), required.end()),
                      "IntervalMap::GetAlignedRecursiveRange missed an entry overlapping the faulting page (seed " + std::to_string(seed) + ", step " + std::to_string(step) + ")");
            }
        }
    }

    // ---- randomized model check --------------------------------------------------------------------------------------

    struct ModelTrap {
        std::optional<TrapHandle> handle;
        std::vector<std::pair<size_t, size_t>> intervals; // byte [begin, end)
        TrapProtection protection{TrapProtection::None};
    };

    // With `faults`, handled accesses (HandleTrap) are mixed in. Those legitimately release protection in ways the simple model does not track,
    // so only the invariant of the crash is checked there: a page that no live trap overlaps is never left protected.
    void FuzzAgainstPerPageOracle(uint32_t seed, bool faults = false) {
        constexpr size_t Pages{48};
        Arena arena{Pages};
        TrapManager trap;
        std::mt19937 rng{seed};
        std::vector<ModelTrap> traps;

        auto expectedProtection{[&](size_t page) {
            TrapProtection strongest{TrapProtection::None};
            for (const auto &model : traps)
                for (const auto &[begin, end] : model.intervals)
                    if (begin < (page + 1) * Page && end > page * Page)
                        strongest = std::max(strongest, model.protection);
            return strongest;
        }};

        for (int step{}; step < 400; step++) {
            const auto operation{rng() % 5};
            if (operation == 0 && traps.size() < 8) {
                std::vector<span<u8>> regions;
                ModelTrap model;
                const auto count{1 + rng() % 3};
                for (size_t i{}; i < count; i++) {
                    const size_t begin{rng() % (Pages * Page - 16)};
                    const size_t size{1 + rng() % std::min<size_t>(10 * Page, Pages * Page - begin)};
                    regions.push_back(arena.Range(begin, size));
                    model.intervals.emplace_back(begin, begin + size);
                }
                model.handle = trap.CreateTrap(span<span<u8>>{regions}, [] {}, [] { return true; }, [] { return true; });
                traps.push_back(std::move(model));
            } else if (operation == 1 && !traps.empty()) {
                auto &model{traps[rng() % traps.size()]};
                const bool writeOnly{(rng() & 1) != 0};
                trap.TrapRegions(*model.handle, writeOnly);
                model.protection = writeOnly ? TrapProtection::WriteOnly : TrapProtection::ReadWrite;
            } else if (operation == 2 && !traps.empty()) {
                auto &model{traps[rng() % traps.size()]};
                trap.RemoveTrap(*model.handle);
                model.protection = TrapProtection::None;
            } else if (operation == 3 && !traps.empty()) {
                const auto index{rng() % traps.size()};
                trap.DeleteTrap(*traps[index].handle);
                traps.erase(traps.begin() + static_cast<ptrdiff_t>(index));
            } else if (faults && operation == 4 && !traps.empty()) {
                const auto &[begin, end]{traps[rng() % traps.size()].intervals[0]};
                const size_t offset{begin + rng() % (end - begin)};
                trap.HandleTrap(arena.base + offset, (rng() & 1) != 0);
            } else {
                continue;
            }

            if (faults) {
                for (size_t page{}; page < Pages; page++)
                    if (expectedProtection(page) == TrapProtection::None && !([&] {
                            for (const auto &model : traps)
                                for (const auto &[begin, end] : model.intervals)
                                    if (begin < (page + 1) * Page && end > page * Page)
                                        return true;
                            return false;
                        }()) && Perms(arena.PageAt(page)) != RWX) {
                        Check(false, "fault fuzz seed " + std::to_string(seed) + " step " + std::to_string(step) + ": page " + std::to_string(page) + " has no covering trap but is " + Perms(arena.PageAt(page)));
                        return;
                    }
                continue;
            }

            for (size_t page{}; page < Pages; page++) {
                const char *expected{[&] {
                    switch (expectedProtection(page)) {
                        case TrapProtection::None: return RWX;
                        case TrapProtection::WriteOnly: return RX;
                        default: return NONE;
                    }
                }()};
                const auto actual{Perms(arena.PageAt(page))};
                if (actual != expected) {
                    Check(false, "fuzz seed " + std::to_string(seed) + " step " + std::to_string(step) + " page " + std::to_string(page) +
                                     ": actual " + actual + ", expected " + expected);
                    return;
                }
            }
        }

        for (auto &model : traps)
            trap.DeleteTrap(*model.handle);
        for (size_t page{}; page < Pages; page++) {
            Check(Perms(arena.PageAt(page)) == RWX, "fuzz seed " + std::to_string(seed) + ": page " + std::to_string(page) + " stale after deleting every trap");
            Check(!trap.HandleTrap(arena.PageAt(page), true), "fuzz seed " + std::to_string(seed) + ": entry left on page " + std::to_string(page));
        }
    }
}

int main() {
    WriteOnlyInstallsRxAndLastDeleteRestoresRwx();
    ReadWriteTrapRemovesAllAccess();
    PartialOverlapDeleteLeavesNoStaleProtection();
    OverlappingTrapsKeepMostRestrictiveProtection();
    RemoveTrapRecomputesPerPage();
    UnalignedEdgesShareBoundaryPages();
    MultiIntervalGroups();
    HandledWriteReleasesAndLaterDeleteIsClean();
    for (uint32_t seed{1}; seed <= 40; seed++)
        FuzzAgainstPerPageOracle(seed);
    for (uint32_t seed{1}; seed <= 40; seed++)
        FuzzAgainstPerPageOracle(seed, true);
    for (uint32_t seed{1}; seed <= 40; seed++)
        FuzzIntervalMapLookups(seed);

    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("PASS: trap protections are recomputed per page; no page keeps a protection without a covering trap");
    return 0;
}
