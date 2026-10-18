#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <limits>
#include <stdexcept>
#include <cstdint>
using u64 = std::uint64_t;
namespace fmt { template<class T> T ptr(T value) { return value; } }
namespace skyline {
struct exception : std::runtime_error {
    template<class... T> exception(const char *message, T&&...) : std::runtime_error(message) {}
};
}
