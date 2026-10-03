#include <array>
#include <cassert>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <span>
#include <skyline/gpu/texture/copy_dependency.h>

using namespace skyline::gpu::texture;

namespace {
    struct Representation {};

    ClassifiedResourceView CopyOnly(std::initializer_list<ResolvedCopySubresource> subresources) {
        return {
            .relation = TextureViewCompatibility::CopyOnly,
            .copyRegion = ResolvedCopyRegion{.subresources = subresources},
        };
    }

    constexpr ResolvedSubresource Subresource(std::uint32_t mip, std::uint32_t layer = 0,
                                               std::uint32_t depthSlice = 0) {
        return {.mip = mip, .layer = layer, .depthSlice = depthSlice};
    }
}

int main() {
    std::array<std::uint8_t, 512> memory{};
    const std::array<std::span<std::uint8_t>, 1> completeMapping{std::span{memory}.subspan(0, 256)};
    const std::array<std::span<std::uint8_t>, 1> partialMapping{std::span{memory}.subspan(224, 64)};
    GuestResourceRanges completeRanges{completeMapping};
    GuestResourceRanges partialRanges{partialMapping};

    const std::array<GuestSubresource, 4> subresources{
        GuestSubresource{.depth = 4, .mip = 0, .layer = 0},
        GuestSubresource{.depth = 2, .mip = 1, .layer = 0},
        GuestSubresource{.depth = 4, .mip = 0, .layer = 1},
        GuestSubresource{.depth = 2, .mip = 1, .layer = 1},
    };
    const TextureResourceLayout layout{.subresources = subresources};

    auto first = std::make_shared<Representation>();
    auto second = std::make_shared<Representation>();
    auto third = std::make_shared<Representation>();
    auto fourth = std::make_shared<Representation>();
    const auto mip0 = Subresource(0);
    const auto mip1 = Subresource(1);
    const auto layer1 = Subresource(0, 1);
    const auto slice1 = Subresource(1, 0, 1);

    CopyDependencyTracker<Representation> dependencies;
    assert(dependencies.RegisterSynchronized(first, completeRanges, layout, second, completeRanges, layout,
        CopyOnly({{mip0, mip0}, {mip1, mip1}, {layer1, layer1}, {slice1, slice1}})));
    assert(dependencies.RelationCount() == 4);
    assert(dependencies.GetState(first, mip0) == CopyRepresentationState::Current);
    assert(dependencies.GetState(second, mip1) == CopyRepresentationState::Current);
    assert(dependencies.GetState(first, layer1) == CopyRepresentationState::Current);
    assert(dependencies.GetState(second, slice1) == CopyRepresentationState::Current);

    // One storage can have another peer for one region without joining unrelated regions.
    assert(dependencies.RegisterSynchronized(first, completeRanges, layout, third, completeRanges, layout,
        CopyOnly({{mip0, mip0}})));
    assert(dependencies.RelationCount() == 5);
    assert(dependencies.GetState(third, mip0) == CopyRepresentationState::Current);
    assert(dependencies.GetState(third, mip1) == CopyRepresentationState::Untracked);

    // Repeating an exact relationship is idempotent.
    assert(dependencies.RegisterSynchronized(first, completeRanges, layout, third, completeRanges, layout,
        CopyOnly({{mip0, mip0}})));
    assert(dependencies.RelationCount() == 5);

    // A later relation cannot remap an existing endpoint within the same representation pair.
    assert(!dependencies.RegisterSynchronized(first, completeRanges, layout, second, completeRanges, layout,
        CopyOnly({{mip0, mip1}})));
    assert(dependencies.RelationCount() == 5);

    // Writes affect only the exact endpoint component and make the latest writer authoritative.
    const std::array mip0Write{mip0};
    assert(dependencies.MarkWritten(first, mip0Write));
    assert(dependencies.GetState(first, mip0) == CopyRepresentationState::Current);
    assert(dependencies.GetState(second, mip0) == CopyRepresentationState::Stale);
    assert(dependencies.GetState(third, mip0) == CopyRepresentationState::Stale);
    assert(dependencies.GetState(first, mip1) == CopyRepresentationState::Current);
    assert(dependencies.GetState(second, layer1) == CopyRepresentationState::Current);

    assert(dependencies.MarkWritten(second, mip0Write));
    assert(dependencies.GetState(second, mip0) == CopyRepresentationState::Current);
    assert(dependencies.GetState(first, mip0) == CopyRepresentationState::Stale);
    assert(dependencies.GetState(third, mip0) == CopyRepresentationState::Stale);

    const std::array layerWrite{layer1};
    assert(dependencies.MarkWritten(first, layerWrite));
    assert(dependencies.GetState(first, layer1) == CopyRepresentationState::Current);
    assert(dependencies.GetState(second, layer1) == CopyRepresentationState::Stale);
    assert(dependencies.GetState(first, mip1) == CopyRepresentationState::Current);

    const std::array sliceWrite{slice1};
    assert(dependencies.MarkWritten(first, sliceWrite));
    assert(dependencies.GetState(first, slice1) == CopyRepresentationState::Current);
    assert(dependencies.GetState(second, slice1) == CopyRepresentationState::Stale);
    assert(dependencies.GetState(first, mip1) == CopyRepresentationState::Current);
    assert(dependencies.GetState(second, mip1) == CopyRepresentationState::Current);

    // The whole batch is validated and deduplicated before one generation is assigned.
    const std::array duplicateBatch{mip1, mip1, layer1};
    assert(dependencies.MarkWritten(second, duplicateBatch));
    assert(dependencies.GetState(second, mip1) == CopyRepresentationState::Current);
    assert(dependencies.GetState(first, mip1) == CopyRepresentationState::Stale);
    assert(dependencies.GetState(second, layer1) == CopyRepresentationState::Current);
    assert(dependencies.GetState(first, layer1) == CopyRepresentationState::Stale);

    CopyDependencyTracker<Representation> atomicWrite;
    assert(atomicWrite.RegisterSynchronized(first, completeRanges, layout, second, completeRanges, layout,
        CopyOnly({{mip0, mip0}, {mip1, mip1}})));
    assert(atomicWrite.MarkWritten(second, mip0Write));
    const std::array invalidBatch{mip0, Subresource(9)};
    assert(!atomicWrite.MarkWritten(first, invalidBatch));
    assert(atomicWrite.GetState(first, mip0) == CopyRepresentationState::Stale);
    assert(atomicWrite.GetState(second, mip0) == CopyRepresentationState::Current);
    assert(atomicWrite.GetState(first, mip1) == CopyRepresentationState::Current);
    assert(!atomicWrite.MarkWritten(first, std::span<const ResolvedSubresource>{}));

    // Joining two existing components preserves every endpoint on both current frontiers.
    CopyDependencyTracker<Representation> joined;
    assert(joined.RegisterSynchronized(first, completeRanges, layout, second, completeRanges, layout,
        CopyOnly({{mip0, mip0}})));
    assert(joined.RegisterSynchronized(third, completeRanges, layout, fourth, completeRanges, layout,
        CopyOnly({{mip0, mip0}})));
    assert(joined.RegisterSynchronized(second, completeRanges, layout, third, completeRanges, layout,
        CopyOnly({{mip0, mip0}})));
    assert(joined.GetState(first, mip0) == CopyRepresentationState::Current);
    assert(joined.GetState(second, mip0) == CopyRepresentationState::Current);
    assert(joined.GetState(third, mip0) == CopyRepresentationState::Current);
    assert(joined.GetState(fourth, mip0) == CopyRepresentationState::Current);

    // Physical partial overlap and unresolved or Full classifications never create validity.
    CopyDependencyTracker<Representation> rejected;
    const auto onePair = CopyOnly({{mip0, mip0}});
    assert(!rejected.RegisterSynchronized(first, completeRanges, layout, second, partialRanges, layout, onePair));
    auto full = onePair;
    full.relation = TextureViewCompatibility::Full;
    assert(!rejected.RegisterSynchronized(first, completeRanges, layout, second, completeRanges, layout, full));
    const ClassifiedResourceView unresolved{.relation = TextureViewCompatibility::CopyOnly};
    assert(!rejected.RegisterSynchronized(first, completeRanges, layout, second, completeRanges, layout, unresolved));
    assert(rejected.RelationCount() == 0);

    // Duplicate/conflicting mappings and exact layout-bound failures are atomic.
    assert(!rejected.RegisterSynchronized(first, completeRanges, layout, second, completeRanges, layout,
        CopyOnly({{mip0, mip0}, {mip0, mip0}})));
    assert(!rejected.RegisterSynchronized(first, completeRanges, layout, second, completeRanges, layout,
        CopyOnly({{mip0, mip0}, {mip0, mip1}})));
    assert(!rejected.RegisterSynchronized(first, completeRanges, layout, second, completeRanges, layout,
        CopyOnly({{Subresource(2), mip0}})));
    assert(!rejected.RegisterSynchronized(first, completeRanges, layout, second, completeRanges, layout,
        CopyOnly({{Subresource(0, 2), mip0}})));
    assert(!rejected.RegisterSynchronized(first, completeRanges, layout, second, completeRanges, layout,
        CopyOnly({{mip0, Subresource(1, 0, 2)}})));
    assert(!rejected.RegisterSynchronized(first, completeRanges, layout, second, completeRanges, layout,
        CopyOnly({{mip0, mip0}, {mip1, Subresource(3)}})));
    assert(rejected.RelationCount() == 0);
    assert(rejected.GetState(first, mip0) == CopyRepresentationState::Untracked);
    assert(rejected.GetState(second, mip0) == CopyRepresentationState::Untracked);
}
