# GPU texture dependency submission order — Naruto Storm 4

## Evidence from mini-RenderDoc capture_001 (PR #336)

Captured title: Naruto Shippuden Ultimate Ninja Storm 4 Road to Boruto
(01006CF00CF60000). Driver: Qualcomm proprietary Adreno 650.
Native present source: 1120×630, RGBA8.

* Captured final source frames 641–643: scene intact, logo absent.
* Captured final source frame 644: scene and logo present.
* `pass_00`: texture 0xB4000079EFA7F548 at guest mapping
  0x1855724000 contains the logo (vertically inverted source).
* `FERMI2D`: source IOVA 0x5A06B4000 (0x1855724000)
  copied back to destination IOVA 0x5900B0000 (0x841694000)
  via negative-height source rectangle to vertically flip.
* `pass_01`: destination texture 0xB4000079EFA73CE8 at
  0x841694000 already lacks logo after that Fermi2D pass.
* Distinct executor identities perform the UI draw and the Fermi2D blit:
  0xB400007B6FC30070 and 0xB400007B6FAB0030.

**Proven**: the intermediate UI output contains the logo but a downstream
Fermi2D destination does not. It is not an Android swapchain-only defect.

**Hypothesis, NOT proven**: two independent command-record threads may
submit the Fermi2D consumer before the prior UI writer reaches Vulkan queue
submission, because `FenceCycle::ChainCycle` orders CPU completion but does
not attach a queue semaphore wait.

## Narrow correctness experiment

In `CommandExecutor::SubmitInternal`, wait for *submission* of each unique
previous texture cycle before publishing the consuming command buffer.
`WaitSubmit()` is not a GPU fence wait, sleep, or global device idle; it
waits for `NotifySubmitted()` after the earlier `vkQueue.submit`.
The existing `ChainCycle` remains in place for completion/lifetime.

This is generic synchronization code, not a title-specific bypass.
No changes to shaders, texture format interpretation, Fermi2D sampling,
GMMU, or NCA.

## Validation required before merge

1. Confirm PR Android reldebug/release builds and host GPU tests pass.
2. A/B test the unchanged master and this PR on the same phone/driver.
   Observe logo/menu UI over multiple boots and at least two minutes.
3. Check FPS, frame pacing and other GPU-heavy games for regressions:
   waiting on a previous *submission* can still introduce CPU-side stalls.
4. If flicker persists, do **not** merge this change. Use phase-3 capture
   or GPU queue-order instrumentation to test stale alias/view ownership.

This PR must remain Draft until device testing establishes a reproducible
improvement and the performance impact is acceptable.
