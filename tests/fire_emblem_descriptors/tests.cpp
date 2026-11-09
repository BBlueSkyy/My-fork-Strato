// SPDX-License-Identifier: MPL-2.0
// Exercises the production descriptor write planner on the host.
//
// Fire Emblem: Three Houses crashes the Adreno proprietary driver (0.777.0) inside vkUpdateDescriptorSets on the first graphics
// update whose single UniformTexelBuffer write spans several bindings (binding 1, descriptorCount 3). Updates with one texel
// buffer binding succeed. These tests pin down the write layout the pipeline manager produces for that shape.
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <gpu/interconnect/common/descriptor_write_policy.h>

using namespace skyline::gpu::interconnect;

static int failures{};

#define EXPECT(expr)                                                                \
    do {                                                                            \
        if (!(expr)) {                                                              \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);    \
            failures++;                                                             \
        }                                                                           \
    } while (false)

// Mirrors the shape of Pipeline::DescriptorInfo::StageDescriptorInfo::UniformTexelBufferDesc (u8 count)
struct Desc {
    std::uint8_t count;
};

static std::vector<DescriptorWriteRange> Plan(const std::vector<Desc> &descs, std::uint32_t firstBinding, bool individual, std::uint32_t *returned = nullptr) {
    std::vector<DescriptorWriteRange> ranges;
    auto result{PlanDescriptorWrites(descs, firstBinding, individual, [&](const DescriptorWriteRange &range) { ranges.push_back(range); })};
    if (returned)
        *returned = result;
    return ranges;
}

// The planner is constexpr so that the layout can be checked at compile time as well
constexpr std::uint32_t CountEmitted(bool individual) {
    constexpr std::array<Desc, 3> descs{{{1}, {1}, {1}}};
    std::uint32_t emitted{};
    PlanDescriptorWrites(descs, 1, individual, [&](const DescriptorWriteRange &) { emitted++; });
    return emitted;
}
static_assert(CountEmitted(false) == 1);
static_assert(CountEmitted(true) == 3);
static_assert(DescriptorWriteCount(0, false) == 0 && DescriptorWriteCount(0, true) == 0);
static_assert(DescriptorWriteCount(3, false) == 1 && DescriptorWriteCount(3, true) == 3);

int main() {
    // Fire Emblem: Three Houses graphics layout: binding 0 is a uniform buffer, bindings 1-3 are three single element uniform texel buffers
    {
        const std::vector<Desc> texel{{1}, {1}, {1}};

        // Batched (every other driver): one write spilling over bindings 1-3, exactly what crashed on Adreno
        auto batched{Plan(texel, 1, false)};
        EXPECT(batched.size() == 1);
        EXPECT(batched[0].dstBinding == 1 && batched[0].descriptorCount == 3 && batched[0].firstElement == 0);

        // Individual (Adreno proprietary): one single descriptor write per binding, none spans more than its own binding
        auto individual{Plan(texel, 1, true)};
        EXPECT(individual.size() == 3);
        for (std::uint32_t i{}; i < 3 && i < individual.size(); i++) {
            EXPECT(individual[i].dstBinding == 1 + i);
            EXPECT(individual[i].descriptorCount == 1);
            EXPECT(individual[i].firstElement == i);
        }
    }

    // A single binding is identical in both modes so games that never hit the spill keep the exact same writes
    for (std::uint8_t count : {1, 2, 5}) {
        const std::vector<Desc> single{{count}};
        auto batched{Plan(single, 7, false)};
        auto individual{Plan(single, 7, true)};
        EXPECT(batched.size() == 1 && individual.size() == 1);
        EXPECT(batched[0].dstBinding == 7 && individual[0].dstBinding == 7);
        EXPECT(batched[0].descriptorCount == count && individual[0].descriptorCount == count);
        EXPECT(batched[0].firstElement == 0 && individual[0].firstElement == 0);
    }

    // Arrays: individual writes keep their own descriptorCount, the descriptor values must stay in the same flat order
    {
        const std::vector<Desc> arrays{{2}, {1}, {3}};
        auto individual{Plan(arrays, 4, true)};
        EXPECT(individual.size() == 3);
        EXPECT(individual[0].dstBinding == 4 && individual[0].descriptorCount == 2 && individual[0].firstElement == 0);
        EXPECT(individual[1].dstBinding == 5 && individual[1].descriptorCount == 1 && individual[1].firstElement == 2);
        EXPECT(individual[2].dstBinding == 6 && individual[2].descriptorCount == 3 && individual[2].firstElement == 3);

        auto batched{Plan(arrays, 4, false)};
        EXPECT(batched.size() == 1 && batched[0].dstBinding == 4 && batched[0].descriptorCount == 6 && batched[0].firstElement == 0);
    }

    // Nothing to write is never an empty VkWriteDescriptorSet
    for (bool individual : {false, true}) {
        std::uint32_t returned{99};
        EXPECT(Plan({}, 0, individual, &returned).empty());
        EXPECT(returned == 0);
    }

    // Both modes cover exactly the same descriptors, the number of planned writes always matches the count used to size the write array
    for (std::uint32_t size{}; size <= 6; size++) {
        for (std::uint32_t countSeed{}; countSeed < 3; countSeed++) {
            std::vector<Desc> descs;
            std::uint32_t totalElements{};
            for (std::uint32_t i{}; i < size; i++) {
                descs.push_back({static_cast<std::uint8_t>(1 + ((i + countSeed) % 3))});
                totalElements += descs.back().count;
            }

            for (bool individual : {false, true}) {
                std::uint32_t returned{};
                auto ranges{Plan(descs, 3, individual, &returned)};
                EXPECT(returned == ranges.size());
                EXPECT(ranges.size() == DescriptorWriteCount(descs.size(), individual));

                std::uint32_t covered{};
                for (const auto &range : ranges) {
                    EXPECT(range.firstElement == covered); // Contiguous, in order, no gaps or overlaps
                    EXPECT(range.descriptorCount > 0);
                    covered += range.descriptorCount;
                }
                EXPECT(covered == totalElements);
            }
        }
    }

    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return EXIT_FAILURE;
    }

    std::puts("Descriptor write planner tests: PASS");
    return EXIT_SUCCESS;
}
