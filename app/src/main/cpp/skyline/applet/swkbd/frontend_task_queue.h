// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright © 2026 Strato Team and Contributors

#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace skyline::applet::swkbd {
    // JNI class loading can wait on ART locks. Never make a guest SVC wait for it.
    // A single worker also preserves show/update/close ordering across guest threads.
    class FrontendTaskQueue {
      public:
        FrontendTaskQueue() : worker{[this] { Run(); }} {}

        FrontendTaskQueue(const FrontendTaskQueue &) = delete;
        FrontendTaskQueue &operator=(const FrontendTaskQueue &) = delete;

        ~FrontendTaskQueue() { Stop(); }

        bool Post(std::function<void()> task) {
            std::lock_guard lock{mutex};
            if (stopping)
                return false;
            tasks.push_back(std::move(task));
            condition.notify_one();
            return true;
        }

        void Stop() {
            {
                std::lock_guard lock{mutex};
                stopping = true;
            }
            condition.notify_one();
            if (worker.joinable())
                worker.join();
        }

      private:
        void Run() {
            for (;;) {
                std::function<void()> task;
                {
                    std::unique_lock lock{mutex};
                    condition.wait(lock, [this] { return stopping || !tasks.empty(); });
                    if (tasks.empty())
                        return;
                    task = std::move(tasks.front());
                    tasks.pop_front();
                }
                task();
            }
        }

        std::mutex mutex;
        std::condition_variable condition;
        std::deque<std::function<void()>> tasks;
        bool stopping{};
        std::thread worker;
    };
}
