// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "lifecycle_trace.h"
#include <functional>
#include <string>

namespace skyline::gpu::texture::diagnostic {
    // Deliberate readback of candidate inputs, not a complete RenderDoc frame.
    class MiniCapture {
        struct Entry { uint32_t phase; uint64_t frame; Snapshot snapshot; std::function<std::vector<uint8_t>()> read; };
        std::mutex mutex;
        std::atomic<uint32_t> armed{};
        std::array<std::vector<uint64_t>, 2> selected;
        std::array<uint64_t, 2> frames{};
        std::vector<Entry> entries;
      public:
        bool Armed() const { return armed.load(std::memory_order_relaxed) != 0; }
        void Reset() {
            std::lock_guard lock{mutex}; armed=0; selected={}; frames={}; entries.clear();
        }
        bool Arm(uint32_t phase) {
            if (phase!=1 && phase!=3) return false;
            std::lock_guard lock{mutex};
            if (!selected[phase==3].empty()) return false;
            armed=phase;
            return true;
        }
        uint32_t Reserve(uint64_t id, uint64_t frame) {
            if (!Armed()) return 0;
            std::lock_guard lock{mutex};
            const auto phase{armed.load()}; if (!phase) return 0;
            auto &ids{selected[phase==3]}; auto &first{frames[phase==3]};
            if (ids.empty()) first=frame;
            if (frame!=first || ids.size()==8) { armed=0; return 0; }
            for (auto previous:ids) if (previous==id) return 0;
            ids.push_back(id); return phase;
        }
        void Commit(uint32_t phase, uint64_t frame, Snapshot snapshot, std::function<std::vector<uint8_t>()> read) {
            std::lock_guard lock{mutex}; entries.push_back({phase,frame,snapshot,std::move(read)});
        }
        bool Export(std::ostream &out) {
            std::vector<Entry> copy;
            { std::lock_guard lock{mutex}; armed=0; copy=entries; }
            bool black{}, correct{};
            for (const auto &e:copy) { black|=e.phase==1; correct|=e.phase==3; }
            if (!black || !correct) return false;
            out << "MK8MINI1\n";
            for (const auto &e:copy) {
                auto bytes{e.read()}; if (bytes.empty()) return false;
                const auto &s{e.snapshot};
                out<<e.phase<<','<<e.frame<<','<<s.id<<','<<s.address<<','<<s.image<<','<<s.generation<<','
                   <<s.hostFormat<<','<<s.width<<','<<s.height<<','<<s.levels<<','<<s.layers<<','<<s.detail<<','
                   <<s.mip<<','<<s.levelCount<<','<<s.layer<<','<<s.layerCount<<','<<s.swizzle<<','<<s.writeSerial<<','<<bytes.size()<<'\n';
                out.write(reinterpret_cast<const char *>(bytes.data()),bytes.size()); out.put('\n');
            }
            return out.good();
        }
    };
}
