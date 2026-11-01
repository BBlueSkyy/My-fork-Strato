// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <algorithm>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>
namespace skyline::gpu {
    // Independent page-aligned authority map. Boundaries only split, so trap
    // callbacks can safely keep scalar ranges while a later partition is added.
    class BufferRegionState {
      public:
        enum class Owner { Synced, Cpu, Gpu };
        struct Range {
            size_t begin, end;
        };
        struct Region : Range {
            Owner owner;
        };

      private:
        size_t total{}, page{4096};
        std::vector<Region> regions;
        std::vector<Range> accesses;
        bool Split(size_t offset) {
            auto it = std::find_if(regions.begin(), regions.end(), [&](auto r) { return r.begin < offset && offset < r.end; });
            if (it == regions.end())
                return false;
            auto index = static_cast<size_t>(it - regions.begin());
            auto right = *it;
            right.begin = offset;
            regions.insert(it + 1, right);
            regions[index].end = offset;
            return true;
        }

      public:
        void Reset(size_t size, size_t pageSize) {
            if (!pageSize || (pageSize & (pageSize - 1)))
                throw std::invalid_argument("Invalid buffer page size");
            std::vector<Region> next;
            if (size)
                next.push_back({{0, size}, Owner::Cpu});
            regions.swap(next);
            total = size;
            page = pageSize;
            accesses.clear();
        }
        Range Aligned(size_t offset, size_t size) const {
            if (offset > total || size > total - offset)
                throw std::out_of_range("Buffer region out of bounds");
            if (!size)
                return {offset, offset};
            const auto end = offset + size;
            const auto padding = (page - end % page) % page;
            return {offset - offset % page, end + std::min(padding, total - end)};
        }
        bool EnsureBounds(size_t offset, size_t size) {
            auto r = Aligned(offset, size);
            if (r.begin == r.end)
                return false;
            if (regions.capacity() - regions.size() < 2) {
                if (regions.size() > regions.max_size() - 2)
                    throw std::length_error("Too many buffer regions");
                auto wanted = regions.size() + 2;
                auto growth = regions.capacity() <= regions.max_size() / 2 ? regions.capacity() * 2 : regions.max_size();
                regions.reserve(std::max(wanted, growth));
            }
            bool changed = Split(r.begin);
            return Split(r.end) || changed;
        }
        void AssignExisting(size_t offset, size_t size, Owner owner) {
            auto r = Aligned(offset, size);
            if (r.begin == r.end)
                return;
            auto first = std::find_if(regions.begin(), regions.end(), [&](auto p) { return p.begin == r.begin; });
            auto last = std::find_if(regions.begin(), regions.end(), [&](auto p) { return p.end == r.end; });
            if (first == regions.end() || last == regions.end())
                throw std::logic_error("Unprotected buffer partition boundary");
            for (auto it = first;; ++it) {
                it->owner = owner;
                if (it == last)
                    break;
            }
        }
        bool Has(Owner owner, size_t offset, size_t size) const {
            auto r = Aligned(offset, size);
            if (r.begin == r.end)
                return false;
            return std::any_of(regions.begin(), regions.end(), [&](auto p) { return p.owner == owner && p.begin < r.end && r.begin < p.end; });
        }
        bool Has(Owner owner) const {
            return Has(owner, 0, total);
        }
        const auto& Regions() const {
            return regions;
        }
        void Use(size_t offset, size_t size) {
            auto r = Aligned(offset, size);
            if (r.begin == r.end)
                return;
            auto first = std::find_if(accesses.begin(), accesses.end(), [&](auto p) { return p.end >= r.begin; });
            if (first == accesses.end() || first->begin > r.end) {
                accesses.insert(first, r);
                return;
            }
            auto last = first;
            while (last != accesses.end() && last->begin <= r.end) {
                r.begin = std::min(r.begin, last->begin);
                r.end = std::max(r.end, last->end);
                ++last;
            }
            first->begin = r.begin;
            first->end = r.end;
            accesses.erase(first + 1, last);
        }
        const auto& Accesses() const {
            return accesses;
        }
        void ClearAccesses() {
            accesses.clear();
        }
    };
} // namespace skyline::gpu
