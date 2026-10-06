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

    // Read preparation is observational and reports only a direct authoritative neighbor.
    CopyDependencyTracker<Representation> direct;
    assert(direct.RegisterSynchronized(first, completeRanges, layout, second, completeRanges, layout,
        CopyOnly({{mip0, mip0}, {mip1, mip1}})));
    assert(direct.PrepareRead(first, mip0).state == CopyReadState::Current);
    assert(direct.MarkWritten(second, mip0Write));
    const auto firstRead = direct.PrepareRead(first, mip0);
    assert(firstRead.state == CopyReadState::SynchronizationRequired);
    assert(firstRead.source == second);
    assert(firstRead.destination == first);
    assert(firstRead.sourceSubresource == mip0);
    assert(firstRead.destinationSubresource == mip0);
    assert(firstRead.sourceGeneration != 0);
    assert(firstRead.destinationGeneration != 0);
    assert(firstRead.sourceGeneration != firstRead.destinationGeneration);
    assert(direct.GetState(first, mip0) == CopyRepresentationState::Stale);
    assert(direct.PrepareRead(first, mip0).state == CopyReadState::SynchronizationRequired);
    assert(direct.GetState(first, mip0) == CopyRepresentationState::Stale);

    // Completion accepts exactly the prepared generations and changes only the named destination.
    assert(!direct.CompleteSynchronization(PreparedDependencyRead<Representation>{}));
    assert(direct.CompleteSynchronization(firstRead));
    assert(direct.GetState(first, mip0) == CopyRepresentationState::Current);
    assert(direct.GetState(first, mip1) == CopyRepresentationState::Current);
    assert(direct.GetState(second, mip1) == CopyRepresentationState::Current);

    // A later source write invalidates a prepared copy even though the source remains current.
    CopyDependencyTracker<Representation> sourceRace;
    assert(sourceRace.RegisterSynchronized(first, completeRanges, layout, second, completeRanges, layout,
        CopyOnly({{mip0, mip0}})));
    assert(sourceRace.MarkWritten(second, mip0Write));
    const auto sourceRaceRead = sourceRace.PrepareRead(first, mip0);
    assert(sourceRaceRead.state == CopyReadState::SynchronizationRequired);
    assert(sourceRace.MarkWritten(second, mip0Write));
    assert(!sourceRace.CompleteSynchronization(sourceRaceRead));
    assert(sourceRace.GetState(first, mip0) == CopyRepresentationState::Stale);
    assert(sourceRace.GetState(second, mip0) == CopyRepresentationState::Current);

    // A destination write after preparation also invalidates the prepared copy.
    CopyDependencyTracker<Representation> destinationRace;
    assert(destinationRace.RegisterSynchronized(first, completeRanges, layout, second, completeRanges, layout,
        CopyOnly({{mip0, mip0}})));
    assert(destinationRace.MarkWritten(second, mip0Write));
    const auto destinationRaceRead = destinationRace.PrepareRead(first, mip0);
    assert(destinationRaceRead.state == CopyReadState::SynchronizationRequired);
    assert(destinationRace.MarkWritten(first, mip0Write));
    assert(!destinationRace.CompleteSynchronization(destinationRaceRead));
    assert(destinationRace.GetState(first, mip0) == CopyRepresentationState::Current);
    assert(destinationRace.GetState(second, mip0) == CopyRepresentationState::Stale);

    // A transitive current endpoint is unavailable until the stale intermediate is synchronized.
    CopyDependencyTracker<Representation> transitive;
    assert(transitive.RegisterSynchronized(first, completeRanges, layout, second, completeRanges, layout,
        CopyOnly({{mip0, mip0}})));
    assert(transitive.RegisterSynchronized(second, completeRanges, layout, third, completeRanges, layout,
        CopyOnly({{mip0, mip0}})));
    assert(transitive.MarkWritten(first, mip0Write));
    assert(transitive.PrepareRead(third, mip0).state == CopyReadState::Unavailable);
    const auto intermediateRead = transitive.PrepareRead(second, mip0);
    assert(intermediateRead.state == CopyReadState::SynchronizationRequired && intermediateRead.source == first);
    assert(transitive.CompleteSynchronization(intermediateRead));
    const auto thirdRead = transitive.PrepareRead(third, mip0);
    assert(thirdRead.state == CopyReadState::SynchronizationRequired && thirdRead.source == second);
    assert(transitive.CompleteSynchronization(thirdRead));
    assert(transitive.PrepareRead(third, mip0).state == CopyReadState::Current);

    // Expiration of the sole authority never promotes stale data or aliases a new allocation.
    auto surviving = std::make_shared<Representation>();
    auto temporary = std::make_shared<Representation>();
    CopyDependencyTracker<Representation> lifetime;
    assert(lifetime.RegisterSynchronized(surviving, completeRanges, layout, temporary, completeRanges, layout,
        CopyOnly({{mip0, mip0}})));
    assert(lifetime.MarkWritten(temporary, mip0Write));
    temporary.reset();
    assert(lifetime.GetState(surviving, mip0) == CopyRepresentationState::Stale);
    assert(lifetime.PrepareRead(surviving, mip0).state == CopyReadState::Unavailable);
    auto replacement = std::make_shared<Representation>();
    assert(lifetime.GetState(replacement, mip0) == CopyRepresentationState::Untracked);
    assert(lifetime.PrepareRead(replacement, mip0).state == CopyReadState::Untracked);

    // Merge preserves exact graph state, is idempotent, and advances the generation allocator.
    auto importedFirst = std::make_shared<Representation>();
    auto importedSecond = std::make_shared<Representation>();
    CopyDependencyTracker<Representation> imported;
    assert(imported.RegisterSynchronized(importedFirst, completeRanges, layout,
        importedSecond, completeRanges, layout, CopyOnly({{mip0, mip0}})));
    for (std::size_t write{}; write < 4; ++write)
        assert(imported.MarkWritten(importedFirst, mip0Write));

    CopyDependencyTracker<Representation> merged;
    assert(merged.RegisterSynchronized(third, completeRanges, layout,
        fourth, completeRanges, layout, CopyOnly({{mip1, mip1}})));
    merged.MergeFrom(imported);
    assert(merged.RelationCount() == 2);
    assert(merged.GetState(importedFirst, mip0) == CopyRepresentationState::Current);
    assert(merged.GetState(importedSecond, mip0) == CopyRepresentationState::Stale);
    assert(merged.GetState(third, mip1) == CopyRepresentationState::Current);
    merged.MergeFrom(imported);
    assert(merged.RelationCount() == 2);
    assert(merged.GetState(importedFirst, mip0) == CopyRepresentationState::Current);
    assert(merged.GetState(importedSecond, mip0) == CopyRepresentationState::Stale);

    // This write must allocate beyond every imported generation, not tie or trail it.
    assert(merged.MarkWritten(importedSecond, mip0Write));
    assert(merged.GetState(importedSecond, mip0) == CopyRepresentationState::Current);
    assert(merged.GetState(importedFirst, mip0) == CopyRepresentationState::Stale);
    merged.MergeFrom(imported);
    assert(merged.RelationCount() == 2);
    assert(merged.GetState(importedSecond, mip0) == CopyRepresentationState::Current);
    assert(merged.GetState(importedFirst, mip0) == CopyRepresentationState::Stale);

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
