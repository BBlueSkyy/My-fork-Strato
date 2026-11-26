#pragma once
#include <vector>
#include <mutex>
#include <functional>
namespace skyline {
namespace submission_test {
// Test-only scheduling seam at the end of dependency traversal. It runs after
// releasing the facade list lock and never changes production FenceCycle code.
inline thread_local std::function<void()> afterIteration;
}
template<class T> class AtomicForwardList {
    std::vector<T> entries;
    std::recursive_mutex mutex;
public:
    template<class... U> void Append(U&&... values) {
        std::scoped_lock lock{mutex};
        (entries.emplace_back(std::forward<U>(values)), ...);
    }
    void Clear() { std::scoped_lock lock{mutex}; entries.clear(); }
    template<class F> void Iterate(F&& fn) {
        { std::scoped_lock lock{mutex}; for(auto &value : entries) fn(value); }
        if (auto hook = std::move(submission_test::afterIteration)) hook();
    }
    template<class F> bool AllOf(F&& fn) { std::scoped_lock lock{mutex}; for(auto &value : entries) if(!fn(value)) return false; return true; }
};
}
