# Scheduler/kernel audit

Base: `075d1baf8a5d84a291c9bb9d0123c347a6832e46` (`master`).
Branch: `fix/scheduler-kernel-modernization`.

Status: **IMPLEMENTADO MAS NÃO VALIDADO** until Android/game tests listed below
have been performed. Compilation and host regression tests do not establish
game compatibility.

## Baseline and scope

The checked-out master has the original partial `GetThreadContext3` implementation
(19 general registers, no PC/SP) and no `fairnessYieldTarget` or idle-core stealing
implementation. The surviving `fix/nce-full-thread-context` branch at `10f48b5d`
also has the same scheduler and NCE files. No prior scheduler/context work was
removed or blindly merged. Existing memory, audio, GPU, filesystem, HID and service
fixes are outside this change.

The one-host-thread-per-guest-thread AArch64/NCE architecture, per-core queues,
priority numbering and 10 ms preemption interval are retained. This audit is not
a complete reimplementation of Horizon's kernel.

## Findings before implementation

| Area | Classification | Evidence / consequence |
| --- | --- | --- |
| Adaptive scheduler condition variable | deadlock possível; race possível | `wait()` manually unlocks `fallbackMutex` while its `unique_lock` still owns it; destruction unlocks twice. Timed waits use wall time and can overflow their deadline. |
| Runnable queue insertion | race possível | Duplicate insertion is only logged in debug builds; no terminated-thread guard. |
| Queue operations | incompleto | `Rotate`/`UpdateCore` dereference an empty queue; `UpdatePriority` advances `end()`. |
| Core selection | race possível; semântica HOS incorreta | Queue reads outside locks; current disallowed core can be returned; remaining-time subtraction underflows and uses `min(...,1)`; weighted timeslice formula has misplaced parentheses. |
| Timed scheduling wait | deadlock possível | Takes migration mutex while holding core mutex, opposite to insertion/affinity operations. |
| Park/yield | deadlock possível; semântica HOS incorreta | Parked waiter holds migration mutex across wait, never removes itself from parked queue; wake dereferences null `nextThread` and ignores affinity. |
| Preemption | race possível; incompleto | `pendingYield` published after sending; status waits under queue locks; stale/failed timers not accounted for; disarm skips killed threads. |
| Priority inheritance | semântica HOS incorreta; deadlock possível | Chain propagation breaks after first reorder, before scheduler update; rollback overwrites concurrent donation; restoration takes minimum with old donation; nested waiter locks have inconsistent order. |
| `SetThreadPriority` | semântica HOS incorreta | Raising urgency can leave effective priority unchanged; lowering can discard donation; equal effective priority skips base-priority update. Input narrows to 8 bits before validation. |
| `WaitSynchronization` | semântica HOS incorreta; incompleto | Invalid handles escape as exceptions; zero handles indexed for tracing; duplicate handles return last index; cancellation precedes already-signalled objects; timeout/signal race may return before scheduling. |
| `KSyncObject` normal wake | correto | Shared lock serializes registration/signal/cancel, and `isCancellable` selects a single winner. Exception/termination cleanup still needs auditing. |
| Address arbiter | semântica HOS incorreta | Less-than comparison is unsigned; timeout clears caller's word; modify-and-signal uses incorrect decrement/boundary; shares namespace with condition-variable keys. |
| Condition variables | semântica HOS incorreta; race possível | Zero timeout waits forever; signal error overwritten by success; timeout removes donation without restoring owner; signal publishes runnable before wait result. |
| Lifecycle | deadlock possível; race possível | Process joins under creation mutex; thread can restart after normal exit; signal sender waits forever for normally exited thread; self Kill(join) can wait on itself. |
| `ExitProcess` | semântica HOS incorreta | Normal exit calls crash-reporting Kill overload and does not immediately disable creation. |
| Affinity SVC | semântica HOS incorreta | 64-bit mask truncated before validation; invalid signed core can be passed to bitset::test; CreateThread misses NPDM core permission. |
| NCE context | incompleto; race possível | Pause removes queue membership without confirming host/guest has stopped updating context. Full context is absent in this master. |
| NCE FP state | semântica HOS incorreta | FP status fields alias vector 0 in C++; assembly status slots overwrite high half of vector 31. |
| Host scheduling signals | suspeito | General signal installer ignores requested `syscallRestart`; changing global signal policy affects other subsystems and requires separate analysis. |
| SDK / memory / services | suspeito | No new game log supplied. A boot failure cannot be attributed to scheduler solely from SDK age. No title-specific fixes are justified. |

## Behavioral references

Behavior was compared with the independent Mesosphere kernel implementation,
not copied or cherry-picked:

- [Thread SVCs](https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libmesosphere/source/svc/kern_svc_thread.cpp)
- [Thread state and priority inheritance](https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libmesosphere/source/kern_k_thread.cpp)
- [Synchronization objects](https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libmesosphere/source/kern_k_synchronization_object.cpp)
- [Condition variables](https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libmesosphere/source/kern_k_condition_variable.cpp)
- [Address arbitration](https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libmesosphere/source/kern_k_address_arbiter.cpp)
- [SVC reverse-engineering documentation](https://switchbrew.org/wiki/SVC)

## Validation plan

1. Compile changed C++/assembly for AArch64 Android; build APK through existing PR CI.
2. Run focused regression tests for queue/wait, donation and ABI invariants.
3. On device, first test two titles known to work on this exact master. Test boot,
   menu, gameplay, returning to launcher, and starting another game.
4. Then collect logs for 2–3 failing titles, starting with one old title and one
   recent SDK title. Record exact game/update, driver, settings, build commit,
   failure point and whether the master reproduces it.
5. Compare actual wakeup/context/boot progress; do not claim a fixed game merely
   because an exception disappeared or compilation passed.

No game regression has been measured in this environment. No game is currently
proven to fail because of the scheduler, nor proven fixed by this branch.

## Implemented changes

- Adaptive CV: use the owning `unique_lock` to unlock, and a saturated steady-clock
  deadline. The existing ARM spin strategy is unchanged.
- Synchronization: one process mutex protects the PI graph and its wait queues;
  priorities are recomputed from base priority and remaining donors along the
  complete chain. Mutex handoff and timeout cleanup restore the old owner's
  priority. Condition-variable and address-arbiter namespaces are separate.
- Address arbiter: signed comparisons and correct waiter-count boundaries;
  timing out no longer overwrites the guest's arbitration word.
- Runnable queues: explicit membership, idempotent removal, killed-thread guards,
  locked core selection, affinity validation, and repaired park/wake operations.
- Preemption/lifecycle: publish yield requests before sending signals, clear
  pending requests before rotation, guard timer creation/arming/deletion, forbid
  restart after termination, remove wait registrations on exit, and request all
  process stops before joining without the thread-creation mutex held.
- Thread SVCs: validate full-width priorities/masks, handle duplicate/invalid
  synchronization handles and signal-versus-timeout ordering, distinguish normal
  process exit from a crash, and reject a second StartThread.
- NCE context: preserve existing TLS offsets; save X19-X30 and guest SP/PC;
  separate FPSR/FPCR from vector storage; publish a complete synchronized snapshot
  from SVC entry or the AArch64 signal frame. GetThreadContext3 waits for a complete
  snapshot while the target is paused.

## Remaining limits, without additional refactoring

All requested areas have been reviewed; coverage does not mean complete HOS
emulation or game validation. In particular:

- The final host-to-guest check/TLS transition can still lose a deferred scheduling
  signal. It needs a device reproducer before changing the NCE transition protocol.
- A signal taken inside generated patch/hook code can expose a trampoline PC.
  Normalization to the corresponding guest instruction is not implemented.
- Remote SetThreadCoreMask and SetThreadActivity do not implement Horizon's full
  synchronous acknowledgement/pinned-thread protocol.
- The inherited asynchronous SIGINT/C++ exception/longjmp architecture remains.
  This branch fixes concrete cleanup/lock problems without replacing it.
- Cyclic guest mutex dependencies remain guest deadlocks. The PI traversal stops
  rather than spinning the host indefinitely; it does not manufacture an unlock.
- Direct guest-pointer validation is incomplete in some SVCs. Memory-manager and
  NPDM/service compatibility questions need their own evidence and changes.
- Existing GetInfo IsVammEnabled compatibility behavior is outside this work.
  No GPU, audio, filesystem, service, or memory-manager fixes are included.

## Build and test status

The previous environment completed the native AArch64 Android `skyline` target
(including the shared-library link) with NDK 27.3.13750724. That environment was
reset before the local commits were pushed. The changes and focused tests were
recovered from the recorded work and published with new commit hashes. The old
native build output is not a substitute for checking this recovered tree.

On the recovered tree, the PI/arbitration test passed with UBSan, and 1,000
production CV fallback wake cycles plus timeout/deadline checks passed with
ThreadSanitizer. The recovered SaveCtx/LoadCtx assembly also passed the Unicorn
AArch64 GP/SP/SIMD/FP-status and instruction-count checks. See
`tests/kernel/README.md` for reproducible commands.
The full Android build is tracked in [PR #146](https://github.com/BBlueSkyy/My-fork-Strato/pull/146).
APK generation is required before device tests; it is not itself game validation.
