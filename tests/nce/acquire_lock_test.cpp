// SPDX-License-Identifier: MPL-2.0
#include "../../app/src/main/cpp/skyline/nce/acquire_lock_patch.h"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

using namespace skyline::nce;
using Word = std::uint32_t;
static Word ldxr(unsigned rt, unsigned rn, bool x = false) { return 0x885F7C00u | (Word(x) << 30) | (rn << 5) | rt; }
static Word stxr(unsigned rs, unsigned rk, unsigned rn, bool x = false) { return 0x88007C00u | (Word(x) << 30) | (rs << 16) | (rn << 5) | rk; }
static Word cbnz(unsigned rt, unsigned back, bool x = false) { return 0x35000000u | (Word(x) << 31) | ((0x80000u - back) << 5) | rt; }
static std::vector<Word> lock(bool x=false) { return {ldxr(9,23,x),cbnz(9,1,x),stxr(9,8,23,x),cbnz(9,3,x)}; }
static unsigned run(std::vector<Word> &words, std::size_t length = 0) {
    return PatchUnfencedLockAcquires(reinterpret_cast<std::uint8_t *>(words.data()), length ? length : words.size() * 4);
}
int main() {
    for (bool x : {false, true}) {
        auto words=lock(x); words.insert(words.begin(), 0xD503201Fu); words.push_back(0xD503201Fu);
        assert(run(words)==1);
        assert(words[1] == (ldxr(9,23,x)|0x8000u));
        assert(run(words)==0); // idempotent
    }
    {
        auto words=lock(); words[2]=stxr(9,31,23); assert(run(words)==0); // zero-register store
    }
    {
        auto words=lock(); words[0]|=0x8000u; assert(run(words)==0); // already acquire
    }
    {
        auto words=lock(); words[2]=stxr(9,8,22); assert(run(words)==0); // different base
    }
    {
        auto words=lock(); words[3]=cbnz(9,4); assert(run(words)==0); // wrong retry
    }
    {
        auto words=lock(); words[2]|=0x8000u; assert(run(words)==0); // ST LXR exclusion
    }
    for (Word fence : {0xD5033BBFu, 0xD50339BFu}) {
        auto words=lock(); words.push_back(fence); assert(run(words)==0); // fenced
    }
    {
        auto words=lock(); words.push_back(0xD503201Fu); assert(run(words,12)==0); assert(run(words,16)==1);
    }
    {
        auto words=lock(); words[1]=cbnz(8,1); assert(run(words)==0); // wrong test register
    }
    {
        auto words=lock(); words[2]=stxr(23,8,23); assert(run(words)==0); // clobbers base
    }
    for (bool x : {false, true}) {
        auto words=lock(x); words[2]=stxr(9,8,23,!x);
        assert(run(words)==0); // exclusive accesses must have the same size
    }
    {
        auto words=lock(true); words[1]=cbnz(9,1,false);
        assert(run(words)==0); // W test does not test all bits loaded into X9
    }
    {
        auto words=lock(); words[0]=ldxr(31,23); words[1]=cbnz(31,1);
        assert(run(words)==0); // no loaded value is tested
    }
    {
        auto words=lock(); words[2]=stxr(31,8,23); words[3]=cbnz(31,3);
        assert(run(words)==0); // discarded status cannot implement a retry
    }
    {
        auto words=lock(); words[0]=ldxr(23,23); words[1]=cbnz(23,1);
        assert(run(words)==0); // load destroys the address used by the store/retry
    }
    {
        auto words=lock(); words[2]=stxr(10,9,23); words[3]=cbnz(10,3);
        assert(run(words)==0); // stores the tested zero, not a locked value
    }
    {
        auto words=lock(); words.push_back(0xD5033BBFu);
        assert(run(words,19)==1); // partial following instruction must not be read
        assert(words[4]==0xD5033BBFu);
    }
    std::puts("PASS: unfenced exclusive-lock acquire matcher");
}
