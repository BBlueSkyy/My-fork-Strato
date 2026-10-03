#include <array>
#include <cassert>
#include <cstdint>
#include <memory>
#include <span>
#include <skyline/gpu/texture/copy_dependency.h>

using namespace skyline::gpu::texture;

namespace {
    struct Representation {
        std::shared_ptr<std::uint32_t> backing;
    };
}

int main() {
    std::array<std::uint8_t, 512> memory{};
    const std::array<std::span<std::uint8_t>, 1> parentMapping{std::span{memory}.subspan(0, 256)};
    const std::array<std::span<std::uint8_t>, 1> childMapping{std::span{memory}.subspan(64, 64)};
    const std::array<std::span<std::uint8_t>, 1> partialMapping{std::span{memory}.subspan(224, 64)};
    GuestResourceRanges parentRanges{parentMapping};
    GuestResourceRanges childRanges{childMapping};
    GuestResourceRanges partialRanges{partialMapping};

    auto parent = std::make_shared<Representation>(Representation{std::make_shared<std::uint32_t>(1)});
    auto child = std::make_shared<Representation>(Representation{std::make_shared<std::uint32_t>(2)});
    auto third = std::make_shared<Representation>(Representation{std::make_shared<std::uint32_t>(3)});

    const ClassifiedResourceView copyOnly{
        .relation = TextureViewCompatibility::CopyOnly,
        .copyRegion = ResolvedCopyRegion{
            .backing = {.mip = 2, .layer = 1},
            .requested = {.mip = 0, .layer = 0},
            .mipCount = 1,
            .layerCount = 1,
        },
    };

    CopyDependencyTracker<Representation> dependencies;
    assert(dependencies.RegisterSynchronized(parent, parentRanges, child, childRanges, copyOnly));
    assert(parent->backing != child->backing); // Registration never aliases host representations.
    assert(dependencies.RelationCount() == 1);
    assert(dependencies.GetState(parent) == CopyRepresentationState::Current);
    assert(dependencies.GetState(child) == CopyRepresentationState::Current);

    // A GPU write makes the writer authoritative and every related representation stale.
    assert(dependencies.MarkWritten(child));
    assert(dependencies.GetState(child) == CopyRepresentationState::Current);
    assert(dependencies.GetState(parent) == CopyRepresentationState::Stale);
    assert(!dependencies.CompleteSynchronization(child, parent)); // Never copy from a stale source.

    // A stale representation cannot be read until the exact dependency is synchronized.
    const auto parentRead = dependencies.PrepareRead(parent);
    assert(parentRead.state == CopyReadState::SynchronizationRequired);
    assert(parentRead.source == child);
    assert(parentRead.region.source.mip == 0 && parentRead.region.source.layer == 0);
    assert(parentRead.region.destination.mip == 2 && parentRead.region.destination.layer == 1);
    assert(dependencies.CompleteSynchronization(parent, child));
    assert(dependencies.PrepareRead(parent).state == CopyReadState::Current);

    // Representation-wide validity cannot safely express multiple independent mip/layer
    // aliases yet, so an additional dependency remains on the legacy path.
    assert(!dependencies.RegisterSynchronized(child, childRanges, third, childRanges, copyOnly));
    assert(dependencies.MarkWritten(parent));
    assert(dependencies.GetState(child) == CopyRepresentationState::Stale);
    const auto childRead = dependencies.PrepareRead(child);
    assert(childRead.state == CopyReadState::SynchronizationRequired && childRead.source == parent);
    assert(childRead.region.source.mip == 2 && childRead.region.destination.mip == 0);
    assert(dependencies.GetState(third) == CopyRepresentationState::Untracked);
    assert(dependencies.PrepareRead(third).state == CopyReadState::Untracked);
    assert(dependencies.CompleteSynchronization(child, parent));

    // Physical overlap alone, Full sharing, and CopyOnly without a resolved plan are rejected.
    CopyDependencyTracker<Representation> rejected;
    assert(!rejected.RegisterSynchronized(parent, parentRanges, child, partialRanges, copyOnly));
    auto full = copyOnly;
    full.relation = TextureViewCompatibility::Full;
    assert(!rejected.RegisterSynchronized(parent, parentRanges, child, childRanges, full));
    const ClassifiedResourceView unresolved{.relation = TextureViewCompatibility::CopyOnly};
    assert(!rejected.RegisterSynchronized(parent, parentRanges, child, childRanges, unresolved));
    assert(rejected.RelationCount() == 0);

    // Losing the authoritative representation cannot silently make stale data current.
    auto surviving = std::make_shared<Representation>(Representation{std::make_shared<std::uint32_t>(4)});
    auto temporary = std::make_shared<Representation>(Representation{std::make_shared<std::uint32_t>(5)});
    CopyDependencyTracker<Representation> expiredSource;
    assert(expiredSource.RegisterSynchronized(surviving, parentRanges, temporary, childRanges, copyOnly));
    assert(expiredSource.MarkWritten(temporary));
    CopyDependencyTracker<Representation> merged;
    merged.MergeFrom(expiredSource);
    merged.MergeFrom(expiredSource);
    assert(merged.RelationCount() == 1);
    temporary.reset();
    assert(merged.GetState(surviving) == CopyRepresentationState::Stale);
    assert(merged.PrepareRead(surviving).state == CopyReadState::Unavailable);
}
