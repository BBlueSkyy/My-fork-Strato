// SPDX-License-Identifier: MPL-2.0
#include <algorithm>
#include <array>
#include <atomic>
#include <bitset>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <csignal>
#include <future>
#include <iostream>
#include <list>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>
#define LOGD(...) ((void)0)
#define LOGE(...) ((void)0)
#define LOGW(...) ((void)0)
#define TRACE_EVENT(...) ((void)0)
namespace skyline {
using u8 = uint8_t; using i8 = int8_t; using u64 = uint64_t; using i64 = int64_t;
namespace util {
u64 GetTimeTicks() { return std::chrono::steady_clock::now().time_since_epoch().count(); }
i64 GetTimeNs() { return GetTimeTicks(); }
}
template<class... T> std::runtime_error exception(const char *s, T...) { return std::runtime_error(s); }
using SpinLock = std::mutex;
struct siginfo; struct ucontext;
#include "condition.inc"
void AdaptiveSingleWaiterConditionVariable::SpinWait() { std::this_thread::yield(); }
void AdaptiveSingleWaiterConditionVariable::SpinWait(i64) { std::this_thread::yield(); }
namespace kernel { class Scheduler; namespace type { struct KThread; } }
struct DeviceState {
    static inline thread_local std::shared_ptr<kernel::type::KThread> thread;
    std::shared_ptr<kernel::Scheduler> scheduler;
};
}
#include "scheduler.inc"
namespace skyline::kernel::type {
struct KThread {
    size_t id{};
    AdaptiveSingleWaiterConditionVariable scheduleCondition;
    std::atomic<i8> priority{35};
    std::recursive_mutex coreMigrationMutex;
    u8 idealCore{}, coreId{};
    CoreMask affinityMask{1}, parkedWakeCores{};
    u64 timesliceStart{}, averageTimeslice{};
    bool isPreempted{}, pendingYield{}, forceYield{}, isPaused{}, insertThreadOnResume{}, parkedRecheck{};
    std::atomic<unsigned> signals{}, armed{}, disarmed{};
    void SendSignal(int) { ++signals; }
    void ArmPreemptionTimer(auto) { isPreempted = true; ++armed; }
    void DisarmPreemptionTimer() { isPreempted = false; ++disarmed; }
    static bool IsHigherPriority(i8 p, const std::shared_ptr<KThread> &t) { return p < t->priority; }
};
}
namespace skyline::kernel {
Scheduler::CoreContext::CoreContext(u8 id, i8 priority) : id(id), preemptionPriority(priority) {}
Scheduler::Scheduler(const DeviceState &state) : state(state) {}
}
#include "methods.inc"
namespace skyline::kernel::svc {
struct SvcContext { i64 x0; };
i64 ReadSvc64(const DeviceState &, SvcContext &ctx, int, int, int) { return ctx.x0; }
}
#include "svc.inc"
using namespace skyline;
using namespace skyline::kernel;
using Thread = std::shared_ptr<type::KThread>;
struct Fixture {
    DeviceState state;
    Fixture() { state.scheduler = std::make_shared<Scheduler>(state); Scheduler::YieldPending = false; }
    Thread add(size_t id, i8 priority, u8 core, unsigned mask) {
        auto t = std::make_shared<type::KThread>();
        t->id = id; t->priority = priority; t->coreId = t->idealCore = core; t->affinityMask = mask;
        DeviceState::thread = t;
        state.scheduler->InsertThread(t);
        return t;
    }
    void yield(Thread t, i64 mode) {
        DeviceState::thread = t;
        svc::SvcContext ctx{mode};
        svc::SleepThread(state, ctx);
    }
    Thread front(unsigned core) {
        auto &c = state.scheduler->cores.at(core);
        std::scoped_lock lock(c.mutex);
        return c.queue.empty() ? nullptr : c.queue.front();
    }
};
void check(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}
void migration() {
    Fixture f;
    auto main = f.add(1, 35, 0, 3);
    auto audio = f.add(32, 43, 0, 1);
    for (int i = 0; i < 4096; ++i) f.yield(main, -1);
    check(main->coreId == 1 && f.front(0) == audio, "migration yield starves C0 audio despite idle C1");
    DeviceState::thread = audio;
    f.state.scheduler->WaitSchedule(false);
    check(audio->coreId == 0, "pinned audio migrated");
}
void withoutMigration() {
    Fixture f;
    auto main = f.add(1, 35, 0, 3);
    auto audio = f.add(32, 43, 0, 1);
    for (int i = 0; i < 4096; ++i) f.yield(main, 0);
    check(main->coreId == 0 && f.front(0) == main && audio->coreId == 0, "zero yield violates priority or migrates");
}
void forbiddenMigration() {
    Fixture f;
    auto main = f.add(1, 35, 0, 1);
    f.add(32, 43, 0, 1);
    f.yield(main, -1);
    check(main->coreId == 0 && f.front(0) == main, "migration escaped affinity");
}
void occupiedCore() {
    Fixture f;
    auto main = f.add(1, 35, 0, 3);
    f.add(32, 43, 0, 1);
    auto busy = f.add(2, 30, 1, 2);
    f.yield(main, -1);
    check(f.front(1) == busy && main->coreId == 0, "yield displaced running higher priority thread");
}
void emptySource() {
    Fixture f;
    auto main = f.add(1, 35, 0, 3);
    f.yield(main, -1);
    check(main->coreId == 0, "unproductive migration of sole runnable thread");
}
void parkAlone() {
    Fixture f;
    auto t = f.add(1, 35, 0, 1);
    f.yield(t, -2);
    check(t->coreId == 0 && f.front(0) == t, "sole -2 thread failed to resume on its allowed core");
}
void parkHandoff() {
    Fixture f;
    auto main = f.add(1, 35, 0, 1);
    auto audio = f.add(32, 43, 0, 1);
    std::promise<void> started;
    auto task = std::async(std::launch::async, [&] {
        started.set_value();
        f.yield(main, -2);
        return main->coreId;
    });
    started.get_future().wait();
    // WaitSchedule exercises the real notification/predicate handoff.
    DeviceState::thread = audio;
    f.state.scheduler->WaitSchedule(false);
    check(f.front(0) == audio, "-2 did not hand off to lower priority thread");
    f.state.scheduler->RemoveThread();
    check(task.wait_for(std::chrono::seconds(2)) == std::future_status::ready, "parked thread never woke on idle core");
    check(task.get() == 0 && f.state.scheduler->parkedQueue.empty(), "stale parked entry or wrong core");
}
void equalPriority() {
    Fixture f;
    auto a = f.add(1, 35, 0, 1);
    auto b = f.add(2, 35, 0, 1);
    auto task = std::async(std::launch::async, [&] {
        f.yield(a, 0);
    });
    DeviceState::thread = b;
    f.state.scheduler->WaitSchedule(false);
    f.state.scheduler->RemoveThread();
    task.get();
    check(f.front(0) == a && a->coreId == 0, "equal-priority yield failed");
}
void preemption() {
    Fixture f;
    auto low = f.add(1, 59, 0, 1);
    f.state.scheduler->WaitSchedule(false);
    check(low->armed == 1, "preemption timer not armed");
    auto high = f.add(2, 35, 0, 1);
    check(low->forceYield && low->pendingYield && low->signals == 1, "higher priority insertion lost preemption");
    DeviceState::thread = low;
    f.state.scheduler->Rotate();
    check(!low->pendingYield && !low->forceYield && low->disarmed == 1, "yield flags/timer not consumed");
    DeviceState::thread = high;
    f.state.scheduler->RemoveThread();
    DeviceState::thread = low;
    f.state.scheduler->WaitSchedule(false);
    check(low->armed == 2, "timer not rearmed after preemption");
}
void parkedAffinity() {
    Fixture f;
    auto wrong = f.add(1, 20, 1, 2);
    auto worker = f.add(2, 30, 0, 1);
    auto allowed = f.add(3, 30, 0, 1);
    // Seed genuine unbound runnable entries, as the parking phase does. C0 must
    // skip the higher-priority C1-only entry and choose an eligible candidate.
    DeviceState::thread = wrong; f.state.scheduler->RemoveThread();
    DeviceState::thread = allowed; f.state.scheduler->RemoveThread();
    wrong->coreId = allowed->coreId = constant::ParkedCoreId;
    f.state.scheduler->parkedQueue = {wrong, allowed};
    DeviceState::thread = worker;
    f.state.scheduler->WakeParkedThread();
    check(!wrong->parkedRecheck && allowed->parkedRecheck && allowed->parkedWakeCores.test(0), "park wake ignored affinity or eligible candidate");
    check(f.state.scheduler->parkedQueue.size() == 2, "wake removed entry before actual handoff");
}
void multipleThreads() {
    Fixture f;
    auto main = f.add(1, 35, 0, 3);
    std::vector<Thread> workers;
    for (int i = 0; i < 8; ++i) workers.push_back(f.add(32 + i, 43, 0, 1));
    std::atomic<unsigned> progress{};
    std::vector<std::future<void>> tasks;
    for (auto worker : workers) tasks.push_back(std::async(std::launch::async, [&, worker] {
        DeviceState::thread = worker;
        f.state.scheduler->WaitSchedule(false);
        for (int i = 0; i < 64; ++i) {
            ++progress;
            f.yield(worker, 0);
        }
        f.state.scheduler->RemoveThread();
    }));
    f.yield(main, -1);
    for (auto &task : tasks) task.get();
    check(progress == 512 && !f.front(0), "concurrent workers failed to progress");
}
void timedAffinity() {
    Fixture f;
    auto main = f.add(1, 35, 0, 3);
    {
        std::scoped_lock lock(main->coreMigrationMutex);
        main->idealCore = 1;
        main->affinityMask = 2;
    }
    DeviceState::thread = main;
    check(f.state.scheduler->TimedWaitSchedule(std::chrono::milliseconds(10)), "timed affinity migration failed");
    check(main->coreId == 1 && !f.front(0) && f.front(1) == main, "timed migration left stale queue");
}
void notifications() {
    AdaptiveSingleWaiterConditionVariable cv;
    std::mutex mutex;
    std::condition_variable ack;
    unsigned produced{}, consumed{};
    auto waiter = std::async(std::launch::async, [&] {
        std::unique_lock lock(mutex);
        for (unsigned i = 1; i <= 1000; ++i) {
            cv.wait(lock, [&] { return produced == i; });
            consumed = i;
            ack.notify_one();
        }
    });
    std::unique_lock lock(mutex);
    for (unsigned i = 1; i <= 1000; ++i) {
        produced = i;
        cv.notify();
        ack.wait(lock, [&] { return consumed == i; });
    }
    lock.unlock(); waiter.get();
}
void awaitParked(Fixture &f, Thread t) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    for (;;) {
        {
            std::scoped_lock lock(f.state.scheduler->parkedMutex);
            auto &queue = f.state.scheduler->parkedQueue;
            if (std::find(queue.begin(), queue.end(), t) != queue.end()) return;
        }
        if (std::chrono::steady_clock::now() > deadline) std::abort();
        std::this_thread::yield(); // Test rendezvous only; production has no polling.
    }
}
void parkedMaskChange() {
    Fixture f;
    auto main = f.add(1, 35, 0, 1);
    f.add(32, 43, 0, 1);
    auto task = std::async(std::launch::async, [&] { f.yield(main, -2); return main->coreId; });
    awaitParked(f, main);
    {
        std::scoped_lock lock(main->coreMigrationMutex);
        main->affinityMask = 3;
        f.state.scheduler->UpdateCore(main);
    }
    check(task.get() == 1, "parked affinity expansion ignored idle C1");
}
void parkedPauseResume() {
    Fixture f;
    auto main = f.add(1, 35, 0, 1);
    auto audio = f.add(32, 43, 0, 1);
    auto task = std::async(std::launch::async, [&] { f.yield(main, -2); });
    awaitParked(f, main);
    {
        std::scoped_lock lock(main->coreMigrationMutex);
        f.state.scheduler->PauseThread(main);
    }
    DeviceState::thread = audio;
    f.state.scheduler->WaitSchedule(false);
    f.state.scheduler->RemoveThread();
    check(task.wait_for(std::chrono::seconds(0)) != std::future_status::ready, "paused parked thread ran");
    {
        std::scoped_lock lock(main->coreMigrationMutex);
        f.state.scheduler->ResumeThread(main);
    }
    task.get();
    check(main->coreId == 0 && f.front(0) == main, "resume failed to reconsider idle core");
}
void contendedWake() {
    Fixture f;
    auto main = f.add(1, 35, 0, 1);
    auto audio = f.add(32, 43, 0, 1);
    auto task = std::async(std::launch::async, [&] { f.yield(main, -2); });
    awaitParked(f, main);
    std::unique_lock lock(main->coreMigrationMutex);
    auto removing = std::async(std::launch::async, [&] {
        DeviceState::thread = audio;
        f.state.scheduler->RemoveThread();
    });
    check(removing.wait_for(std::chrono::seconds(2)) == std::future_status::ready,
          "idle wake blocked on foreign migration lock");
    removing.get(); lock.unlock(); task.get();
    check(main->coreId == 0, "contended idle wake lost retry");
}
void migrationWakesParked() {
    Fixture f;
    auto parked = f.add(1, 20, 1, 2);
    auto active = f.add(2, 35, 1, 3);
    auto task = std::async(std::launch::async, [&] { f.yield(parked, -2); });
    awaitParked(f, parked);
    {
        std::scoped_lock lock(active->coreMigrationMutex);
        active->idealCore = 0; active->affinityMask = 1;
    }
    DeviceState::thread = active;
    check(f.state.scheduler->TimedWaitSchedule(std::chrono::milliseconds(10)), "active thread did not migrate");
    task.get();
    check(parked->coreId == 1 && f.front(1) == parked, "affinity migration stranded parked thread");
}
void pauseWakesParked() {
    Fixture f;
    auto parked = f.add(1, 20, 1, 2);
    auto active = f.add(2, 35, 1, 2);
    auto task = std::async(std::launch::async, [&] { f.yield(parked, -2); });
    awaitParked(f, parked);
    {
        std::scoped_lock lock(active->coreMigrationMutex);
        f.state.scheduler->PauseThread(active);
    }
    task.get();
    check(parked->coreId == 1 && f.front(1) == parked, "pause stranded parked thread");
}
void contendedPriorityWake() {
    Fixture f;
    auto main = f.add(1, 35, 0, 1);
    auto audio = f.add(32, 43, 0, 1);
    auto task = std::async(std::launch::async, [&] { f.yield(main, -2); });
    awaitParked(f, main);
    std::unique_lock lock(main->coreMigrationMutex);
    auto yielding = std::async(std::launch::async, [&] { f.yield(audio, -1); });
    check(yielding.wait_for(std::chrono::seconds(2)) == std::future_status::ready,
          "priority wake blocked on foreign migration lock");
    yielding.get(); lock.unlock();
    bool ready = task.wait_for(std::chrono::milliseconds(100)) == std::future_status::ready;
    if (!ready) {
        DeviceState::thread = audio;
        f.state.scheduler->RemoveThread(); // Unblock the failing-test cleanup.
    }
    task.get();
    check(ready, "contended non-idle wake stranded higher-priority parked thread");
}
void delayedParkWake() {
    Fixture f;
    auto main = f.add(1, 35, 0, 3);
    auto r = f.add(32, 43, 0, 1);
    auto s = f.add(33, 43, 1, 2);
    auto task = std::async(std::launch::async, [&] { f.yield(main, -2); });
    awaitParked(f, main);
    std::unique_lock lock(main->coreMigrationMutex);
    DeviceState::thread = r; f.state.scheduler->RemoveThread();
    DeviceState::thread = s; f.state.scheduler->RemoveThread();
    auto high = f.add(2, 20, 0, 1);
    lock.unlock();
    bool ready = task.wait_for(std::chrono::milliseconds(100)) == std::future_status::ready;
    if (!ready) {
        DeviceState::thread = high; f.state.scheduler->RemoveThread();
    }
    task.get();
    check(ready && main->coreId == 1, "delayed wake missed idle alternative core");
}
void equalParkedWake() {
    Fixture f;
    auto main = f.add(1, 35, 0, 1);
    auto worker = f.add(2, 35, 0, 1);
    auto task = std::async(std::launch::async, [&] {
        f.yield(main, -2);
        f.state.scheduler->RemoveThread();
    });
    awaitParked(f, main);
    auto yielding = std::async(std::launch::async, [&] { f.yield(worker, -1); });
    bool ready = task.wait_for(std::chrono::milliseconds(100)) == std::future_status::ready;
    if (!ready) {
        DeviceState::thread = worker; f.state.scheduler->RemoveThread();
    }
    task.get(); yielding.get();
    check(ready, "equal-priority parked suggestion missed yield rotation");
}
void simultaneousIdleWake() {
    Fixture f;
    auto p = f.add(1, 20, 0, 3);
    auto r = f.add(2, 43, 0, 1);
    auto s = f.add(3, 43, 1, 2);
    auto pt = std::async(std::launch::async, [&] { f.yield(p, -2); });
    awaitParked(f, p);
    auto q = f.add(4, 35, 0, 3);
    auto qt = std::async(std::launch::async, [&] { f.yield(q, -2); });
    awaitParked(f, q);
    std::unique_lock lock(p->coreMigrationMutex);
    DeviceState::thread = r; f.state.scheduler->RemoveThread();
    DeviceState::thread = s; f.state.scheduler->RemoveThread();
    lock.unlock();
    pt.get(); qt.get();
    check(p->coreId != q->coreId && f.state.scheduler->parkedQueue.empty(), "duplicate idle requests stranded parked thread");
}
int main(int argc, char **argv) {
    try {
        std::vector<std::pair<std::string, void(*)()>> tests{
            {"migration", migration}, {"zero", withoutMigration}, {"affinity", forbiddenMigration},
            {"occupied", occupiedCore}, {"sole", emptySource}, {"park-alone", parkAlone},
            {"park-handoff", parkHandoff}, {"equal", equalPriority}, {"preemption", preemption},
            {"park-affinity", parkedAffinity}, {"concurrent", multipleThreads},
            {"timed-affinity", timedAffinity}, {"notifications", notifications},
            {"park-mask", parkedMaskChange}, {"park-pause", parkedPauseResume},
            {"contended-wake", contendedWake}, {"migrate-wake", migrationWakesParked},
            {"pause-wake", pauseWakesParked}, {"contended-priority", contendedPriorityWake}, {"delayed-wake", delayedParkWake}, {"equal-parked", equalParkedWake}, {"two-idle", simultaneousIdleWake}};
        for (auto &[name, test] : tests) {
            if (argc > 1 && name != argv[1]) continue;
            test(); std::cout << "PASS " << name << std::endl;
        }
    } catch (const std::exception &e) { std::cerr << "FAIL " << e.what() << std::endl; return 1; }
}
