# Texman Directional Copy Capability Design

## Scope

This stage describes whether one exact CopyOnly endpoint can be synchronized into another.
It does not register CopyOnly relationships in `TextureManager`, submit Vulkan work, or change
runtime texture selection. Full reuse, the #250 depth-slice path, legacy fallback, and the
existing validity graph remain unchanged.

## Separation of concerns

The CopyOnly graph proves that two resolved `(mip, layer, depthSlice)` endpoints describe the
same guest data. A separate directional registry proves that one concrete operation is valid
from a source endpoint to a destination endpoint. Registering `A -> B` never implies `B -> A`.

The only capability introduced here is `ExactImageCopy`. Registration requires:

- an existing direct semantic edge for the exact endpoint pair;
- non-3D images and `depthSlice == 0` on both endpoints;
- identical nonzero host-format and aspect identities;
- one sample on both images;
- source transfer-read and destination transfer-write support in the requested direction;
- exact equality of the resolved mip extents, with no scaling or conversion.

No format conversion, blit, 3D/slice mapping, partial overlap, or transitive semantic path is
accepted.

## Preparation and completion

`PrepareSynchronization(destination)` first asks the validity graph to prepare an exact read.
A current destination produces no copy. A stale destination produces a plan only when the
graph returns a direct current source and the exact `source -> destination` capability exists.
Routes from stale sources and reverse-only routes are ignored.

The prepared plan captures both endpoint generations. Execution remains external. Completion
receives the prepared plan plus an explicit success result and updates the validity graph only
when:

- execution succeeded;
- the route still exists;
- the endpoints remain directly related;
- both source and destination generations still equal the captured values; and
- the source is still current.

A write to either endpoint after preparation invalidates the plan. Completion affects only the
destination endpoint named by that plan.

## Integration boundary

`TextureGroup` owns the capability registry beside the existing dependency tracker and exposes
metadata-only forwarding methods. Group merges merge directional routes idempotently. No code
in `TextureManager`, `Texture`, Vulkan submission, `CommandExecutor`, `UsageTracker`, or
`BufferManager` calls these methods in this stage.
