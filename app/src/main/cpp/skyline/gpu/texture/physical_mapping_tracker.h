// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <vector>
#include <utility>

namespace skyline::gpu::texture {
    // CPU-address spans are cache identities, not certificates that a guest
    // allocation was freed. Only CPU-authoritative, unaliased images may use
    // this tracker for eviction; GPU-owned data is never discarded here.
    class PhysicalMappingTracker {
      public:
        struct Range { uintptr_t begin{}, end{}; };
        std::recursive_mutex mutex;
      private:
        struct Mapping { uint64_t end{}; uintptr_t physical{}; };
        std::map<uint64_t, std::map<uint64_t, Mapping>> spaces;
        std::vector<Range> changed, resident;
        uint64_t nextSpace{}, epoch{};
        bool valid{true}, dirty{true}, fullSweep{};
        static constexpr size_t MaxChanges{4096};

        void Changed(Range range) {
            if (range.begin >= range.end) return;
            if (changed.size() < MaxChanges) changed.push_back(range);
            else { changed.clear(); fullSweep = true; }
        }
      public:
        uint64_t Register() {
            std::scoped_lock lock{mutex};
            const auto id{++nextSpace};
            spaces.emplace(id, std::map<uint64_t, Mapping>{});
            return id;
        }
        uint64_t Generation() { std::scoped_lock lock{mutex}; return epoch; }
        bool HasChanges() const { return fullSweep || !changed.empty(); }
        bool Valid() const { return valid; }

        // Called only after the real GMMU mutation succeeded. Failure closes
        // collection permanently; emulation never relies on this index for VA translation.
        void Map(uint64_t space, uint64_t address, uintptr_t physical, uint64_t size) noexcept {
            std::scoped_lock lock{mutex};
            if (!size) return;
            dirty = true;
            bool overwritten{};
            try {
                if (address > UINT64_MAX - size || (physical && physical > UINTPTR_MAX - size)) {
                    valid = false; ++epoch; return;
                }
                auto &mappings{spaces.at(space)};
                const auto end{address + size};
                auto it{mappings.upper_bound(address)};
                if (it != mappings.begin()) --it;
                while (it != mappings.end() && it->first < end) {
                    const auto start{it->first};
                    const auto old{it->second};
                    if (old.end <= address) { ++it; continue; }
                    const auto first{std::max(start, address)}, last{std::min(old.end, end)};
                    overwritten = true;
                    Changed({old.physical + first - start, old.physical + last - start});
                    it = mappings.erase(it);
                    if (start < address)
                        mappings.emplace(start, Mapping{address, old.physical});
                    if (old.end > end) {
                        mappings.emplace(end, Mapping{old.end, old.physical + end - start});
                        break;
                    }
                }
                if (physical) mappings.emplace(address, Mapping{end, physical});
                if (overwritten) ++epoch;
            } catch (...) { valid = false; ++epoch; }
        }
        void Remove(uint64_t space) noexcept {
            std::scoped_lock lock{mutex};
            dirty = true;
            try {
                auto it{spaces.find(space)};
                if (it == spaces.end()) return;
                if (!it->second.empty()) ++epoch;
                for (const auto &[va, map] : it->second)
                    Changed({map.physical, map.physical + map.end - va});
                spaces.erase(it);
            } catch (...) { valid = false; }
        }
        std::vector<Range> TakeChanges(bool &sweep) {
            sweep = std::exchange(fullSweep, false);
            std::vector<Range> result;
            result.swap(changed);
            return result;
        }
        void Prepare() {
            if (!dirty || !valid) return;
            std::vector<Range> result;
            for (const auto &[space, mappings] : spaces)
                for (const auto &[va, map] : mappings)
                    result.push_back({map.physical, map.physical + map.end - va});
            std::sort(result.begin(), result.end(), [](auto a, auto b) { return a.begin < b.begin; });
            resident.clear();
            for (auto range : result) {
                if (!resident.empty() && range.begin <= resident.back().end)
                    resident.back().end = std::max(resident.back().end, range.end);
                else resident.push_back(range);
            }
            dirty = false;
        }
        bool IsMapped(uintptr_t begin, size_t size) const {
            if (!valid || dirty || begin > UINTPTR_MAX - size) return true;
            const auto end{begin + size};
            auto it{std::lower_bound(resident.begin(), resident.end(), begin,
                [](Range range, uintptr_t value) { return range.end <= value; })};
            return it != resident.end() && it->begin < end;
        }
    };
}
