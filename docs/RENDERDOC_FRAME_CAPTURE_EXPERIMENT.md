# Experimental RenderDoc frame capture for Strato

This branch implements a **request / verification / export** bridge for one RenderDoc Vulkan frame capture. It does **not** bundle a RenderDoc Vulkan layer, alter the graphics scheduling model, or enable host Vulkan replay.

## How to test

1. Install the **RenderDocCmd ARM64 APK** from [BBlueSkyy/renderdoc](https://github.com/BBlueSkyy/renderdoc/actions). It contains the Vulkan capture layer; RenderDoc Mobile is the viewer.
2. Use **Strato DevRelease** from this PR. The package is `org.stratoemu.strato.dev`; Android's debugger must target this exact package (not the mainline Strato package).
3. Configure GPU debug layers on Android **before launching** Strato. With wireless ADB (e.g. paired on-device ADB client) or a PC:

   ```sh
   adb shell settings put global enable_gpu_debug_layers 1
   adb shell settings put global gpu_debug_app org.stratoemu.strato.dev
   adb shell settings put global gpu_debug_layer_app org.renderdoc.renderdoccmd.arm64
   adb shell settings put global gpu_debug_layers VK_LAYER_RENDERDOC_Capture
   ```

4. In Strato's graphics settings, initially select **the system/proprietary Vulkan driver**. Strato uses AdrenoTools for Vulkan driver loading, which can bypass Android's layer loader for custom drivers. Capturing under Turnip/custom drivers is not yet validated.
5. Restart Strato after changing debug-layer settings. Start a game and tap the small **camera icon** under the pause button. Long-press the icon to see setup guidance.
6. If the RenderDoc API can be accessed, the capture is requested for the next Vulkan frame. The UI checks the capture registry for a non-empty `.rdc` under Strato's app-specific capture folder. It does not claim a successful capture merely from an API call. Once successful, tap **Export .rdc** to save through Android's Storage Access Framework to Downloads.
7. In RenderDoc Mobile, tap **Open .rdc capture** and select the exported file.

**Limitations**: Android cross-package Vulkan debug layers require developer/debug configuration; the Strato APK cannot silently grant itself permission or inject a layer into other apps. With external RenderDocCmd as layer provider the native module may remain invisible across Android linker namespaces; in that case the UI reports that condition instead of producing a fake capture. Changes are experimental, not yet validated on the Moto G100. Android 12 and custom Vulkan driver paths need device testing. The trigger captures what RenderDoc considers the next frame; it may not contain all command-buffer recordings performed previously, and can impose significant overhead or cause device loss. Prefer testing a lightweight game first.

## Recovery

Turn off GPU debug layers when finished:

```sh
adb shell settings delete global enable_gpu_debug_layers
adb shell settings delete global gpu_debug_app
adb shell settings delete global gpu_debug_layers
adb shell settings delete global gpu_debug_layer_app
```

The PR is draft and targets master, **do not merge** pending full build and device validation.
