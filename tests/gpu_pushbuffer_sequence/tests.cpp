// SPDX-License-Identifier: MPL-2.0
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <span>
#include <stdexcept>
#include <vector>
using u32 = uint32_t;
using u8 = uint8_t;
template<class T> struct span : std::span<T> {
    using std::span<T>::span;
    span(std::vector<T> &v) : std::span<T>{v} {}
    template<class U> span<U> cast() { return {reinterpret_cast<U *>(this->data()), this->size_bytes() / sizeof(U)}; }
};
void Check(bool value, const char *text) { if (!value) throw std::runtime_error(text); }
struct Fixture {
    std::vector<u32> bytes{1, 2, 3, 4}, pushBufferData;
    bool fragmented{}, dirty{}, failFlush{}, skipDirtyFlushes{};
    unsigned submissions{}, reads{};
    struct GpEntry { u32 size{4}; unsigned Address() const { return 0; } } gpEntry;
    struct Executor {
        Fixture &test;
        struct Tracker {
            struct Intervals { Fixture &test; bool Intersect(span<u8>) const { return test.dirty; } } dirtyIntervals;
        } usageTracker{{test}};
        void Submit(std::function<void()>, bool wait) {
            Check(wait, "existing blocking flush semantics changed");
            if (test.failFlush) throw std::runtime_error("injected producer failure");
            ++test.submissions; test.bytes = {42, 43, 44, 45}; test.dirty = false;
        }
    } executor{*this};
    struct Gmmu {
        Fixture &test;
        std::vector<span<u8>> TranslateRange(unsigned, size_t) {
            auto all{span<u32>{test.bytes}.cast<u8>()};
            if (test.fragmented) return {{all.data(), 8}, {all.data() + 8, 8}};
            return {all};
        }
        template<class T> void Read(std::vector<T> &out, unsigned) {
            ++test.reads; std::memcpy(out.data(), test.bytes.data(), out.size() * sizeof(T));
        }
    } gmmu{*this};
    struct As { Gmmu &gmmu; } addressSpace{gmmu};
    struct Channel { Executor &executor; As *asCtx; } channelCtx{executor, &addressSpace};
    void Process() {
#include "production.inc"
        if (skipDirtyFlushes) {
            Check(pushbufferDirty == dirty, "deferred macro dirty flag changed");
        } else {
            Check(!dirty && pushBuffer[0] == (submissions ? 42U : 1U), "fragmented pushbuffer consumed bytes copied before its producer flush");
        }
        Check(pushBufferCopied == fragmented, "contiguous zero-copy behavior changed");
    }
};
int main() {
    try {
        for (bool fragmented : {false, true}) for (bool skip : {false, true}) {
            Fixture test; test.fragmented = fragmented; test.skipDirtyFlushes = skip; test.dirty = true;
            test.Process();
            Check(test.submissions == unsigned(!skip), "unexpected number of flushes");
            Check(test.reads == unsigned(fragmented), "unexpected materialization count");
        }
        for (bool fragmented : {false, true}) {
            Fixture clean; clean.fragmented = fragmented; clean.Process();
            Check(!clean.submissions, "clean pushbuffer caused an unnecessary flush");
        }
        Fixture failure; failure.fragmented = true; failure.dirty = true; failure.failFlush = true;
        bool failed{}; try { failure.Process(); } catch (const std::runtime_error &) { failed = true; }
        Check(failed && !failure.reads, "CPU read occurred before failed producer synchronization");
        failure.failFlush = false; failure.Process();
        std::puts("PASS: production pushbuffer producer-before-read order, fragmented/contiguous, deferred macros and failed flush retry");
    } catch (const std::exception &e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
