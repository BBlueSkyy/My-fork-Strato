// Host boundaries for the exact production Buffer/BufferManager bodies.
// Vulkan storage is coherent host-visible memory; guest mirrors share actual bytes.
using GuestBuffer = span<u8>;
using TrapHandle = int;
struct GPU;
struct UsageTracker;
class BufferView; class TextureView; class BufferDelegate; class BufferManager; class TextureManager;
namespace interconnect { struct CommandExecutor; }
#include "buffer_support.inc"
template<class = void> struct LinearAllocatorState {
    std::vector<std::shared_ptr<void>> objects;
    template<class T, class... A> T *EmplaceUntracked(A&&... args) {
        auto object = std::make_shared<T>(std::forward<A>(args)...); auto result=object.get(); objects.push_back(std::move(object)); return result;
    }
};
struct MegaBufferAllocator {
    GPU *owner{};
    std::vector<std::shared_ptr<memory::StagingBuffer>> allocations;
    struct Allocation { vk::Buffer buffer{}; size_t offset{}; span<u8> region{}; explicit operator bool() const { return static_cast<bool>(buffer); } };
    Allocation Push(const std::shared_ptr<FenceCycle> &,span<u8>,bool = false);
};
constexpr size_t MegaBufferChunkSize{1024*1024};
namespace tsl { template<class K,class V,class H> using robin_map=std::unordered_map<K,V,H>; }
#include "buffer_class.inc"

// Interval/page lookup is the host boundary. Dispatch, callback retries and
// protection changes execute the exact production TrapManager::HandleTrap body.
// Actual mprotect at the end of HandleTrap applies to mmap-backed mixed tests.
enum class TrapProtection { None, WriteOnly, ReadWrite };
struct HostTrap {
    using LockCallback=std::function<void()>;
    std::recursive_mutex trapMutex;
    std::recursive_mutex &mutex{trapMutex};
    struct Callbacks {
        LockCallback prepare;
        std::function<bool()> read,write;
        std::vector<span<u8>> regions;
        TrapProtection protection{TrapProtection::None};
        bool deleted{};
        LockCallback lockCallback;
        std::function<bool()> readCallback,writeCallback;
        Callbacks(std::vector<span<u8>> mappings,LockCallback p,std::function<bool()> r,std::function<bool()> w)
            : prepare{std::move(p)},read{std::move(r)},write{std::move(w)},regions{std::move(mappings)},
              lockCallback{prepare},readCallback{read},writeCallback{write} {}
    };
    std::vector<Callbacks> callbacks;
    std::atomic<unsigned> changes{};
    bool reverseEntries{};
    struct Interval { u8 *start,*end; size_t Size() const { return end-start; } };
    struct Map {
        HostTrap *owner;
        template<size_t PageSize> auto GetAlignedRecursiveRange(u8 *address) {
            u8 *first=util::AlignDown(address,PageSize), *last=first+PageSize;
            bool changed{};
            do {
                changed=false;
                for (const auto &entry : owner->callbacks) if (!entry.deleted)
                    for (const auto &region : entry.regions) {
                        auto begin=util::AlignDown(region.data(),PageSize), end=util::AlignUp(region.data()+region.size(),PageSize);
                        if (begin>=last || end<=first) continue;
                        if (begin<first) { first=begin; changed=true; }
                        if (end>last) { last=end; changed=true; }
                    }
            } while(changed);
            std::vector<std::reference_wrapper<Callbacks>> entries;
            for (auto &entry : owner->callbacks) if (!entry.deleted && std::any_of(entry.regions.begin(),entry.regions.end(),[&](auto region) {
                return region.data()<last && region.data()+region.size()>first;
            })) entries.push_back(entry);
            if (owner->reverseEntries) std::reverse(entries.begin(),entries.end());
            return std::pair{std::move(entries),std::vector<Interval>{{first,last}}};
        }
    } trapMap{this};
    int CreateTrap(span<u8> mapping,LockCallback prepare,std::function<bool()> read,std::function<bool()> write) {
        return CreateTrap(std::vector<span<u8>>{mapping},std::move(prepare),std::move(read),std::move(write));
    }
    int CreateTrap(const std::vector<span<u8>> &mappings,LockCallback prepare,std::function<bool()> read,std::function<bool()> write) {
        std::scoped_lock lock{trapMutex}; callbacks.emplace_back(mappings,std::move(prepare),std::move(read),std::move(write));
        return static_cast<int>(callbacks.size()-1);
    }
    void TrapRegions(int handle,bool writeOnly) {
        std::scoped_lock lock{trapMutex}; ++changes;
        if (handle>=0 && static_cast<size_t>(handle)<callbacks.size()) callbacks[handle].protection=writeOnly ? TrapProtection::WriteOnly : TrapProtection::ReadWrite;
    }
    void DeleteTrap(int handle) {
        std::scoped_lock lock{trapMutex}; ++changes;
        if (handle>=0 && static_cast<size_t>(handle)<callbacks.size()) callbacks[handle].deleted=true;
    }
    bool HandleTrap(u8 *,bool);
};
#include "trap_handler.inc"
