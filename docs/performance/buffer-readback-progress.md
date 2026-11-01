# Buffer readback investigation after device captures

Base master fc8eb519 (#409), modern compiler a668a313 unchanged. Old common git
metadata no longer exists, so a fresh clone was created without altering prior
snapshot directories. All 1758 tracked files of the existing diagnostic snapshot
matched the published diagnostic branch, including the Settings include fix.
Other Drafts remain separate; no #411/#412 product changes imported.

The supplied logs confirm 6780 normal buffer downloads /543408128 bytes OFF;
938 /33816576 bytes plus11345 unsafe copies /736321536 bytes ON. Both show116
texture readbacks and no texture shortcut. Full recordings differ in duration
(140.058s vs155.069s), and contain boot/menu periods. The user's steady-window
rates must not be conflated with whole-log totals, display FPS or simulation.
Producer/fence/prepare times overlap and include multiple threads.

The observer also found 9561 MAPERR faults OFF and15388 ON, while only6 OFF
faults qualified as #412 reads and0 ON. This shows why eligibility on this kernel
cannot be inferred from synthetic ACCERR tests. It does not justify changing
fallback routing or prove that every MAPERR is a load.

Step1: revalidate the preparation predicate after acquiring the buffer resource.
A recorded execution can drop AllWrites while the CPU waits for the lock. When
the mirror is already current, waiting on the subsequent GPU reader is not
needed for a mirror store. The write callback already permits this exact state.
Keep cycle untouched so a future backing upload still waits on the reader.
GPU-dirty and still-blocked paths retain their waits/readbacks. Independent
source review found no critical/important issue; host RED/GREEN and native
Vulkan gated-consumer tests validate original and updated byte versions.

This is a correction of an overbroad preparation dependency, not a claim that
the dominant GPU-dirty readback cost is solved. Zelda's frequency/benefit for
this case remain unmeasured. Latest-writer-only waiting needs further auditing:
inline GPU copies and cached writer bindings cannot be classified safely just
from one dirty flag. Regional tracking also needs authoritative write ranges
and alias handling. Do not ship either as a guessed optimization.

Continue on the same branch/Draft and publish each validated step immediately.
If per-resource/state attribution requires another capture, add bounded
instrumentation rather than omit required waits. Do not monitor Actions.

Moto tests: Zelda, Marvel Cosmic Invasion (no magenta), SpongeBob (boot/menu),
initially both readback hacks, maximum clocks and Free Guest OFF. Compare real
progress duration and graphics alongside presentation cadence; no FPS or normal
simulation speed claim until device validation.
