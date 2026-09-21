// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <algorithm>
#include <memory>
#include <vector>

namespace skyline::kernel {
    // Caller holds the process wait-graph lock. Guest cycles must not spin the host forever.
    template<typename Thread, typename Update>
    void RecomputePriorityInheritance(std::shared_ptr<Thread> current, Update update) {
        std::vector<Thread *> visited;
        while (current && std::find(visited.begin(), visited.end(), current.get()) == visited.end()) {
            visited.push_back(current.get());
            current->waiters.sort([](const auto &a, const auto &b) { return a->priority < b->priority; });
            auto inherited{current->basePriority.load()};
            for (const auto &waiter : current->waiters)
                inherited = std::min(inherited, waiter->priority.load());
            if (current->priority.exchange(inherited) != inherited)
                update(current);
            current = current->waitThread;
        }
    }
}
