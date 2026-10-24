// Host boundaries only. Real TrapManager/IntervalMap, mmap/mprotect and Vulkan
// are used. Observers record which production callback actually executes.
struct Memory {
    unsigned discards{};
    std::vector<std::pair<void *, size_t>> mirrors;
    ~Memory() { for (auto [address, size] : mirrors) assert(!munmap(address, size)); }
    span<u8> CreateMirror(span<u8> mapping) {
        auto &arena{*SharedGuestMemory::active};
        assert(mapping.data() >= arena.guest && mapping.data() + mapping.size() <= arena.guest + arena.size);
        auto *p{static_cast<u8 *>(mmap(nullptr, mapping.size(), PROT_READ | PROT_WRITE,
            MAP_SHARED, arena.fd, mapping.data() - arena.guest))};
        assert(p != MAP_FAILED);
        mirrors.emplace_back(p, mapping.size());
        return {p, mapping.size()};
    }
    span<u8> CreateMirrors(const std::vector<span<u8>> &mappings) {
        auto &arena{*SharedGuestMemory::active};
        size_t total{}; for (const auto &mapping : mappings) total+=mapping.size();
        auto *base{static_cast<u8 *>(mmap(nullptr,total,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0))};
        assert(base!=MAP_FAILED);
        size_t offset{};
        for (const auto &mapping : mappings) {
            assert(mapping.size()%4096==0 && (mapping.data()-arena.guest)%4096==0);
            assert(mmap(base+offset,mapping.size(),PROT_READ|PROT_WRITE,MAP_SHARED|MAP_FIXED,
                arena.fd,mapping.data()-arena.guest)==base+offset);
            offset+=mapping.size();
        }
        mirrors.emplace_back(base,total); return {base,total};
    }
    void FreeMemory(span<u8>) { ++discards; throw std::runtime_error("Guest eviction must remain disabled"); }
} memory;
struct Trap {
    std::recursive_mutex mutex; // Retained for unexecuted base-harness test declarations.
    struct Callbacks { std::function<void()> prepare; std::function<bool()> read, write; };
    std::vector<Callbacks> callbacks;
    std::vector<skyline::TrapHandle> handles;
    std::vector<std::pair<int, char>> events;
    std::atomic<unsigned> changes{};
    skyline::TrapManager manager;
    int CreateTrap(const std::vector<span<u8>> &mappings, std::function<void()> prepare,
                   std::function<bool()> read, std::function<bool()> write) {
        const auto id{static_cast<int>(callbacks.size())};
        callbacks.push_back({prepare,read,write});
        // CreateTrap does not retain the input span; production copies the regions.
        auto regions{mappings};
        handles.push_back(manager.CreateTrap(span<span<u8>>{regions},
            [this,id,prepare] { events.emplace_back(id,'p'); prepare(); },
            [this,id,read] { events.emplace_back(id,'r'); const bool ok{read()}; if (ok) events.emplace_back(id,'R'); return ok; },
            [this,id,write] { events.emplace_back(id,'w'); const bool ok{write()}; if (ok) events.emplace_back(id,'W'); return ok; }));
        return id;
    }
    void TrapRegions(int id, bool writeOnly) { ++changes; manager.TrapRegions(handles.at(id),writeOnly); }
    void RemoveTrap(int id) { ++changes; manager.RemoveTrap(handles.at(id)); }
    void DeleteTrap(int id) { ++changes; manager.DeleteTrap(handles.at(id)); }
} trap;
