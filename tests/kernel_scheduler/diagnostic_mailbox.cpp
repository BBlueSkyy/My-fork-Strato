// SPDX-License-Identifier: MPL-2.0
#include "kernel/scheduler_diagnostics.h"
#include <atomic>
#include <cerrno>
#include <csignal>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <pthread.h>

using skyline::kernel::diagnostic::ContextMailbox;
using skyline::kernel::diagnostic::ContextSample;
static void check(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
static ContextSample value(uint64_t n) {
    ContextSample sample{};
    sample.pc = n; sample.sp = n + 1; sample.guest = true;
    for (size_t i{}; i < sample.regs.size(); ++i) sample.regs[i] = n + i + 2;
    return sample;
}
static void ownership() {
    ContextMailbox box; ContextSample got{};
    check(!box.Publish(value(1)) && !box.Consume(got), "unsolicited signal published a sample");
    check(box.Request() && !box.Request(), "overlapping requests accepted");
    check(box.Publish(value(42)) && !box.Publish(value(99)), "capture overwrote an outstanding response");
    check(!box.CancelUnsent(), "cancel discarded a captured response");
    check(box.Consume(got) && got.pc == 42 && got.sp == 43 && got.regs[30] == 74 && got.guest,
          "captured register response torn or missing");
    for (size_t i{}; i < got.regs.size(); ++i)
        check(got.regs[i] == 44 + i, "register omitted from published response");
    check(!box.Consume(got) && box.Request(), "response was duplicated or mailbox failed to reset");
    check(box.CancelUnsent() && !box.Publish(value(3)), "definitely unsent request did not cancel");
}
static void delayedDelivery() {
    ContextMailbox box; ContextSample got{};
    check(box.Request(), "request failed");
    for (int i{}; i < 100; ++i)
        check(!box.Consume(got) && !box.Request(), "unanswered request recycled before delayed delivery");
    check(box.Publish(value(7)) && box.Consume(got) && got.pc == 7, "delayed signal lost its original response");
}
static void concurrent() {
    ContextMailbox box;
    auto writer = std::async(std::launch::async, [&] {
        for (uint64_t n = 1; n <= 10000; ++n) {
            while (!box.Publish(value(n))) std::this_thread::yield();
        }
    });
    for (uint64_t n = 1; n <= 10000; ++n) {
        check(box.Request(), "consumer failed to reserve idle mailbox");
        ContextSample got{};
        while (!box.Consume(got)) std::this_thread::yield();
        check(got.pc == n && got.sp == n + 1 && got.regs[30] == n + 32, "concurrent register payload torn");
        for (size_t i{}; i < got.regs.size(); ++i)
            check(got.regs[i] == n + i + 2, "concurrent register omitted or torn");
    }
    writer.get();
}
static ContextMailbox *active;
static void signalCapture(int) {
    const int saved = errno;
    active->Publish(value(123));
    errno = saved;
}
static void nativeSignal() {
    ContextMailbox box; active = &box;
    struct sigaction action{}, previous{};
    action.sa_handler = signalCapture; action.sa_flags = SA_RESTART;
    sigemptyset(&action.sa_mask);
    check(sigaction(SIGUSR1, &action, &previous) == 0, "signal installation failed");
    check(box.Request(), "signal request failed");
    errno = EDOM;
    check(pthread_kill(pthread_self(), SIGUSR1) == 0, "signal delivery failed");
    check(errno == EDOM, "signal changed interrupted errno");
    ContextSample got{};
    check(box.Consume(got) && got.pc == 123, "native signal did not publish requested payload");
    sigaction(SIGUSR1, &previous, nullptr); active = nullptr;
}
int main() {
    ownership(); delayedDelivery(); concurrent(); nativeSignal();
    std::cout << "PASS diagnostic mailbox ownership, delayed delivery, 10000 publications and native signal\n";
}
