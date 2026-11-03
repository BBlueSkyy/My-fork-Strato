// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace skyline {
    // Identity of a translation association, NOT of a physical allocation.
    // The owner serializes Capture/Invalidate with its translation table lock.
    // A publication lease never acquires that table lock or waits for GPU work.
    class MappingIdentity {
        struct Generation { uint64_t value; };
        struct Cell {
            uint64_t end;
            std::shared_ptr<const Generation> generation;
        };
        struct State {
            std::shared_mutex mutex;
            std::map<uint64_t, Cell> cells;
            std::atomic<uint64_t> revision{1};
            uint64_t nextGeneration{1};
            bool closed{};
        };
        struct Range {
            uint64_t begin, end;
            std::shared_ptr<const Generation> generation;
        };
        struct Receipt {
            std::shared_ptr<State> state;
            std::vector<Range> ranges;
            uint64_t begin{}, end{};
            mutable std::atomic<uint64_t> checkedRevision{};
            mutable std::atomic<bool> current{true};
        };
        std::shared_ptr<State> state{std::make_shared<State>()};

        static uint64_t End(uint64_t begin, uint64_t size) {
            if (!size || size > std::numeric_limits<uint64_t>::max() - begin)
                throw std::out_of_range("Invalid mapping identity range");
            return begin + size;
        }
        static bool MatchesLocked(const Receipt &receipt, uint64_t begin, uint64_t end) {
            if (receipt.state->closed || begin < receipt.begin || end > receipt.end) return false;
            for (const auto &range : receipt.ranges) {
                auto cursor{std::max(begin, range.begin)};
                const auto last{std::min(end, range.end)};
                if (cursor >= last) continue;
                auto entry{receipt.state->cells.upper_bound(cursor)};
                if (entry == receipt.state->cells.begin()) return false;
                --entry;
                while (cursor < last) {
                    if (entry == receipt.state->cells.end() || entry->first > cursor || entry->second.end <= cursor ||
                        entry->second.generation != range.generation) return false;
                    cursor = std::min(last, entry->second.end);
                    ++entry;
                }
            }
            return true;
        }
        // Only current associations are stored. Old generations survive solely
        // in receipts retained by pending operations. Reclaim unreferenced cells
        // at the metadata ceiling, never on a draw/dispatch or publication.
        static void Reclaim(State &value) {
            struct Count { size_t references{}; bool reclaim{}; };
            std::unordered_map<const Generation *, Count> counts;
            for (const auto &[begin, cell] : value.cells) ++counts[cell.generation.get()].references;
            for (const auto &[begin, cell] : value.cells) {
                auto &count{counts.at(cell.generation.get())};
                count.reclaim = cell.generation.use_count() == static_cast<long>(count.references);
            }
            for (auto it{value.cells.begin()}; it != value.cells.end();) {
                if (counts.at(it->second.generation.get()).reclaim)
                    it = value.cells.erase(it);
                else ++it;
            }
        }

      public:
        static constexpr size_t MaxCells{65536};
        static constexpr size_t MaxReceiptRanges{4096};
        class Snapshot;
        class Epoch;
        class Guard {
            friend class Snapshot;
            friend class Epoch;
            std::shared_ptr<State> state;
            std::shared_lock<std::shared_mutex> lock;
            bool valid{true};
            explicit Guard(const std::shared_ptr<Receipt> &receipt) {
                if (!receipt) return;
                state = receipt->state;
                lock = std::shared_lock{state->mutex};
                valid = MatchesLocked(*receipt, receipt->begin, receipt->end);
            }
          public:
            Guard() = default;
            Guard(Guard &&) = default;
            Guard &operator=(Guard &&) = default;
            explicit operator bool() const { return valid; }
            bool Matches(const Snapshot &snapshot) const;
        };
        class Epoch {
            friend class MappingIdentity;
            friend class Snapshot;
            std::shared_ptr<State> state;
            uint64_t revision{};
            Epoch(std::shared_ptr<State> value, uint64_t version) : state{std::move(value)}, revision{version} {}
          public:
            Epoch() = default;
            explicit operator bool() const { return static_cast<bool>(state); }
            bool SameVersion(const Epoch &other) const { return state == other.state && revision == other.revision; }
            Guard Acquire() const {
                Guard guard;
                if (state) {
                    guard.state = state;
                    guard.lock = std::shared_lock{state->mutex};
                    guard.valid = !state->closed && state->revision.load(std::memory_order_relaxed) == revision;
                }
                return guard;
            }
        };
        class Snapshot {
            friend class MappingIdentity;
            friend class Guard;
            std::shared_ptr<Receipt> receipt;
            explicit Snapshot(std::shared_ptr<Receipt> value) : receipt{std::move(value)} {}
          public:
            Snapshot() = default;
            explicit operator bool() const { return static_cast<bool>(receipt); }
            bool SameReceipt(const Snapshot &other) const { return receipt == other.receipt; }
            bool IsCurrent() const {
                if (!receipt) return true;
                if (!receipt->current.load(std::memory_order_acquire)) return false;
                const auto revision{receipt->state->revision.load(std::memory_order_acquire)};
                if (receipt->checkedRevision.load(std::memory_order_acquire) == revision)
                    return receipt->current.load(std::memory_order_acquire);
                std::shared_lock lock{receipt->state->mutex};
                const bool current{MatchesLocked(*receipt, receipt->begin, receipt->end)};
                if (!current) receipt->current.store(false, std::memory_order_release);
                else receipt->checkedRevision.store(receipt->state->revision.load(std::memory_order_relaxed), std::memory_order_release);
                return current;
            }
            bool IsCurrent(uint64_t begin, uint64_t size) const {
                if (!receipt) return false;
                const auto end{End(begin, size)};
                std::shared_lock lock{receipt->state->mutex};
                return MatchesLocked(*receipt, begin, end);
            }
            uint64_t GenerationAt(uint64_t address) const {
                if (receipt) for (const auto &range : receipt->ranges)
                    if (range.begin <= address && address < range.end) return range.generation->value;
                return 0;
            }
            Guard Acquire() const { return Guard{receipt}; }
            Epoch ReadEpoch() const {
                if (!receipt) return {};
                return Epoch{receipt->state, receipt->state->revision.load(std::memory_order_acquire)};
            }
        };
        using Mutation = std::unique_lock<std::shared_mutex>;
        MappingIdentity() = default;
        MappingIdentity(const MappingIdentity &) = delete;
        ~MappingIdentity() {
            std::unique_lock lock{state->mutex};
            state->closed = true;
            state->cells.clear();
            ++state->revision;
        }
        // Failure-only diagnostic: observe existing identity without minting a
        // receipt, changing history or touching the translation's backing bytes.
        struct Description { uint64_t revision{}, generation{}, begin{}, end{}; };
        Description Inspect(uint64_t address) const {
            std::shared_lock lock{state->mutex};
            const auto revision{state->revision.load(std::memory_order_relaxed)};
            auto entry{state->cells.upper_bound(address)};
            if (entry != state->cells.begin()) {
                --entry;
                if (entry->second.end > address)
                    return {revision, entry->second.generation->value, entry->first, entry->second.end};
            }
            return {revision, 0, address, address}; // No tracked identity, not an absent-page assertion.
        }
        uint64_t Revision() const { return state->revision.load(std::memory_order_acquire); }
        Epoch ReadEpoch() const { return Epoch{state, Revision()}; }
        Snapshot Capture(uint64_t begin, uint64_t size) {
            const auto end{End(begin, size)};
            std::unique_lock lock{state->mutex};
            auto receipt{std::make_shared<Receipt>()};
            receipt->state = state; receipt->begin = begin; receipt->end = end;
            auto cursor{begin};
            while (cursor < end) {
                if (receipt->ranges.size() == MaxReceiptRanges) throw std::length_error("Too many translation identity fragments");
                auto entry{state->cells.upper_bound(cursor)};
                if (entry != state->cells.begin() && std::prev(entry)->second.end > cursor) {
                    --entry;
                } else {
                    const auto last{entry == state->cells.end() ? end : std::min(end, entry->first)};
                    if (state->cells.size() == MaxCells) Reclaim(*state);
                    if (state->cells.size() == MaxCells) throw std::length_error("Translation identity metadata limit");
                    if (state->nextGeneration == std::numeric_limits<uint64_t>::max())
                        throw std::overflow_error("Translation identity generation exhausted");
                    const auto generation{std::make_shared<const Generation>(Generation{state->nextGeneration++})};
                    entry = state->cells.emplace(cursor, Cell{last, generation}).first;
                }
                const auto last{std::min(end, entry->second.end)};
                receipt->ranges.push_back({cursor, last, entry->second.generation});
                cursor = last;
            }
            receipt->checkedRevision.store(state->revision.load(std::memory_order_relaxed));
            return Snapshot{std::move(receipt)};
        }
        // Must span the actual table mutation. Prepare the only possible new
        // right-tail node before editing, so allocation failure leaves receipts
        // and the translation table unchanged. No callbacks run under this lock.
        Mutation Invalidate(uint64_t begin, uint64_t size) {
            End(begin, size);
            Mutation lock{state->mutex};
            InvalidateLocked(lock, begin, size);
            return lock;
        }
        void InvalidateLocked(Mutation &lock, uint64_t begin, uint64_t size) {
            if (!lock.owns_lock() || lock.mutex() != &state->mutex) throw std::logic_error("Invalid translation mutation lease");
            const auto end{End(begin, size)};
            auto first{state->cells.upper_bound(begin)};
            if (first != state->cells.begin() && std::prev(first)->second.end > begin) --first;
            const auto last{state->cells.lower_bound(end)};
            std::map<uint64_t, Cell> tail;
            if (first != last && std::prev(last)->second.end > end) {
                if (state->cells.size() == MaxCells && first->first < begin && std::next(first) == last)
                    throw std::length_error("Translation identity split limit");
                tail.emplace(end, std::prev(last)->second);
            }
            if (first != last && first->first < begin) { first->second.end = begin; ++first; }
            state->cells.erase(first, last);
            if (!tail.empty()) state->cells.insert(tail.extract(tail.begin()));
            ++state->revision;
        }
    };
    inline bool MappingIdentity::Guard::Matches(const Snapshot &snapshot) const {
        if (!snapshot.receipt) return true;
        return valid && state == snapshot.receipt->state &&
            MatchesLocked(*snapshot.receipt, snapshot.receipt->begin, snapshot.receipt->end);
    }
}
