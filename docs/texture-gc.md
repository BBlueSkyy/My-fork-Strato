# Conservative texture cache collection

Base: PR #462, 4396c0de. Diagnostic findings: PR #461, ddb7b042.

This implementation frees actual host images and allocations, not just staging.
The first recoverable class is intentionally narrow: one contiguous physical CPU
span, no GMMU mapping in any registered address space, CPU-authoritative bytes,
no alias group, no overlapping representation, no unpreserved GPU write serial, no guest-memory pin, no external
view/descriptor/executor owner and a completed fence. No guest pages are freed.

GMMU Map/Unmap record old physical spans and update interval coverage. Another VA,
another AS or any still-resident portion prevents collection. Reusing a VA updates
coverage before a candidate is inspected. All ASes created by AsGpu share the GPU's
tracker. Sparse holes do not count as physical memory. Index allocation failure
disables collection rather than authorizing release.

The tracker mutex serializes mapping mutations against CPU batch construction.
Lock order is global channel mutex, tracker mutex, then existing GMMU block locks.
The worker releases the new lease before exception handling; its GPFIFO worker
is joined before the lease member is destroyed. Mapping callbacks perform no Vulkan work, trap cleanup or texture destruction.
At a changed mapping generation, the existing submission/flush path invalidates
TIC fast entries and the historical strong store before another batch can reuse
them. A manager-owned deferred queue retains raw view objects until both their previous
image-use fence and any current recorded submission complete. No late dependency
is appended to a submitted fence, and empty cycles are never retained.

Old physical ranges select candidates using the overlap index. A queue processes
at most 16 candidates per batch; an empty queue with no events returns immediately.
There is no full texture scan in a draw or frame. The bounded journal requests a
single event-driven full candidate selection only if 4096 changed ranges overflow.
Erasure publishes a freshly rebuilt index and survivor vector together. Allocation
failure leaves the original lookup/index and image ownership intact.

External users and unfinished fences defer collection. A reference count is only
a final ownership gate after mapping and data proofs; it never proves allocation
death. Trap weak promotions are serialized by the existing texture/state locks.
The CPU-authoritative state prevents any final destructor from encoding obsolete
image contents into reused guest bytes. #462 supplies exactly-once trap cleanup
and framebuffer retirement. Collection adds no GPU waits or readbacks.

## Preserved cases and limits

GPU-dirty images, unmatched write/readback serials (including skipped readbacks), aliases (including MK8 HDR/mip relationships), overlapping
representations, multiple physical mappings, freed guest mirrors and pinned
resources remain resident. Their authoritative bytes cannot be abandoned based
on a VA unmap, age, replaced flag or memory target.

This is cache eviction of reconstructible images, not a guest allocation GC.
A kernel allocation discard/reuse event is not inferred from nvmap handle release
or engine arena unload. Recycling suballocations inside an unchanged live guest
heap cannot be detected and remains blocked. The full physical allocation
generation protocol for GPU writers is still absent; it is not bypassed.

Thus MK11 benefits only if arena changes unmap eligible GMMU spans. The existing
captures do not establish that event pattern; device RAM/FPS improvement requires
a Moto G100 A/B run. VMA reserved blocks and RSS need not shrink with every
destroyed image. PRs #441/#457/#461/#462 and scheduler #362 are not modified.

## Validation

Actions run 43984434487 passed ASan/UBSan and Vulkan core/synchronization validation
on source commit 7e8461e6bb40625965f1f903c3cdd9394b420901:
https://github.com/BBlueSkyy/My-fork-Strato/actions/runs/43984434487

The native Vulkan cases reclaimed 2 images / 1024 allocation bytes per format-list
variant, and another 20 images as bounded 16+4 batches per variant (44 images
total). These are small test allocations, not a measured MK11 RAM reduction.
A real pending image-to-buffer copy and external/recorded views blocked release;
remapping rebuilt backing from every preserved guest byte. The exact MK8-shaped
640x360 HDR child/parent footprints (983040/1310720 bytes) stayed alive.
Existing alias/readback, mip-growth, submission, framebuffer and fragmented
lookup regressions passed. GMMU/channel hooks, generation changes, worker error
unlock and 1000 concurrent mapping mutations passed with the same sanitizers.
The real-trap 180 interleavings and 11 lifetime cases from #462 also passed.

No additional workflow is retained in this change. The existing #462 validation
workflow was used for the checks and restored unchanged after completion.
Android assembly and device gameplay are separate from these host checks.

## Runtime retirement corrections after 9a709beb

Two deterministic regressions were reproduced using production code:

- A CPU write raises SIGSEGV on a trapped page. Before its signal handler looks
  up the trap, collection destroys the final texture and DeleteTrap restores RWX
  and erases the entry. HandleTrap then returns false, so NCE treats an already
  resolved protection fault as fatal. Callback owner pins cannot cover this
  window because no callback has begun. The native regression pauses a real
  SIGSEGV before dispatch: the old code exits through the fatal decision (86),
  while the corrected code retries the instruction and completes the write.
- RetainTextureView used Texture::lock/unlock while an executor could already
  own a tagged lock. The nested unlock erased that ownership tag. The regression
  runs the actual Texture locking methods and observes an erroneous second
  tagged acquisition. Retention now uses the underlying recursive mutex, keeping
  the executor tag and the same protection of the cycle snapshot.

NCE now distinguishes SEGV_ACCERR for the stale-fault fallback. Only when normal
trap dispatch finds no entry does the fallback read current kernel permissions
from /proc/self/maps using fixed stack buffers and open/read/close. It retries
only a currently RWX page, the permissions restored by final trap deletion.
It never changes permissions, retains retired textures/traps, or retries null,
unmapped, read-only, inaccessible or NX pages. Failure to read/parse permissions
rejects the fault. Active-trap dispatch and ordinary GC perform no such scan.

Validation after both fixes passed with Clang 19 ASan/UBSan: the real delayed
SIGSEGV and negative protection controls; 180 trap-owner interleavings and 11
lifetime cases; 1000 concurrent mapping mutations, partial unmaps and VA reuse;
submission/fence, sampled-layout and fragmented mapping/index regressions. Real
lavapipe Vulkan core/synchronization validation reported zero errors. The targeted
lifetime lane still destroyed 44 images, preserved pending copies/external views,
MK8 HDR overlap, guest bytes, aliases, backing promotion and mip growth. Fragmented
Texman lookup/writer preservation also passed. LeakSanitizer was disabled because
this host runs under ptrace; AddressSanitizer and UndefinedBehaviorSanitizer were
active. No workflow, runtime counters or temporary instrumentation was added.

The provided MK8/DBFZ success on #462 and failures on #463 establish the reported
regression. The two code defects above are reproduced independently of a driver.
The attachments were absent from their advertised paths on the resumed host;
there is no symbolized device stack linking each reported PID to these defects.
No claim is made that every recorded exit has the same cause, or that device
FPS/frametime/gameplay has been measured. The collection policy, GMMU/channel
synchronization and scheduler remain unchanged by these corrections.
