// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <span>
#include <vector>

namespace skyline::gpu::texture {
    // Stage C / I3 metadata-only prototype. The caller must hold the owning
    // regional mutex and submit/retain pending GPU users before registration.
    // Tokens must uniquely identify a resource/backing generation (no pointer ABA).
    enum class GuestBridgeResourceKind : uint8_t { Buffer, Image };
    struct GuestBridgeOwnedSpan { uintptr_t begin{}, end{}; };
    struct GuestBridgeParticipant {
        uint64_t token{};
        GuestBridgeResourceKind kind{};
        std::vector<GuestBridgeOwnedSpan> coverage; // sorted physical byte runs
        bool pendingUntrackedGpuUse{}; // requires executor unwind/retry
    };
    struct GuestBridgeCell {
        uintptr_t begin{}, end{};
        uint64_t hostVersion{}, guestVersion{};
        uint64_t writerToken{};
        GuestBridgeResourceKind writerKind{};
    };

    // Contains no GPU transport or CPU publication. Join() is transactional:
    // no old owners or versions may be silently dropped when peers arrive.
    class GuestBridgeLedger {
      public:
        const std::vector<GuestBridgeCell> &Cells() const { return cells; }
        const std::vector<GuestBridgeParticipant> &Participants() const { return participants; }

        bool Join(std::span<const GuestBridgeParticipant> incoming, size_t maxCells = 8192) {
            if (incoming.empty() || incoming.size() > 64 || !maxCells || maxCells > 65536) return false;
            std::vector<GuestBridgeParticipant> next(incoming.begin(), incoming.end());
            std::vector<uintptr_t> endpoints;
            endpoints.reserve(cells.size() * 2);
            for (const auto &cell : cells) {
                if (cell.begin >= cell.end || endpoints.size() + 2 > maxCells * 2) return false;
                endpoints.push_back(cell.begin); endpoints.push_back(cell.end);
            }
            for (const auto &member : next) {
                if (!member.token || member.pendingUntrackedGpuUse || member.coverage.empty()) return false;
                for (const auto &previous : next)
                    if (&member != &previous && member.token == previous.token) return false;
                uintptr_t previousEnd{};
                for (const auto &span : member.coverage) {
                    if (span.begin >= span.end || span.begin < previousEnd ||
                        endpoints.size() + 2 > maxCells * 2) return false;
                    previousEnd = span.end;
                    endpoints.push_back(span.begin); endpoints.push_back(span.end);
                }
            }
            std::sort(endpoints.begin(), endpoints.end());
            endpoints.erase(std::unique(endpoints.begin(), endpoints.end()), endpoints.end());
            if (endpoints.size() < 2 || endpoints.size() - 1 > maxCells) return false;
            std::vector<GuestBridgeCell> nextCells;
            nextCells.reserve(endpoints.size() - 1);
            for (size_t i{1}; i < endpoints.size(); ++i) {
                const auto begin{endpoints[i - 1]}, end{endpoints[i]};
                bool covered{};
                for (const auto &member : next) covered |= Covers(member, begin, end);
                if (!covered) {
                    if (PriorCell(begin, end)) return false; // old component would be lost
                    continue;
                }
                GuestBridgeCell cell{begin, end};
                if (auto prior{PriorCell(begin, end)}) {
                    cell.hostVersion = prior->hostVersion;
                    cell.guestVersion = prior->guestVersion;
                    cell.writerToken = prior->writerToken;
                    cell.writerKind = prior->writerKind;
                    if (cell.guestVersion > cell.hostVersion) return false;
                    if (cell.writerToken) {
                        const auto owner{std::find_if(next.begin(), next.end(), [&](const auto &member) {
                            return member.token == cell.writerToken && member.kind == cell.writerKind &&
                                   Covers(member, begin, end);
                        })};
                        if (owner == next.end()) return false;
                    } else if (cell.hostVersion != cell.guestVersion) {
                        return false; // no source for unpublished guest bytes
                    }
                }
                nextCells.push_back(cell);
            }
            if (nextCells.empty()) return false;
            participants = std::move(next);
            cells = std::move(nextCells);
            return true;
        }

        // Called only after a real producer receipt is committed. Writes must
        // be completely covered; split boundaries to preserve foreign bytes.
        bool RecordWrite(uint64_t token, GuestBridgeResourceKind kind,
                         uintptr_t begin, uintptr_t end, uint64_t version, size_t maxCells = 8192) {
            if (!token || begin >= end || !version || !maxCells) return false;
            const auto member{std::find_if(participants.begin(), participants.end(), [&](const auto &item) {
                return item.token == token && item.kind == kind;
            })};
            if (member == participants.end()) return false;
            std::vector<GuestBridgeCell> updated;
            updated.reserve(cells.size() + 2);
            uintptr_t position{begin};
            for (const auto &cell : cells) {
                if (cell.end <= begin || cell.begin >= end) { updated.push_back(cell); continue; }
                const auto first{std::max(cell.begin, begin)}, last{std::min(cell.end, end)};
                if (first != position || !Covers(*member, first, last) ||
                    version <= cell.hostVersion) return false;
                if (cell.begin < first) { auto left{cell}; left.end = first; updated.push_back(left); }
                auto written{cell};
                written.begin = first; written.end = last; written.hostVersion = version;
                written.writerToken = token; written.writerKind = kind;
                updated.push_back(written);
                if (last < cell.end) { auto right{cell}; right.begin = last; updated.push_back(right); }
                position = last;
                if (updated.size() > maxCells) return false;
            }
            if (position != end || updated.size() > maxCells) return false;
            cells = std::move(updated);
            return true;
        }

        // Metadata confirmation ONLY; actual masked readback and publication
        // must happen before the caller confirms a matching writer/version.
        // Do not advance unrequested portions of the same owner's region.
        bool ConfirmGuestPublication(uint64_t token, GuestBridgeResourceKind kind,
                                     uintptr_t begin, uintptr_t end, uint64_t version,
                                     size_t maxCells = 8192) {
            if (!token || !version || begin >= end || !maxCells) return false;
            uintptr_t position{begin};
            std::vector<GuestBridgeCell> updated;
            updated.reserve(cells.size() + 2);
            for (const auto &cell : cells) {
                if (cell.end <= begin || cell.begin >= end) { updated.push_back(cell); continue; }
                const auto first{std::max(cell.begin, begin)}, last{std::min(cell.end, end)};
                if (first != position || cell.writerToken != token ||
                    cell.writerKind != kind || cell.hostVersion != version) return false;
                if (cell.begin < first) { auto left{cell}; left.end = first; updated.push_back(left); }
                auto published{cell};
                published.begin = first; published.end = last;
                published.guestVersion = published.hostVersion;
                updated.push_back(published);
                if (last < cell.end) { auto right{cell}; right.begin = last; updated.push_back(right); }
                position = last;
                if (updated.size() > maxCells) return false;
            }
            if (position != end || updated.size() > maxCells) return false;
            cells = std::move(updated);
            return true;
        }

      private:
        std::vector<GuestBridgeParticipant> participants;
        std::vector<GuestBridgeCell> cells;
        static bool Covers(const GuestBridgeParticipant &member, uintptr_t begin, uintptr_t end) {
            const auto it{std::upper_bound(member.coverage.begin(), member.coverage.end(), begin,
                [](uintptr_t value, const GuestBridgeOwnedSpan &span) { return value < span.begin; })};
            if (it == member.coverage.begin()) return false;
            const auto &span{*std::prev(it)};
            return span.begin <= begin && span.end >= end;
        }
        const GuestBridgeCell *PriorCell(uintptr_t begin, uintptr_t end) const {
            const auto it{std::upper_bound(cells.begin(), cells.end(), begin,
                [](uintptr_t value, const GuestBridgeCell &cell) { return value < cell.begin; })};
            if (it == cells.begin()) return nullptr;
            const auto &cell{*std::prev(it)};
            return cell.begin <= begin && cell.end >= end ? &cell : nullptr;
        }
    };
}
