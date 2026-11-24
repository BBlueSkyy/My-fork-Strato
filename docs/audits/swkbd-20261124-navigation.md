# PR #486 — Android navigation bar, not Strato touch controls

Starting commit: `124125a0d2c5dd25a37907ad38337603ad48ca8d`. Same Draft PR and branch; no merge.

The user clarified that the highlighted buttons are Android's three-button system navigation bar. The intended behavior is to retain Strato's touch controls and request hidden Android navigation during SWKBD. The earlier overlay-hiding requirements were a misunderstanding and are superseded by this correction.

## Selective reversal

- Removed all SWKBD-to-touch-visibility coupling from `SoftwareKeyboardHost`. Opening, resuming, restoring, closing or cancelling a keyboard does not change controller/pause/toggle visibility.
- Removed the Activity-wide SWKBD touch interceptor and the SWKBD condition in `onTouch`, restoring the previous game/controller input path. Inline retains its original non-touchable transparent window. Normal retains standard modal-dialog input behavior; its visible backdrop controls are not forcibly made non-modal.
- Restored `emu_activity.xml` exactly to the master baseline `e8e38237`, removing the dedicated suppression container.
- Kept PiP suppression and its user-visibility snapshot as a separate `OnScreenControlsVisibility` helper under `input/onscreen`. Only PiP callbacks invoke it. Keyboard sessions do not release held virtual inputs or clear guest touchscreen contacts; the existing PiP path can still do so.
- Kept the fix preventing disabled controls from being made visible during initial physical-controller detection.

All other SWKBD corrections remain: invalid-character flags, Unicode/composition spans, byte/unit limits, filtered cursor bounds, inline MovedCursor/V2 replies, native sessions and serialization, actual Back handling, stale/restored dialog identity, view binding/alert ownership, text-check retry, and rejection of cancellation inferred solely from IME insets. Physical key/joystick dispatch is unchanged.

## Why the system bar reappears

There are two separate issues:

1. The Activity configures its own immersive window. Opening `SoftwareKeyboardDialog` gives a different focusable window control of the UI. The dialog previously set IME/inline flags but did not request hidden navigation itself. Activity-only system-UI flags are not a substitute for configuring the focused dialog.
2. Android may override an application's request while the IME is visible. This is a platform-policy limitation, not a remaining touch-overlay defect.

The exact AOSP Android 12 release source was inspected, not inferred from a newer Android branch:

- [Android 12 r1 InsetsPolicy.java](https://android.googlesource.com/platform/frameworks/base/+/android-12.0.0_r1/services/core/java/com/android/server/wm/InsetsPolicy.java), `getNavControlTarget` (around lines 355–361): a visible IME causes a null navigation control target, with an explicit policy comment about forcing navigation visible.
- The same file's `BarWindow.updateVisibility` (around lines 468–470) treats a null control target as visible. `adjustVisibilityForIme` also makes navigation insets visible to the IME.
- [Public WindowInsetsController.hide API](https://developer.android.com/reference/android/view/WindowInsetsController#hide(int)) documents that a hide request waits until the window has control of the corresponding type.
- [Official immersive-mode guidance](https://developer.android.com/develop/ui/views/layout/immersive) documents the navigation-only mask and transient swipe behavior.

The Android 12 r1 code does not use the `config_hideNavBarForKeyboard` conditional found in newer policy implementations. That newer implementation must not be presented as the Android 12 release behavior. Motorola/IME firmware can differ from AOSP; its exact policy was not read from the Moto G100. Three-button navigation while Gboard is visible therefore cannot be guaranteed hidden by a normal application's public API.

## Public-API change

`SoftwareKeyboardWindow.hideNavigationBar` applies only `WindowInsetsCompat.Type.navigationBars()` through `WindowInsetsControllerCompat`, with `BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE`.

The request is made to the SWKBD dialog's own window on start/resume, text-check retry and actual window-focus gain. Text-check alerts get the same navigation request on their own windows. The dialog uses the same AndroidX `ComponentDialog` base as DialogFragment's default implementation, adding only the focus callback.

The code does not hide IME, change keyboard layout/extraction settings, set NOT_FOCUSABLE or ALT_FOCUSABLE_IM, or change decor fitting to disguise the bar. It does not poll insets, restart the IME, repeatedly fight a visible-bar callback, crop the window, cover system buttons, change system settings/navigation mode, use accessibility/overlay permissions, invoke shell/root commands, or call hidden Android APIs. System gestures can still reveal transient bars as intended by Android.

On firmware that permits the focused window to hide navigation, the public request can take effect. On policy-forced firmware it may remain pending and the bar can stay visible until the IME closes. This limitation is accepted rather than bypassed.

## Validation

```sh
./gradlew -p tests/swkbd/ui testDebugUnitTest --no-daemon
./gradlew -p tests/swkbd/android test --no-daemon
bash tests/swkbd/run.sh
```

Local result: all 14 API 31 UI tests, all 9 existing API 29 tests and the native SWKBD suite (including 30 inline reply-format cases) passed. The AAR test harness now copies extracted jars with writable permissions so repeat runs do not fail on read-only archive entries.

The UI suite uses API 31, production keyboard/session/controller code, actual XML/bindings, and native Skia drawing. Its navigation test checks the framework resource for navigation mode `0` (three buttons). Updated cases assert that drawn touch controls remain visible during both modes, replacement, background/foreground, text-check and confirmation/cancellation; explicit user-hidden states stay hidden. An actual virtual-button tap reaches the controller before, during and after inline SWKBD.

Navigation-specific cases verify the request targets the dialog, uses transient swipe behavior, leaves status-bar requests unchanged, preserves editability/focus and compatible window flags, reapplies on real focus regain, and tolerates a system-owned visible navigation bar without cancelling the keyboard or starting a retry loop. Previous Unicode/flag/limit/PiP/native regression tests remain.

Robolectric records client hide requests; it does not run Android's system_server navigation policy or Gboard. Its simulated hidden-bar result is not evidence that the Moto G100 can override the platform restriction. Private reflection in tests only reads Robolectric's bookkeeping or prepares fixture focus; no such access is part of production code.

There is no attached Moto G100 or hardware gamepad. On-device validation must check the new commit's APK in normal and inline games with three-button navigation, including text input, Back, repeated opens and returning to gameplay. If navigation remains visible only while the IME is open, that is consistent with the documented platform restriction and is not a reason to introduce an invasive workaround.

The new commit triggers the existing ARM64 DevRelease PR workflow. As requested, the APK build will not be monitored or reported complete without evidence. No temporary production instrumentation is retained.
