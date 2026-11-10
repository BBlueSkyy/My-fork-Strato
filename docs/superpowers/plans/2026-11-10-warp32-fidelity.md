# Warp32 fallback fidelity

**Goal:** Audit the #430 unsupported-stage fallbacks and improve only provably exact cases.

**Architecture:** Keep the Vulkan stage gate and scalar fallback. For constant shuffle controls,
prove across all 32 possible guest lanes that the existing returned value and a true validity
predicate are both exact. Accept literal values or self-lane transfers only. Unknown cases retain
the #430 predicate and value. No new lane mapping, synchronization, or subgroup operation.

**Constraints:** Separate Draft based on master f0a95dd8; preserve GetThreadId in FSwizzleAdd;
no game/hash conditions, texture work, RenderDoc, merge, or Actions monitoring.

## Tasks

- [x] Document each guest operation, existing approximation, exact cases, and limitations.
- [x] Add an independent CPU Warp32 oracle and real emitter tests; demonstrate the old false
  shuffle predicate failing for identity/constant cases.
- [x] Implement conservative predicate proof in emit_spirv_warp.cpp, leaving supported stages unchanged.
- [x] Exhaust constant controls; test dynamic/partial-mask counterexamples and #430 contracts.
- [x] Emit real SPIR-V for supported/unsupported stages and validate with spirv-val.
- [ ] Review, publish compiler commit and parent submodule pin, then open the parent Draft last.
  Opening it triggers ARM64 DevRelease; report its filtered Actions link and stop immediately.

## Review focus

Unknown lane identity; partial active warps; lane-varying values; dynamic or non-five-bit controls;
shuffle validity changed while its returned value remains approximate. Tests must refuse those
unproven promotions. Uniform-looking registers are not proof of uniformity.

## Execution record

The proof passed all 262,144 controls; 102 modules passed universal SPIR-V 1.3 and
78/78 unsupported-stage modules passed Vulkan 1.1. All 24 supported-stage modules
are byte-identical to #430; eight inherit the documented Flat-interface errors.
Independent review found no production blocker. Its two test improvements were
incorporated: assertions stay enabled and sparse-active-mask cases compare the
actual emitter with the oracle. Parent workflow regression checks passed locally
with host-only compatibility flags described in the compiler audit.

Publication is the final step after this checkpoint; no merge or Actions polling.
