# Cosmic Shake experiment B — isolate Free Guest memory from generic GPU ownership

## Tested configuration (required)

- Use this experiment B APK with the **Free Guest Texture Memory UI option ON**.
- Despite the UI setting, the generic `gpuDirty` attachment policy is forced OFF for *nonexplicit* writers in `SynchronizeHost()` and `SynchronizeHostInline()`.
- Actual guest memory freeing still follows the UI setting; RT/storage real writers still call `RecordGpuWrite()`.
- Logs identify the build with `[GPU-MEMAB] experiment=B-policy-OFF-free-ON` and include the user setting as `free_guest=true/false`.

## Safety / valid inference

The PR targets **master only to activate the existing PR Build workflow**, but the experiment code branches from PR #477. This changes no master file and is not merge-ready.\n\nThis is **experimental behavior**, NOT a candidate fix. Do not merge. The experiment intentionally changes dirty-state ownership and can break CPU/GPU memory coherence and render wrong or crash. Use only the SpongeBob initial title menu, do not enter gameplay or save progress, and do not evaluate FPS with instrumented APKs. Preserve the existing installation/backup before testing a diagnostic build.

Compare **at least six complete game boots per variant** with the same Turnip and identical settings other than the required Free Guest option, plus six control boots from the PR #477 APK if needed. Exit the game between boots. Export full emulation logs for one representative nearly correct and one severe boot when possible, and attach screenshot with corresponding build, policy, setting and boot number.

| Case | Build | Free Guest memory UI | Generic gpuDirty policy |
|---|---|---|---|
| Original OFF | PR #477 | OFF | OFF |
| Original ON | PR #477 | ON | ON |
| Experiment A | this variant's PR | OFF | ON |
| Experiment B | this variant's PR | ON | OFF |

Neither a correlation nor an isolated improvement proves the first incorrect GPU write. If one split reproduces the benefit, trace the actual texture/guest mapping and validate a minimal production fix separately without experimental overrides or logging.
