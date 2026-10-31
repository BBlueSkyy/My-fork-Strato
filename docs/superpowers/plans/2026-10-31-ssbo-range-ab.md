# SSBO range A/B implementation plan

Goal: one controlled experiment, not a permanent fix. User explicitly authorizes preparing the experiment without another planning approval.
Base: c7cb28d, compiler dcb93886. Both ARM64 DevRelease arms use one commit; only build-time STRATO_SSBO_RANGE_AB differs.

- [x] Inspect GOOD/BAD paths and existing logs. CBUF0 runtime size words absent; compile snapshot only CBUF1. No compiler/replay/Eden retesting.
- [x] Tests first for certificate, padded safe extent, unknown-module refusal, bounded observations and recorded view/backing distinction.
- [x] A unchanged ranges; B only certified Compute E8B459126941F6CE raw readonly cbuf0:280. Require exact final SPIR-V SHA256 before using previously proven4608B bound. Never shrink unrecognized modules, offsets or writes. Keep default modern fallback and synchronization.
- [x] Both arms read the same descriptor struct once to observe +8 word for280/2A0; record counterfactual GOOD clamp and actual request/view/resolved range/backing. No buffer-state, trap, fence or coalescing changes.
- [x] Bounded fixed counters, no per-draw log, one5s FPS report plus changed rows. Record descriptor during existing resolution, not by forcing early resolution. No additional resource locks.
- [x] One workflow matrix A/B, Java17, existing assembleDevRelease, arm64 only, compiler pin and APK/provenance checks. No monitoring after request.
- [x] Fresh review found no blockers; publication follows. One experimental branch and PR404 comment, no production PR/merge.

Device protocol: restart app/process between arms, same save/camera/area and Turnip24.0.99/settings. Warm both; measure fixed90s windows, at least A-B-A order to expose thermal drift (reuses same two APKs). Record overlay/screens and complete rendering. Reject mismatched certificate, truncated mappings, dropped observation rows or scenes/workloads. Do not compare snapshots polluted by startup/menus.

Verdict limits: a gain with stable graphics supports this target sizing mechanism; no gain with a real request/view reduction rules it out as the dominant cause in that scene. If backing remains large, it does not refute whole-buffer coalescence costs. Other unknown shaders (e.g.290) are not silently covered by this experiment. No global hypothesis verdict from incomplete coverage.
