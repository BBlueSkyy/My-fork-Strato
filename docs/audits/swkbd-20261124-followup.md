# PR #486 follow-up — visible controls during SWKBD

> Scope correction: the user meant Android system navigation, not the Strato touch overlay. Touch-suppression behavior described below is historical and has been removed. See [the current navigation-bar audit](swkbd-20261124-navigation.md). Other SWKBD correctness fixes are retained.

Starting point: `f82a0ec550cc1f1c4b63cb6966a010efb96f2744`, on the existing `fix/swkbd-lifecycle-overlay-20261124` branch. No new PR or merge.

The user tested the previous APK on a Moto G100 and reported that Dark Souls Remastered still displayed the virtual controls. The earlier helper-only tests and successful build did not establish correctness of the complete Android keyboard flow.

## Actual overlay and Android entrypoint

`EmulationActivity` inflates `EmuActivityBinding` once and installs `binding.root` with `setContentView`. The actual control renderer is `binding.onScreenControllerView`, an `OnScreenControllerView` from `emu_activity.xml`. The other instance in the repository belongs to the separate controller editor Activity. There is no second emulation controller overlay to target.

The native frontend calls `openSoftwareKeyboard(J, ByteBuffer, String, boolean)` on the global reference to that same emulation Activity. The JNI name, signature, argument decoding, normal/inline mode flag, asynchronous native worker and native protocol remain intact. The Activity's existing SWKBD methods now delegate their session/window ownership to `SoftwareKeyboardHost`; this extracts the previous Android-only methods so the very same opening/closing code can run in framework tests without booting emulation or mocking the keyboard dialog.

The UI tests pass the actual serialized `0x4C8` configuration through this host entrypoint, receive `FrontendOpened`, assert normal versus inline window flags, and confirm that `findViewById` returns the same controller instance as the binding. This validates the Android boundary and displayed instance. No JNI/Moto G100 trace was obtained from the user's run.

## Confirmed failures in the old flow

### Insets incorrectly ended a session

The old `SoftwareKeyboardDialog.onStart` installed an IME visibility listener. Once IME-visible had been observed, a focused/resumed visible-to-hidden transition called `cancelFromUser`, regardless of whether the user pressed Back. Native normal and inline cancellation then closed/hid the frontend, causing the Activity to restore the overlay. Window insets describe window/IME geometry; they are not an applet completion event.

Reproduction used the old dialog and old Android session logic (mechanically extracted into the testable host), the production layout, actual AndroidX DialogFragment and Android API 31/Skia rendering. After opening the keyboard, the test delivered visible then hidden IME insets without a Back event. Both independent regressions failed:

- `imeHidingDoesNotEndNormal`: `IME geometry must not terminate SWKBD (inline=false)`.
- `imeHidingDoesNotEndInline`: `IME geometry must not terminate SWKBD (inline=true)`.

The ordinary open/draw/confirm and replacement/background tests passed on that same old code, explaining why a simpler visibility test missed the problem. The reproduced premature cancellation is consistent with early restoration; it does not prove that this was the exact sequence emitted by Gboard on the user's device, or the only contributing cause there.

The listener and its visibility heuristic were removed. An actual Back key is handled by `SoftwareKeyboardEditText.onKeyPreIme`, with tracking and cancellation checks, and forwarded through the existing cancellation callback. A cancel-disabled keyboard consumes Back without ending its session. Merely hiding/replacing the IME, changing focus, pausing or resuming no longer ends SWKBD.

### A visible restored dialog did not own suppression

Previously, only the JNI opening method populated the Activity's map. FragmentManager can show an existing/restored dialog through its lifecycle without calling that JNI entrypoint again. Such a visible dialog could have no suppression owner.

The old code also fails `visibleFragmentWithoutJniReopenStillOwnsTheOverlay`: the dialog is shown but the host is not active.

`SoftwareKeyboardDialog.onStart` now reports its actual displayed instance to its Activity/host. That host suppresses the real overlay even when the JNI opening method was not reentered. An old restored instance cannot supersede another active session. This is a UI ownership fix, not support for preserving the native emulator across process death.

### Suppression covered children, not their displayed parent

The old helper set three individual views to GONE. Any later child visibility assignment could make that child draw again. The static writer audit found initialization/user-toggle assignments and SWKBD/PiP restoration; it did not identify an independent periodic writer repeatedly resetting the emulation controls during an active session.

`emu_activity.xml` now has a dedicated `on_screen_controls_container` containing the real controller, pause button and controller toggle. The same suppression snapshot covers this parent and its children. A child changed to VISIBLE cannot render or receive controller touches through a hidden parent. On completion, all four visibility values are restored, preserving the user's previous choices. Performance/pipeline overlays remain outside this container. No title-specific condition, frame-by-frame hiding loop or visibility polling was added.

## Session lifetime and retained behavior

- The host retains ownership until the actual dialog dismissal callback. Native close/hide requests no longer remove the owner before dismissing the window.
- Opening, replacing and dismissing use the displayed dialog identity, preserving same-ID inline replacement and rejecting stale callbacks.
- Activity touch dispatch still consumes touches while the host has an active session. Pressed virtual buttons/sticks and guest touchscreen contacts are released through the previous PR paths when suppression starts.
- Both normal and inline use the same ownership/visibility gate. Inline retains its non-touchable transparent window and active IME connection; normal retains its text/confirmation UI.
- The previous character-flag, Unicode-span, length, cursor, inline reply, text-check and resource cleanup fixes are preserved. Physical key/joystick dispatch is unchanged.
- No temporary production logging or instrumentation remains. Tests record outgoing frontend events and render bitmaps in memory.

## Validation and limits

Commands:

```sh
./gradlew -p tests/swkbd/ui testDebugUnitTest --no-daemon
./gradlew -p tests/swkbd/android test --no-daemon
bash tests/swkbd/run.sh
```

The new API 31 UI suite uses production `SoftwareKeyboardHost`, `SoftwareKeyboardDialog`, `SoftwareKeyboardEditText`, `OnScreenControllerView`, generated bindings, emulation/keyboard XML and controller drawables. A minimal AppCompat Activity hosts them; it records/replies to native completion events instead of starting the native emulator. Real platform/AndroidX behavior and native Skia drawing are used, rather than fake View/binding classes.

The ten UI tests cover normal/inline opening, `FrontendOpened`, correct bound renderer, drawn controller pixels disappearing/reappearing, unexpected IME transitions in each mode, real pre-IME Back and disabled cancellation, same-ID replacement, background/foreground, visible dialogs without JNI reopen, stale restored dialogs, child visibility interference, controller touch input before/during/after the session, three consecutive sessions, user visibility restoration, failed opening after state save, and text-check focus/retry. Existing helper tests also cover both PiP-close orders.

The UI test viewport is 1280×720 landscape on API 31. An earlier touch-fixture attempt used the default 320-wide test window, so its synthetic tap did not reach the controller; setting the window's landscape dimensions fixed the positive control. No production change was made to satisfy that fixture issue.

Local results: all 10 API 31 UI tests, all 9 existing API 29 helper tests, and the native SWKBD suite (including 30 inline wire-format cases) passed. The three regressions (normal IME transition, inline IME transition and a visible dialog without JNI reopen) fail with the old logic and pass afterward. These tests go beyond compilation, but they do not run Dark Souls Remastered, the Moto G100's Gboard, actual JNI applet transactions or a hardware gamepad. The complete native EmulationActivity is not booted by the fixture. Physical-device retesting is still required before declaring the reported symptom resolved.

The PR workflow now runs the API 31 UI suite before assembling the new ARM64 DevRelease APK. Publishing the follow-up commit requests a fresh APK through that workflow. As requested, the APK build will not be monitored; the previous artifact for `f82a0ec5` is not the new validation APK.
