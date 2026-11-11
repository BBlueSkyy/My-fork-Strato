# Self-contained experimental RenderDoc capture in Strato

Draft PR #422. **No merge.** Current branch includes master merge
`f0a95dd80eb150e81d16e1b0fa88c3a812a820c8` (PR #430)
plus the capture-only correlation hooks.

## Zelda: Echoes of Wisdom — capture the black frame with Turnip

The earlier November 5 APK **forced Android's system Vulkan loader**;
that cannot reproduce the black-screen bug when the game is run on
Turnip 24.0.99. The updated Draft instead uses the **normal AdrenoTools
selected driver path**. Only if the capture preference is enabled
at game launch does it attempt to place the bundled RenderDoc Vulkan
layer above that driver using a Vulkan loader-info chain.

**Runtime capture is experimental and not verified on the Moto G100.**
The bridge does not switch to Qualcomm on failure. If the layer fails
to hook the selected device, the camera action returns an explicit
failure and never claims to have saved an RDC. A successfully loaded
RenderDoc .so is NOT evidence of an intercepted Vulkan device.

1. Install a **successful** `RenderDoc Bundled - ARM64 DevRelease`
   APK built from this updated PR (not the old November 5 artifact).
2. In the Strato GPU settings, select **Turnip 24.0.99**.
3. Start Zelda: Echoes of Wisdom; tap the camera icon below Pause,
   select **Enable**, then exit/relaunch the **game** to recreate Vulkan.
4. Confirm `emulation.log` says `Driver ID: MesaTurnip` and
   `RenderDoc: selected-driver Vulkan device intercepted`. If either
   is absent, do **not** treat the frame as a Turnip capture.
5. Enter gameplay with the black 3D scene already visible. Tap the
   camera icon once. This requests **one frame**; it does not capture
   the entire boot. Wait for `RenderDoc frame captured` and a real
   non-empty `.rdc` file before exporting anything.
6. Choose **Export .rdc**, save into Downloads, open it with the
   separate **RenderDoc Mobile** app and use **Export AI diagnostics
   (.zip)**. Replay, images and Post-VS extraction are not guaranteed
   for a game of this size on a phone.
7. If available, also choose **Export ZIP + links** within Strato to
   save the original RDC and the optional
   `<frame>.rdc.strato-links.jsonl`. This sidecar helps correlate
   TIC/IOVA with Vulkan images; it is not a replay file.
8. Share `emulation.log`, the RenderDoc Mobile AI ZIP, and the
   Strato links ZIP if it is a manageable size. If capture or replay
   crashes, share the log, not fabricated outputs.

Full RDC captures can be very large and exhaust Android memory;
do not loop the capture button. Disable capture after the test and
relaunch the game to restore the normal (unintercepted) Vulkan path.
The PR remains **Draft**, with **no merge**.


The CI workflow compiles the RenderDoc ARM64 Vulkan layer from pinned fork
commit `c041c2a70e2de22b463b5a84274b98ebea81fe6a`
and bundles it as `lib/arm64-v8a/libVkLayer_GLES_RenderDoc.so`
inside the experimental Strato **DevRelease** APK. No binary blob is added
to Git. RenderDoc uses the MIT license.

## Use on a phone, without ADB or Termux

1. Install the APK from the **RenderDoc Bundled - ARM64 DevRelease**
   workflow artifact `strato-renderdoc-bundled-arm64`.
2. Start a game, and tap the camera icon under Pause.
3. Choose **Enable**. This saves a Strato-local preference.
4. Exit the game and reopen it. Vulkan must recreate its instance.
5. Tap the camera again to request a captured frame.
6. If a real RDC is saved, use **Export .rdc** to choose Downloads.
   Open it with **RenderDoc Mobile** using Open .rdc capture.
7. Long-press the camera and select **Disable** to restore normal graphics
   behavior after another game relaunch.

No ADB, Shizuku, Termux, root, RenderDocCmd or developer GPU debug settings
are required for the intended opt-in mode. The **selected driver remains
selected** (system Qualcomm or AdrenoTools Turnip); the layer is
directly chained rather than relying on Android's layer enumeration.
If the bundled layer cannot be loaded/linked, the game stays on the
selected driver, but the capture request reports that there is no
intercepted Vulkan device. The original APK-bundled layer is unchanged.
This new direct-driver chain requires real Android validation.

**Unverified on the Moto G100**: whether the new direct layer bridge
intercepts the Turnip instance/device, whether Zelda can be captured
without memory exhaustion, and whether the standalone RenderDoc Mobile
can replay such a large RDC. Do not infer success from APK compilation.
The separate RenderDoc Mobile exporter supports more than metadata
on certain captures, but GPU replay and texture/vertex exports still
depend on the actual capture and driver.

The normal `PR Build` workflow retains the existing unrelated
SSBO test failure. This experimental workflow does not change that test.

Source and pin: https://github.com/BBlueSkyy/renderdoc/tree/c041c2a70e2de22b463b5a84274b98ebea81fe6a

## Capture correlation for AI diagnosis (Draft #422)

While the experimental RenderDoc capture is explicitly enabled, the camera button
now also prepares a per-capture JSONL sidecar. It records up to 4096 TIC/image
binding associations and a separate bounded collection of synchronisation
observations. The file is renamed to `<capture>.rdc.strato-links.jsonl` when the
RDC file has been verified on disk.

The captured metadata can include: TIC index, TIC IOVA, native Vulkan image
handle, dimensions, TIC format word, channel sequence and a best-effort
snapshot of dirty/alias state. Synchronisation observations cover declarations
of GPU writes, guest-to-host upload requests and host-to-guest readbacks.
These observations are NOT execution-order proofs and NOT a replacement
for checking the actual Vulkan barriers, hazards or guest region authority.

An optional VK_EXT_debug_utils extension is enabled only for explicitly
captured runs if the instance advertises support. Strato attempts to give
each Vulkan VkImage a debug name like `STRATO TIC=17 IOVA=0x1234` so the
RenderDoc resource inspector may recover the guest association. If this
extension is unavailable, the sidecar still contains process-local VkImage
handles, but they MUST NOT be directly equated to RenderDoc ResourceIds.

After a capture, choose **Export ZIP + links** to stream both the original
RDC and correlation JSONL into one Android Save Document ZIP. The existing
**Export .rdc** path remains available for opening the capture in RenderDoc
Mobile. A TXT warning is included if the correlation file was unavailable.
No PC, ADB, Termux, root or network permission is required.

The path shared between JNI and renderer threads uses capture-local locked
state with an atomic fast check, not a concurrent getenv()/setenv() race.
Normal sessions without the RenderDoc opt-in remain unchanged.

For GPU diagnosis, share this Strato ZIP and the separate RenderDoc Mobile
**Export AI diagnostics** ZIP, plus emulation.log. Validate in the Moto G100
before claiming that a particular TIC or IOVA produced a black texture.
Do not merge this Draft before the Android ARM64 build and runtime test.
