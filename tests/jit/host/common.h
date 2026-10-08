// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <algorithm>
#include <array>
#include <cassert>
#include <cerrno>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <fmt/format.h>
#include <common/result.h>
namespace skyline {
    class exception : public std::runtime_error {
      public:
        template<class... Args> exception(std::string_view message,Args &&...args)
            : std::runtime_error(fmt::format(fmt::runtime(message),std::forward<Args>(args)...)) {}
    };
    template<class T> class span : public std::span<T> {
      public:
        using std::span<T>::span;
        span(const std::span<T> &s) : std::span<T>(s) {}
        bool valid() const { return this->data()!=nullptr; }
        bool contains(T *p) const { return p>=this->data() && p<this->data()+this->size(); }
        bool contains(span<T> other) const {
            return other.data()>=this->data() && other.data()-this->data()<=static_cast<std::ptrdiff_t>(this->size()) &&
                other.size()<=this->size()-static_cast<std::size_t>(other.data()-this->data());
        }
        bool operator==(const span &s) const { return this->data()==s.data() && this->size()==s.size(); }
    };
    namespace util {
        template<class T,size_t N> constexpr T MakeMagic(const char (&s)[N]) {T v{};for(size_t i=0;i<N-1;i++) v|=static_cast<T>(s[i])<<(8*i);return v;}
        template<class T> T AlignUp(T v,size_t alignment) {
            if constexpr(std::is_pointer_v<T>) return reinterpret_cast<T>((reinterpret_cast<uintptr_t>(v)+alignment-1)&~(alignment-1));
            else return (v+alignment-1)&~(alignment-1);
        }
        template<class T> T AlignDown(T v,size_t alignment) {
            if constexpr(std::is_pointer_v<T>) return reinterpret_cast<T>(reinterpret_cast<uintptr_t>(v)&~(alignment-1));
            else return v&~(alignment-1);
        }
        template<class T> bool IsAligned(T v,size_t a) {
            if constexpr(std::is_pointer_v<T>) return !(reinterpret_cast<uintptr_t>(v)&(a-1));
            else return !(v&(a-1));
        }
        template<class T> bool IsPageAligned(T v) { return IsAligned(v,constant::PageSize); }
        template<class T> T HexStringToInt(std::string_view s) { return std::stoull(std::string(s),nullptr,16); }
    }
    namespace kernel::type { class KProcess; struct KThread { bool killed{}; }; }
    struct DeviceState { std::shared_ptr<kernel::type::KProcess> process; static thread_local inline std::shared_ptr<kernel::type::KThread> thread; };
}
#define LOGW(...) ((void)0)
#define LOGD(...) ((void)0)
#define LOGE(...) ((void)0)

#define LOGI(...) ((void)0)
