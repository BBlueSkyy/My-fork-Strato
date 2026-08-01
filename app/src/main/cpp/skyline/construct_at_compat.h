#pragma once
#include <memory>
#include <new>
#include <utility>

#if defined(__ANDROID__) && !defined(__cpp_lib_constexpr_dynamic_alloc)
namespace std {
    template<class T, class... Args>
    constexpr T* construct_at(T *location, Args&&... args) {
        return ::new (const_cast<void*>(static_cast<const volatile void*>(location))) T(std::forward<Args>(args)...);
    }
}
#endif
