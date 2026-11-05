# Self-contained experimental RenderDoc capture in Strato

Draft PR #422. **No merge.**

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

No ADB, Shizuku, Termux, root, RenderDocCmd, or developer GPU debug settings
are needed in the intended self-contained mode. When enabled, the **system
Vulkan driver** is used (not a custom Turnip driver). The embedded layer must
be discoverable by the Android loader. Strato tests this via
`vkEnumerateInstanceLayerProperties()` and only requests
`VK_LAYER_RENDERDOC_Capture` if present. If absent,
the game should continue normally and the capture button reports failure;
it does not fabricate a file.

**Unverified on the Moto G100**: whether Android 12 exposes the embedded
layer, whether RenderDoc intercepts Strato's instance, and whether games
can be captured and replayed. The standalone RenderDoc Mobile currently
reads RDC metadata, not full Vulkan replay. Game capture may cause large
performance/memory overhead or driver crashes.

The normal `PR Build` workflow retains the existing unrelated
SSBO test failure. This experimental workflow does not change that test.

Source and pin: https://github.com/BBlueSkyy/renderdoc/tree/c041c2a70e2de22b463b5a84274b98ebea81fe6a
