// SPDX-License-Identifier: MPL-2.0
#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <bitset>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <csignal>
#include <cerrno>
#include <future>
#include <functional>
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
// Test-only rendezvous at the native queue-lock boundary. Normal lock
// ownership and the extracted condition-variable implementation are retained.
struct SpinLock : std::mutex {
    std::function<void()> beforeLock;
    void lock() { if (beforeLock) beforeLock(); std::mutex::lock(); }
};
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
    std::atomic<unsigned> idleMigrationRequested{};
    int signalResult{};
    std::mutex statusMutex;
    pthread_t pthread{};
    bool running{true}, ready{true}, killed{};
    int SignalSend(pthread_t, int) { ++signals; return signalResult; }
    bool TrySendSignal(int signal);
    void SendSignal(int) { ++signals; }
    void ArmPreemptionTimer(auto) { isPreempted = true; ++armed; }
    void DisarmPreemptionTimer() { isPreempted = false; ++disarmed; }
    static bool IsHigherPriority(i8 p, const std::shared_ptr<KThread> &t) { return p < t->priority; }
};
}
namespace skyline::kernel {
inline thread_local type::KThread *this_thread{};
Scheduler::CoreContext::CoreContext(u8 id, i8 priority) : id(id), preemptionPriority(priority) {}
Scheduler::Scheduler(const DeviceState &state) : state(state) {
}
}
#include "methods.inc"
#ifdef HAS_TRY_SIGNAL
#include "try_signal.inc"
#include "host_signal.inc"
#else
bool skyline::kernel::type::KThread::TrySendSignal(int signal) { return SignalSend(pthread, signal) == 0; }
#endif
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
void idleFrontWithoutSvc() {
    Fixture f;
    auto main = f.add(1, 35, 0, 3);
    auto worker = f.add(32, 43, 0, 1);
    std::atomic_bool completed{};
    auto waiting = std::async(std::launch::async, [&] {
        DeviceState::thread = worker;
        f.state.scheduler->WaitSchedule();
        completed = true;
        f.state.scheduler->RemoveThread();
    });
    DeviceState::thread = main;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
    // Guest busy work: no SleepThread SVC. The signal boundary is the only
    // facade; production Rotate/WaitSchedule must perform any migration.
    while (!main->signals && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    const bool requested = main->signals.load() != 0;
    if (requested) {
        f.state.scheduler->Rotate();
        Scheduler::YieldPending = false;
        f.state.scheduler->WaitSchedule();
    }
    const bool migrated = main->coreId == 1 && f.front(1) == main;
    f.state.scheduler->RemoveThread(); // Also release the baseline waiter.
    waiting.get();
    check(requested && migrated && completed,
          "busy guest front strands pinned work despite an allowed idle core");
}
#ifdef HAS_TRY_SIGNAL
void idleMigrationBounds() {
    for (unsigned scenario{}; scenario < 5; ++scenario) {
        Fixture f;
        auto main = f.add(1, 35, 0, scenario == 0 ? 1 : 3);
        auto waiter = f.add(32, 43, 0, 1);
        if (scenario == 1) f.add(2, 30, 1, 2);
        if (scenario == 2) {
            DeviceState::thread = waiter;
            f.state.scheduler->RemoveThread(); // No queued demand remains.
        }
        if (scenario == 3) main->ready = false;
        if (scenario == 4) main->isPaused = true;
        DeviceState::thread = waiter;
        f.state.scheduler->RequestIdleCoreMigration();
        check(!main->signals && !main->idleMigrationRequested && !main->pendingYield,
              "idle request ignored affinity, occupancy, membership or host state");
    }
}
void idleMigrationRevalidation() {
    for (unsigned scenario{}; scenario < 3; ++scenario) {
        Fixture f;
        auto main = f.add(1, 35, 0, 3);
        auto waiter = f.add(32, 43, 0, 1);
        DeviceState::thread = waiter;
        f.state.scheduler->RequestIdleCoreMigration();
        check(main->signals == 1 && main->idleMigrationRequested, "idle request missing");
        if (scenario == 0) f.add(2, 30, 1, 2); // Target fills before delivery.
        if (scenario == 1) main->affinityMask = 1;
        if (scenario == 2) f.state.scheduler->RemoveThread();
        DeviceState::thread = main;
        f.state.scheduler->Rotate();
        Scheduler::YieldPending = false;
        f.state.scheduler->WaitSchedule();
        check(main->coreId == 0 && f.front(0) == main && !main->idleMigrationRequested,
              "idle migration used stale capacity, affinity or demand");
    }
}
void idleMigrationSignalOwnership() {
    Fixture f;
    auto main = f.add(1, 35, 0, 3);
    auto waiter = f.add(32, 43, 0, 1);
    DeviceState::thread = waiter;
    main->signalResult = ESRCH;
    f.state.scheduler->RequestIdleCoreMigration();
    check(main->signals == 1 && !main->idleMigrationRequested && !main->pendingYield,
          "failed signal poisoned idle request");
    main->signalResult = 0;
    main->pendingYield = true;
    this_thread = main.get();
    // An unrelated yield may have reached HOST before an idle request existed.
    Scheduler::HostSignalHandler(Scheduler::YieldSignal, nullptr, nullptr);
    this_thread = nullptr;
    f.state.scheduler->RequestIdleCoreMigration();
    check(main->signals == 1, "borrowed yield was not reused initially");
    f.state.scheduler->RequestIdleCoreMigration();
    f.state.scheduler->RequestIdleCoreMigration();
    check(main->signals == 2 && main->idleMigrationRequested,
          "already-delivered unrelated yield permanently suppressed idle request");
    DeviceState::thread = main;
    f.state.scheduler->Rotate();
    f.state.scheduler->WaitSchedule();
    check(main->coreId == 1 && !main->idleMigrationRequested,
          "existing signal did not carry idle migration");
}
void idleMigrationContention() {
    for (unsigned scenario{}; scenario < 2; ++scenario) {
        Fixture f;
        auto main = f.add(1, 35, 0, 3);
        auto waiter = f.add(32, 43, 0, 1);
        std::promise<void> held, release;
        auto released = release.get_future();
        auto holder = std::async(std::launch::async, [&] {
            if (scenario == 0) {
                std::unique_lock lock{main->coreMigrationMutex};
                held.set_value(); released.wait();
            } else {
                std::unique_lock lock{main->statusMutex};
                held.set_value(); released.wait();
            }
        });
        held.get_future().wait();
        auto request = std::async(std::launch::async, [&] {
            DeviceState::thread = waiter;
            f.state.scheduler->RequestIdleCoreMigration();
        });
        const bool nonblocking = request.wait_for(std::chrono::milliseconds(150)) == std::future_status::ready;
        release.set_value(); holder.get(); request.get();
        check(nonblocking && !main->signals && !main->idleMigrationRequested,
              "idle request blocked on a foreign lock or retained unsent request");
        DeviceState::thread = waiter;
        f.state.scheduler->RequestIdleCoreMigration();
        check(main->signals == 1 && main->idleMigrationRequested,
              "contention prevented a later idle request");
    }
}
void concurrentIdleMigrationRequests() {
    Fixture f;
    auto main = f.add(1, 35, 0, 3);
    auto a = f.add(32, 43, 0, 1);
    auto b = f.add(39, 48, 0, 1);
    std::barrier start{3};
    auto request = [&](Thread waiter) {
        DeviceState::thread = waiter;
        start.arrive_and_wait();
        f.state.scheduler->RequestIdleCoreMigration();
    };
    auto one = std::async(std::launch::async, request, a);
    auto two = std::async(std::launch::async, request, b);
    start.arrive_and_wait(); one.get(); two.get();
    check(main->signals == 1 && main->idleMigrationRequested,
          "simultaneous waiters sent duplicate idle migration signals");
    DeviceState::thread = main;
    f.state.scheduler->Rotate();
    f.state.scheduler->WaitSchedule();
    check(main->coreId == 1 && f.front(0) == a && f.front(1) == main,
          "simultaneous requests left stale queue membership");
}
void idleMigrationEqualPriority() {
    Fixture f;
    auto main = f.add(1, 35, 0, 3);
    auto equal = f.add(4, 35, 0, 1);
    auto waiter = f.add(32, 43, 0, 1);
    DeviceState::thread = waiter;
    f.state.scheduler->RequestIdleCoreMigration();
    DeviceState::thread = main;
    f.state.scheduler->Rotate();
    f.state.scheduler->WaitSchedule();
    check(main->coreId == 1 && f.front(0) == equal && f.front(1) == main && waiter->coreId == 0,
          "idle migration disturbed equal-priority rotation or pinned waiter");
}
void idleMigrationPauseBeforeDelivery() {
    Fixture f;
    auto main = f.add(1, 35, 0, 3);
    auto waiter = f.add(32, 43, 0, 1);
    DeviceState::thread = waiter;
    f.state.scheduler->RequestIdleCoreMigration();
    {
        std::scoped_lock migrationLock{main->coreMigrationMutex};
        f.state.scheduler->PauseThread(main);
    }
    DeviceState::thread = main;
    f.state.scheduler->MigrateToIdleCore();
    check(main->coreId == 0 && !f.front(1) && f.front(0) == waiter,
          "paused host migrated or re-entered a queue");
    {
        std::scoped_lock migrationLock{main->coreMigrationMutex};
        f.state.scheduler->ResumeThread(main);
    }
    f.state.scheduler->Rotate();
    f.state.scheduler->WaitSchedule();
    check(main->coreId == 1 && f.front(0) == waiter && f.front(1) == main && !main->idleMigrationRequested,
          "resumed host duplicated queue membership or lost pending request");
}
void idleMigrationLateRequest() {
    Fixture f;
    auto main = f.add(1, 35, 0, 3);
    auto waiter = f.add(32, 43, 0, 1);
    std::promise<void> entered, deliver;
    auto delivery = deliver.get_future();
    std::atomic_bool first{true};
    f.state.scheduler->cores[0].mutex.beforeLock = [&] {
        if (DeviceState::thread == main && first.exchange(false)) {
            entered.set_value(); delivery.wait();
        }
    };
    auto recipient = std::async(std::launch::async, [&] {
        DeviceState::thread = main;
        f.state.scheduler->WaitSchedule(false);
    });
    entered.get_future().wait(); // Recipient has passed the entry flag check.
    DeviceState::thread = waiter;
    f.state.scheduler->RequestIdleCoreMigration();
    deliver.set_value(); recipient.get();
    check(main->coreId == 1 && f.front(0) == waiter && f.front(1) == main && !main->idleMigrationRequested && !main->pendingYield,
          "late idle request was stranded after WaitSchedule entry");
}
void idleMigrationHostDeliveryRetry() {
    Fixture f;
    auto main = f.add(1, 35, 0, 3);
    auto waiter = f.add(32, 43, 0, 1);
    DeviceState::thread = waiter;
    f.state.scheduler->RequestIdleCoreMigration();
    check(main->signals == 1, "initial idle signal missing");
    // Actual production host callback: the request was delivered too late
    // for a host-to-guest boundary to consume it. Existing backoff must retry.
    this_thread = main.get();
    Scheduler::HostSignalHandler(Scheduler::YieldSignal, nullptr, nullptr);
    f.state.scheduler->RequestIdleCoreMigration();
    f.state.scheduler->RequestIdleCoreMigration();
    check(main->signals == 2 && main->idleMigrationRequested == Scheduler::IdleMigrationSignalPending,
          "host-delivered idle request was suppressed or duplicated");
    Scheduler::HostSignalHandler(Scheduler::YieldSignal, nullptr, nullptr);
    main->signalResult = ESRCH;
    f.state.scheduler->RequestIdleCoreMigration();
    check(main->idleMigrationRequested == Scheduler::IdleMigrationRetry,
          "failed retry discarded an acknowledged idle request");
    main->signalResult = 0;
    f.state.scheduler->RequestIdleCoreMigration();
    check(main->signals == 4, "failed idle signal could not be retried");
    DeviceState::thread = main;
    f.state.scheduler->Rotate();
    Scheduler::YieldPending = false;
    f.state.scheduler->WaitSchedule();
    this_thread = nullptr;
    check(main->coreId == 1 && !main->idleMigrationRequested,
          "retried host-delivered request did not migrate on its own host");
}
void idleMigrationEntryAcknowledgesYield() {
    Fixture f;
    auto main = f.add(1, 35, 0, 3);
    auto waiter = f.add(32, 43, 0, 1);
    // Prior scheduler handler has already rotated; a new idle request now
    // arrives just before its WaitSchedule entry.
    DeviceState::thread = main;
    f.state.scheduler->Rotate();
    DeviceState::thread = waiter;
    f.state.scheduler->RequestIdleCoreMigration();
    DeviceState::thread = main;
    f.state.scheduler->WaitSchedule(false);
    check(main->coreId == 1 && !main->idleMigrationRequested && !main->pendingYield,
          "entry-consumed idle request left signal deduplication poisoned");
}
#endif
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
// Publish runnable work while a selector is pending on the queue lock. Reading
// empty() without that lock incorrectly returns the pre-publication topology.
void balanceQueuePublication(bool source) {
    Fixture f;
    auto caller = f.add(1, 35, 0, source ? 3 : 7);
    DeviceState::thread = caller;
    f.state.scheduler->RemoveThread();
    if (!source)
        f.add(2, 20, 0, 1);

    auto competitor = std::make_shared<type::KThread>();
    competitor->id = 3; competitor->priority = 20;
    competitor->coreId = competitor->idealCore = source ? 0 : 1;
    competitor->affinityMask = 1U << competitor->coreId;
    auto &publishingCore = f.state.scheduler->cores.at(competitor->coreId);
    std::unique_lock queueLock(publishingCore.mutex);
    std::promise<void> started;
    auto selector = std::async(std::launch::async, [&] {
        DeviceState::thread = caller;
        std::scoped_lock migrationLock(caller->coreMigrationMutex);
        started.set_value();
        return f.state.scheduler->GetOptimalCoreForThread(caller).id;
    });
    started.get_future().get();
    const bool selectedUnpublishedQueue = selector.wait_for(std::chrono::milliseconds(100)) == std::future_status::ready;
    {
        std::scoped_lock migrationLock(competitor->coreMigrationMutex);
        f.state.scheduler->InsertThreadLocked(competitor, publishingCore);
    }
    queueLock.unlock();
    const auto selected = selector.get();
    check(!selectedUnpublishedQueue, "load balancing read an empty queue without its publication lock");
    check(selected == (source ? 1 : 2), "load balancing ignored newly published runnable work");
}
void balanceSourcePublication() { balanceQueuePublication(true); }
void balanceCandidatePublication() { balanceQueuePublication(false); }
void concurrentBalancePublication() {
    Fixture f;
    auto caller = f.add(1, 35, 0, 7);
    f.state.scheduler->RemoveThread();
    std::barrier start{3};
    std::vector<std::future<void>> publishers;
    for (unsigned core : {0U, 1U}) {
        auto worker = std::make_shared<type::KThread>();
        worker->id = core + 2; worker->priority = 20;
        worker->coreId = worker->idealCore = core;
        worker->affinityMask = 1U << core;
        publishers.push_back(std::async(std::launch::async, [&, worker] {
            DeviceState::thread = worker;
            start.arrive_and_wait();
            for (unsigned i = 0; i < 4000; ++i) {
                f.state.scheduler->InsertThread(worker);
                std::this_thread::yield();
                f.state.scheduler->RemoveThread();
            }
        }));
    }
    DeviceState::thread = caller;
    start.arrive_and_wait();
    for (unsigned i = 0; i < 8000; ++i) {
        std::scoped_lock migrationLock(caller->coreMigrationMutex);
        check(f.state.scheduler->GetOptimalCoreForThread(caller).id < 3,
              "concurrent balance escaped allowed cores");
        std::this_thread::yield();
    }
    for (auto &publisher : publishers) publisher.get();
    check(!f.front(0) && !f.front(1), "concurrent balance lost a queue removal");
}
int main(int argc, char **argv) {
    try {
        std::vector<std::pair<std::string, void(*)()>> tests{
            {"migration", migration}, {"idle-front", idleFrontWithoutSvc}, {"zero", withoutMigration}, {"affinity", forbiddenMigration},
            {"occupied", occupiedCore}, {"sole", emptySource}, {"park-alone", parkAlone},
            {"park-handoff", parkHandoff}, {"equal", equalPriority}, {"preemption", preemption},
            {"park-affinity", parkedAffinity}, {"concurrent", multipleThreads},
            {"timed-affinity", timedAffinity}, {"notifications", notifications},
            {"park-mask", parkedMaskChange}, {"park-pause", parkedPauseResume},
            {"contended-wake", contendedWake}, {"migrate-wake", migrationWakesParked},
            {"pause-wake", pauseWakesParked}, {"contended-priority", contendedPriorityWake}, {"delayed-wake", delayedParkWake}, {"equal-parked", equalParkedWake}, {"two-idle", simultaneousIdleWake},
            {"balance-source", balanceSourcePublication}, {"balance-candidate", balanceCandidatePublication},
            {"balance-concurrent", concurrentBalancePublication}};
#ifdef HAS_TRY_SIGNAL
        tests.insert(tests.end(), {{"idle-bounds", idleMigrationBounds}, {"idle-revalidate", idleMigrationRevalidation},
            {"idle-signal", idleMigrationSignalOwnership}, {"idle-contended", idleMigrationContention},
            {"idle-concurrent", concurrentIdleMigrationRequests}, {"idle-equal", idleMigrationEqualPriority},
            {"idle-pause", idleMigrationPauseBeforeDelivery}, {"idle-late", idleMigrationLateRequest},
            {"idle-host-retry", idleMigrationHostDeliveryRetry},
            {"idle-entry-ack", idleMigrationEntryAcknowledgesYield}});
#endif
        for (auto &[name, test] : tests) {
            if (argc > 1 && name != argv[1]) continue;
            test(); std::cout << "PASS " << name << std::endl;
        }
    } catch (const std::exception &e) { std::cerr << "FAIL " << e.what() << std::endl; return 1; }
}
