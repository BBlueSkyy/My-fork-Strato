// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <list>
#include <memory>
#include <span>
#include <thread>
#include <unordered_set>
#include <vector>
#include <utility>
#include <common.h>
#include <vulkan/vulkan_raii.hpp>
using u8 = std::uint8_t;
namespace vk {
using DeviceSize = std::uint64_t;
using Buffer = std::uint64_t;
}
constexpr size_t PAGE_SIZE = 4096;
namespace ranges { using std::ranges::find_if; }
namespace skyline {
namespace util { inline size_t AlignUp(size_t value, size_t alignment) { return (value + alignment - 1) & ~(alignment - 1); } }
template<class T> struct span : std::span<T> {
    using std::span<T>::span;
    span(std::span<T> value) : std::span<T>{value} {}
    span subspan(size_t offset, size_t count = std::dynamic_extent) const { return std::span<T>::subspan(offset, count); }
    void copy_from(span source) { std::memcpy(this->data(), source.data(), source.size_bytes()); }
};
namespace gpu {
namespace memory {
struct Buffer : span<u8> {
    inline static size_t allocated{}, destroyed{}, peak{}, nextId{};
    inline static std::unordered_set<vk::Buffer> live;
    inline static std::thread::id lastDestruction;
    vk::Buffer vkBuffer{};
    std::unique_ptr<u8[]> storage;
    explicit Buffer(size_t size) : storage{new u8[size]} {
        vkBuffer = ++nextId;
        static_cast<span<u8>&>(*this) = span<u8>{storage.get(), size};
        live.insert(vkBuffer); ++allocated;
        peak = std::max(peak, live.size());
    }
    Buffer(Buffer &&other) noexcept : span<u8>{other}, vkBuffer{std::exchange(other.vkBuffer, 0)}, storage{std::move(other.storage)} {}
    ~Buffer() { if (vkBuffer) { live.erase(vkBuffer); ++destroyed; lastDestruction = std::this_thread::get_id(); } }
};
}
struct GPU { struct Memory { memory::Buffer AllocateBuffer(size_t size) { return memory::Buffer{size}; } } memory; };
}
}
