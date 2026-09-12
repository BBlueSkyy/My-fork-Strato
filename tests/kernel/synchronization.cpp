// SPDX-License-Identifier: MPL-2.0
#include <cassert>
#include <atomic>
#include <list>
#include <kernel/priority_inheritance.h>
#include <kernel/address_arbiter.h>

struct Thread {
    std::atomic<int> basePriority, priority;
    std::shared_ptr<Thread> waitThread;
    std::list<std::shared_ptr<Thread>> waiters;
    explicit Thread(int p) : basePriority(p), priority(p) {}
};

int main() {
    using namespace skyline::kernel;
    auto a = std::make_shared<Thread>(10), b = std::make_shared<Thread>(30);
    auto c = std::make_shared<Thread>(40), d = std::make_shared<Thread>(50);
    auto extra = std::make_shared<Thread>(25);
    a->waitThread = b; b->waiters.push_back(a);
    b->waitThread = c; c->waiters.push_back(b);
    c->waitThread = d; d->waiters.push_back(c);
    extra->waitThread = b; b->waiters.push_back(extra);
    auto update = [](const auto &) {};
    RecomputePriorityInheritance(a, update);
    assert(b->priority == 10 && c->priority == 10 && d->priority == 10);
    b->waiters.remove(a); a->waitThread.reset();
    RecomputePriorityInheritance(b, update);
    assert(b->priority == 25 && c->priority == 25 && d->priority == 25);
    b->basePriority = 5;
    RecomputePriorityInheritance(b, update);
    assert(b->priority == 5 && c->priority == 5 && d->priority == 5);
    b->basePriority = 35;
    RecomputePriorityInheritance(b, update);
    assert(d->priority == 25);
    b->waiters.remove(extra); extra->waitThread.reset();
    RecomputePriorityInheritance(b, update);
    assert(b->priority == 35 && c->priority == 35 && d->priority == 35);
    d->waitThread = b; b->waiters.push_back(d);
    RecomputePriorityInheritance(b, update); // A guest cycle must terminate the host traversal.
    d->waitThread.reset(); b->waiters.remove(d);
    assert(AddressIsLessThan(0xffffffffU, 0));
    assert(AddressIsLessThan(0x80000000U, 0x7fffffffU));
    assert(!AddressIsLessThan(0x7fffffffU, 0x80000000U));
    assert(ModifyAddressByWaiterCount(10, 0, 2) == 9);
    assert(ModifyAddressByWaiterCount(10, -1, 2) == 9);
    assert(ModifyAddressByWaiterCount(10, 2, 2) == 9);
    assert(ModifyAddressByWaiterCount(10, 1, 2) == 10);
    assert(ModifyAddressByWaiterCount(10, 3, 2) == 9);
    assert(ModifyAddressByWaiterCount(0xffffffffU, 1, 0) == 0);
    assert(ModifyAddressByWaiterCount(0, 1, 1) == 0xffffffffU);
}
