# Naruto Connections — four-frame mini-RenderDoc (diagnostic Draft)

This branch ports *only* the instrumentation from closed PR #357 onto master
after PR #359. It does NOT bring back the older #356 synchronization changes
and does not change production GPU rendering semantics when the capture is off.

## Capture both drivers on Moto G100

1. Install this PR's **reldebug** APK, not the old APK from #336/#357.
2. On Naruto Connections character selection, reproduce the P1 skills list
   flickering using Qualcomm proprietary.
3. In the public Strato files, open `gpu_capture/naruto_connections/` and rename
   `ARM_CAPTURE.rename_this_file_when_ready` to `ARM_CAPTURE`.
4. Return to character selection for four presentations. Wait for
   `MINIRD_CONNECTIONS wrote snapshot` in Info logs *after*
   `capture_complete.txt` before zipping the `capture_###` directory.
5. Upload full ZIP + matching `emulation.log` + screenshot/video.
6. Repeat with **Turnip 24.0.99**, same characters/menu, fresh session.

**Known symptoms:** proprietary driver loses the P1 skill bars transiently;
Turnip draws stable skill bars, but places the Connections logo on P1/CPU
character nameplates. The proprietary log had 29 `TIC_UNMAPPED`-equivalent
warnings for IOVA `0x588A90000` (these were not proven causal).
`Mirror Clamp To Edge` warning exists on both drivers. Proprietary enables
`needsIndividualTextureBindingWrites`, Turnip doesn't. One driver quirk
alone does not prove cause.

## Diagnostic budget and limitations

Four raw presented source frames, up to 24 globally named pass/atlas snapshots
(6/interval, 2 sampled atlases/interval), max 8 MiB per backing and 160 MiB
intermediate total; 12 MiB/presentation and 2400 bounded GPU events.
Supports compressed and uncompressed color atlases, full-sized targets,
actual descriptor-array counts and trait flags, shaders, CPU-side CBUF prefixes,
vertex/index data, TIC/TSC mapping, and Fermi2D/DMA events.

Presentation intervals are **approximate**, since rendering and presentation
are asynchronous. Renderpass splitting and readbacks may perturb timing
and temporarily mask a race. The first matching draw is not necessarily the
defective UI draw; compare log draw identities and pixels before inferring
cause. CBUF prefix dumps can be stale if host GPU writes were not read back.

Only **reldebug** is built on this diagnostic PR; normal master build outputs
remain unchanged. Do not merge this instrumentation. No game/driver hacks.
