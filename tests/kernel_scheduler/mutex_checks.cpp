// SPDX-License-Identifier: MPL-2.0
// Test-only pthread wrappers catch unmatched unlocks (normal mutexes may silently
// accept them). libc's internal condition-wait unlock/relock pair is balanced.
#include <pthread.h>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
namespace { thread_local std::unordered_map<pthread_mutex_t *, unsigned> held; }
extern "C" int __real_pthread_mutex_lock(pthread_mutex_t *);
extern "C" int __real_pthread_mutex_trylock(pthread_mutex_t *);
extern "C" int __real_pthread_mutex_unlock(pthread_mutex_t *);
extern "C" int __wrap_pthread_mutex_lock(pthread_mutex_t *mutex) {
    int result = __real_pthread_mutex_lock(mutex);
    if (!result) ++held[mutex];
    return result;
}
extern "C" int __wrap_pthread_mutex_trylock(pthread_mutex_t *mutex) {
    int result = __real_pthread_mutex_trylock(mutex);
    if (!result) ++held[mutex];
    return result;
}
extern "C" int __wrap_pthread_mutex_unlock(pthread_mutex_t *mutex) {
    auto it = held.find(mutex);
    if (it == held.end() || !it->second) {
        std::fputs("FAIL unmatched mutex unlock\n", stderr);
        std::abort();
    }
    if (!--it->second) held.erase(it);
    return __real_pthread_mutex_unlock(mutex);
}
