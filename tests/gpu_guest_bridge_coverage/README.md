# Stage C / I3: guest-bridge encoder coverage preflight (PR #424)

Run `python3 tests/gpu_guest_bridge_coverage/run.py` (or `SANITIZE=undefined` if ASan is
unavailable). This test has no Android, Vulkan or Strato runtime dependencies.

`PlanGuestBridgeByteCoverage` computes bounded, exact **guest mapping offsets**
written by the native 2D block-linear encoder, including partial sectors,
right/bottom edge GOBs, mip/layer gaps and padded physical GOB columns. The
independent per-byte inverse-Morton oracle checks every covered byte and every
untouched padding byte. The 1024x1024 case exceeds the direct-copy 128-GOB
limit. Input `GuestBridgeSurface2D` geometry must come from the *actual* format,
`GetBlockLinearMipLayout` and `GetBlockLinearWidthGobs`; the caller must separately
prove resident mappings, reversibility, texture lifetime, level/layer layout and
non-DMI backing. The result is an offset run list. `ResolveGuestBridgePhysicalRuns` then splits
that list across discontiguous physical mappings, retaining each run's guest
byte offset and rejecting physical self-aliases. Neither function creates an
authority owner, lifetime receipt or synchronization. The resolver is also
covered by split-map, reversed-map, alias and overflow negative tests.

**Not implemented here:** retaining the persistent writer/version graph,
waiting for pending consumers, selective publication, trap integration, and
Vulkan staging. This preflight
must not be used as an automatic fallback or proof that any new texture format
is safe. Both activation flags stay OFF. Real Vulkan validation and Moto G100
runtime testing remain required before enabling mixed authority.
