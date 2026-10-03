# Texman v2 Subresource Validity Design

## Context

PR #244 currently has a validated conservative Full path and a behavior-neutral CopyOnly
dependency foundation. The existing `CopyDependencyTracker` assigns one epoch to an entire
representation and consequently restricts every representation to a single dependency. That
model cannot distinguish independent mip levels, array layers, or 3D depth slices.

This stage replaces representation-wide authority with an explicit graph whose endpoints are
individual texture subresources. It remains metadata-only: no CopyOnly relationship is registered
by `TextureManager`, no Vulkan copy is scheduled, and no production read or write path changes.

## Goals

- Track validity independently for mip, array layer, and depth slice.
- Represent multiple CopyOnly dependencies without mixing unrelated subresources.
- Identify the newest authoritative representation for each connected subresource region.
- Preserve stale state when the authoritative representation disappears.
- Describe the exact direct synchronization required before a stale representation can be read.
- Keep the current Full path, legacy selection precedence, fallback behavior, #250 semantics,
  `Texture::replaced`, and all Vulkan integration unchanged.

## Non-goals

- Enabling CopyOnly in `TextureManager`.
- Executing Vulkan copies or format conversions.
- Connecting the metadata to `UsageTracker`, `CommandExecutor`, traps, or render-target writes.
- General dirty propagation outside CopyOnly relationships.
- Replacing `Texture::replaced`.
- Generalizing the validated #250 depth-slice synchronization path.

## Subresource Identity

An exact endpoint is identified by:

```text
(TextureStorage ownership identity, mip, layer, depthSlice)
```

The ownership identity uses `shared_ptr`/`weak_ptr` control-block identity rather than the raw
address. This keeps an expired representation distinct from a later allocation that reuses the
same address.

`depthSlice` is zero for ordinary 1D/2D resources and array layers. For a 3D image it identifies
the Z slice within the selected mip. A depth slice is not treated as a Vulkan array layer.

## Resolved Copy Relationships

`ResolvedCopyRegion` becomes an explicit list of proven pairs:

```text
(backing mip/layer/depthSlice) <-> (requested mip/layer/depthSlice)
```

The normal resource classifier constructs one pair for every selected mip/layer that already
passes the existing exact guest-byte, dimension, tile-geometry, image/view, and format checks.
Regular classifier output uses depth slice zero.

The representation is intentionally a list instead of a rectangular base plus counts. This avoids
assuming that a multi-mip 3D relationship has constant depth and permits a future #250 adapter to
describe individually proven depth slices. This stage does not add that adapter or register #250
relationships at runtime.

Registration is rejected unless all of the following hold:

- the classification is CopyOnly and does not contain a shared Full view;
- the resolved mapping list is non-empty and has no duplicate or conflicting endpoint pairs;
- one complete ordered guest resource is contained in the other, as required by
  `IsCompleteGuestAlias`;
- every mapping names a distinct exact subresource on each side;
- every mip and layer exists in its representation's real `TextureResourceLayout` and every
  depth slice is less than the depth of that specific mip/layer;
- any already tracked endpoint is current when the caller declares the representations
  synchronized.

A physical partial overlap alone never creates shared validity.

## Graph and Generation Model

Each graph vertex is an exact subresource endpoint. Each edge is one direct CopyOnly mapping
between equivalent regions in two distinct storages. A storage may participate in multiple edges,
including several representations of the same region and independent edges for different mips,
layers, or depth slices.

Every endpoint stores a monotonically increasing generation. For one connected component:

- the component generation is the maximum generation of all its endpoints;
- an endpoint is `Current` exactly when its generation equals the component generation;
- an endpoint with an older generation is `Stale`;
- no query changes an endpoint generation.

Multiple endpoints may have the same current generation only when their equality was established
by synchronized registration or by a completed synchronization. Numeric generations are compared
only inside one connected component, so equal numbers in unrelated components do not establish
equivalence.

### Synchronized registration

Registration is atomic across every pair in the relationship.

- If both endpoints are new, both start at one fresh generation.
- If one endpoint is already tracked, it must be current; the new endpoint inherits that current
  generation because `RegisterSynchronized` asserts equivalent contents.
- If both endpoints belong to different existing components, both must be current. Their current
  frontiers receive one fresh generation before the edge joins the components. Older stale
  endpoints remain stale.
- A repeated identical relationship is idempotent; contradictory mappings are rejected.

Failure leaves the graph unchanged.

### Write

`MarkWritten(representation, subresources)` first validates and deduplicates the complete batch.
An empty batch, an invalid coordinate, or any untracked endpoint rejects the whole operation without
changing a generation. After successful validation it allocates one fresh generation for the write
and assigns it only to the unique named endpoints. Other endpoints retain their previous
generations.

Consequences:

- writing mip 2 does not affect mip 1;
- writing layer 3 does not affect layer 2;
- writing depth slice 5 does not affect depth slice 4;
- all equivalent endpoints in the affected component become stale unless they are explicitly
  synchronized afterward;
- successive writes to different representations leave the endpoint from the latest write as the
  only current generation for that component.

Untracked subresources remain outside this metadata model and continue to use legacy behavior.

### Read preparation

`PrepareRead(representation, subresource)` is observational and never promotes stale data.

- Return `Untracked` when the endpoint has no CopyOnly validity state.
- Return `Current` when its generation equals the component generation.
- Return `SynchronizationRequired` only when a directly connected, live endpoint has the component
  generation. The result contains that source and the exact source/destination subresource pair.
- Return `Unavailable` when the newest endpoint has expired, when no direct authoritative neighbor
  exists, or when the only path crosses a stale intermediate representation.

If an authoritative source is not directly connected to the destination, callers must first
synchronize a direct intermediate endpoint and then prepare the final read again. The tracker does
not invent a transitive copy operation.

### Synchronization completion

`CompleteSynchronization(destination, destinationSubresource, source, sourceSubresource)` succeeds
only when:

- the exact pair is a direct registered edge;
- the source is live and current for that component;
- the supplied direction matches the registered subresource mapping.

Completion assigns the source generation only to the destination endpoint. It does not mark the
rest of either storage current and does not affect other mappings in the same relationship.

## Representation Lifetime

Graph nodes and endpoint generations outlive a representation through weak ownership metadata.
If the sole current representation expires, its generation remains the component maximum. Older
live endpoints therefore remain stale and `PrepareRead` returns `Unavailable`. An expired source
never causes the next-oldest endpoint to become current.

Repeated group merges preserve weak ownership identities, endpoint generations, and edges. Merging
the same dependency metadata twice is idempotent. After every merge, `nextGeneration` is strictly
greater than every generation imported or already present, so the next write cannot tie an older
authority accidentally.

## TextureGroup API

`TextureGroup` exposes only subresource-granular wrappers:

- register a synchronized CopyOnly relationship;
- mark one or more exact subresources written;
- inspect one exact endpoint state;
- prepare one exact endpoint for reading;
- complete one exact direct synchronization.

The current representation-wide wrappers are removed or replaced because retaining them would make
whole-storage invalidation easy to reintroduce accidentally.

## #250 Preservation

The block-linear 3D depth-slice logic in `TextureManager` remains unchanged. It continues to:

- calculate physical Z-slice offsets using `GetBlockLinearDepthSliceOffset`;
- prefer the established legacy representation;
- synchronize GPU-written 2D slice textures to guest memory before creating a complete 3D backing.

The new `depthSlice` coordinate makes that relationship representable later, but this stage does
not reroute #250 through CopyOnly metadata or Vulkan copies.

## Tests

Host tests must cover:

1. A write to one mip stales only the equivalent mip.
2. A write to one array layer leaves other layers unchanged.
3. A write to one 3D depth slice leaves other slices unchanged.
4. Two CopyOnly representations of one region alternate writes; only the latest generation is
   current after each write.
5. Authoritative-to-stale propagation across direct dependencies.
6. `PrepareRead` never promotes stale data.
7. Destruction of the sole authoritative representation leaves live peers stale and unavailable.
8. Multiple dependencies for the same region and independent dependencies for different regions.
9. A transitive path with a stale intermediate is unavailable until the intermediate is explicitly
   synchronized.
10. Partial physical overlap does not register shared validity.
11. Relationship registration is atomic on invalid input.
12. Mip, layer, and depth slice registration is rejected when it exceeds the real layout, including
    the smaller depth of higher 3D mips.
13. Batched writes deduplicate coordinates and reject invalid or untracked input atomically.
14. Group metadata merge is idempotent, preserves generations, and advances `nextGeneration`
    beyond every imported generation.
15. Existing Full classification, resolved view selection, and legacy-precedence tests remain
    unchanged and passing.

## Expected Code Scope

- `resource_layout.h`: exact subresource coordinates and explicit resolved CopyOnly mappings.
- `copy_dependency.h`: subresource graph, generations, state queries, and direct synchronization.
- `storage.h`: subresource-granular `TextureGroup` wrappers and metadata merge.
- texture host tests and their runner.

`texture_manager.cpp`, Vulkan copy code, `Texture`, `UsageTracker`, `CommandExecutor`, and
`BufferManager` are not modified in this stage.
