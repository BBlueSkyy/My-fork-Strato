# SWKBD navigation: Eden comparison and unresolved device discrepancy

Starting Strato HEAD: `b58e6874c71e7520ed9597a02ffe4145e7a2eb65`, existing Draft PR #486, branch `fix/swkbd-lifecycle-overlay-20261124`. No merge or replacement PR.

**The Moto G100 issue is not resolved by this audit.** The user reports that Eden keeps Android navigation hidden during text entry. This observation must not be dismissed as a universal Android limitation, or presented as reproduced here. There is no attached Moto G100, matching Eden APK, or trace of its focused/IME windows. This revision changes tests and documentation only; its APK has the same production SWKBD implementation as `b58e6874`.

## Reference identity

Inspected the official Eden mirror at immutable commit [`1cd2ecd562c5679c3220129259b64d3f38b862cf`](https://github.com/eden-emulator/mirror/commit/1cd2ecd562c5679c3220129259b64d3f38b862cf). The version/build installed on the user's phone is unknown, so equivalence to that APK is not established. No Eden code was transplanted into Strato.

Paths below are relative to that Eden commit:

| Reference | Relevant behavior |
| --- | --- |
| [`EmulationActivity.kt`](https://github.com/eden-emulator/mirror/blob/1cd2ecd562c5679c3220129259b64d3f38b862cf/src/android/app/src/main/java/org/yuzu/yuzu_emu/activities/EmulationActivity.kt#L559) | `enableFullscreenImmersive`: disables decor fitting, sets six public legacy fullscreen/layout/immersive flags, then calls Compat hide(systemBars) with transient swipe behavior. No API-level guard around the legacy flags. |
| [`SoftwareKeyboard.kt`](https://github.com/eden-emulator/mirror/blob/1cd2ecd562c5679c3220129259b64d3f38b862cf/src/android/app/src/main/java/org/yuzu/yuzu_emu/applets/keyboard/SoftwareKeyboard.kt#L29) | Normal opens a DialogFragment. Inline focuses the existing Activity overlay, calls IMM.restartInput and showSoftInput(SHOW_FORCED), then polls IME visibility to submit when hidden. |
| [`InputOverlay.kt`](https://github.com/eden-emulator/mirror/blob/1cd2ecd562c5679c3220129259b64d3f38b862cf/src/android/app/src/main/java/org/yuzu/yuzu_emu/overlay/InputOverlay.kt#L93) | Inline editor lives in the Activity window; custom BaseInputConnection forwards text/delete/actions to native code. Uses NO_EXTRACT_UI and ACTION_DONE, without NO_FULLSCREEN. |
| [`KeyboardDialogFragment.kt`](https://github.com/eden-emulator/mirror/blob/1cd2ecd562c5679c3220129259b64d3f38b862cf/src/android/app/src/main/java/org/yuzu/yuzu_emu/applets/keyboard/ui/KeyboardDialogFragment.kt) | Normal uses MaterialAlertDialogBuilder with an editable view and positive/negative buttons. No keyboard-specific hide-navigation call, focus callback, fullscreen Window flag, or decor-fitting override. |
| [`dialog_edit_text.xml`](https://github.com/eden-emulator/mirror/blob/1cd2ecd562c5679c3220129259b64d3f38b862cf/src/android/app/src/main/res/layout/dialog_edit_text.xml) / [`fragment_emulation.xml`](https://github.com/eden-emulator/mirror/blob/1cd2ecd562c5679c3220129259b64d3f38b862cf/src/android/app/src/main/res/layout/fragment_emulation.xml) | Normal TextInputEditText has no explicit IME options; inline overlay is focusable in touch mode. |
| [`themes.xml`](https://github.com/eden-emulator/mirror/blob/1cd2ecd562c5679c3220129259b64d3f38b862cf/src/android/app/src/main/res/values/themes.xml) / [`styles.xml`](https://github.com/eden-emulator/mirror/blob/1cd2ecd562c5679c3220129259b64d3f38b862cf/src/android/app/src/main/res/values/styles.xml) / [`AndroidManifest.xml`](https://github.com/eden-emulator/mirror/blob/1cd2ecd562c5679c3220129259b64d3f38b862cf/src/android/app/src/main/AndroidManifest.xml) | Material3 activity, transparent system-bar colors and disabled contrast enforcement. Emulation Activity has no explicit soft-input-mode override. Transparency/contrast do not themselves hide navigation buttons. |
| [`FullscreenHelper.kt`](https://github.com/eden-emulator/mirror/blob/1cd2ecd562c5679c3220129259b64d3f38b862cf/src/android/app/src/main/java/org/yuzu/yuzu_emu/utils/FullscreenHelper.kt) | Ordinary Compat systemBars request; keyboard dialog does not call this helper. |
| [`software_keyboard.cpp`](https://github.com/eden-emulator/mirror/blob/1cd2ecd562c5679c3220129259b64d3f38b862cf/src/android/app/src/main/jni/applets/software_keyboard.cpp#L139) | HLE native normal and inline routes call the Java methods above. Several inline operations are marked stubbed; this is not a complete protocol implementation to substitute for Strato's. |
| [`library_applet_creator.cpp`](https://github.com/eden-emulator/mirror/blob/1cd2ecd562c5679c3220129259b64d3f38b862cf/src/core/hle/service/am/service/library_applet_creator.cpp#L26) / [`settings.h`](https://github.com/eden-emulator/mirror/blob/1cd2ecd562c5679c3220129259b64d3f38b862cf/src/common/settings.h#L153) | HLE is default, but `swkbd_applet_mode=LLE` tries to create a guest firmware applet, falling back to frontend if unavailable. The UI exposes Custom Frontend / Real Applet in SettingsItem.kt and arrays.xml. This is a different path, potentially without any Android IME window. The user's selected path is unknown. |

The global controller navigation hook changes controller button dispatch; it does not configure system-bar visibility. No hidden navigation API or privileged keyboard-window operation was found in the inspected SWKBD paths.

## Window and input comparison

| Concern | Strato at b58e6874 | Eden reference |
| --- | --- | --- |
| Activity immersive setup on API 31 | InsetsController systemBars request on creation; legacy mask only through API 30 on resume | Legacy mask plus Compat systemBars on creation |
| Normal input owner | Separate ComponentDialog window containing SoftwareKeyboardEditText | Separate Material AlertDialog window containing TextInputEditText |
| Inline input owner | Separate transparent, non-touchable **focusable** ComponentDialog window | Existing Activity overlay / Activity window |
| Request target during SWKBD | Own focused dialog window, including real focus regain and text-check windows | Activity; no extra normal-dialog navigation request |
| IME show | Soft-input window state plus Compat show(ime) with focused editor | Inline IMM restartInput / SHOW_FORCED; normal standard editable dialog |
| Landscape IME options | NO_EXTRACT_UI **and NO_FULLSCREEN**, both modes | Inline NO_EXTRACT_UI only; normal no explicit extraction/fullscreen flags |
| Insets close heuristic | None; real Back and native session callbacks | Inline polls every 500 ms and submits once IME is hidden |
| Touch controls | Preferences preserved; inline window passes touch through, normal remains modal | Activity overlay also serves as inline editor |

A search of Strato's system-UI setters found no SWKBD/EmulationActivity routine explicitly showing navigation again: the Activity hides system bars, the keyboard hides navigation and only shows the IME type, and WindowInsetsHelper reads insets for padding/margins. Dialog resume, focus regain and text-check retry request navigation hiding. The existing tests exercise those paths and background/foreground without treating changed insets as completion. Eden's EmulationFragment inset listeners similarly apply cutout/menu padding; they do not grant control of navigation.

These are source-verified differences, **not experimentally isolated causes** of the reported Motorola behavior. In particular, moving Strato inline into the Activity would require changing Back routing, Activity keyboard dispatch, editor attachment/restoration and shared-window IME cleanup. Doing that solely because Eden uses it would be an unproven architectural change. It also cannot explain Eden's normal-dialog behavior by itself.

Copying Eden's inline connection would lose Strato's existing rich editor/filter/cursor and protocol behavior. Copying its polling would reintroduce the known false-completion problem from IME transitions. SHOW_FORCED affects IME lifetime, not ownership of navigation control; official Android guidance discourages it because a keyboard may remain open after leaving the application. None was adopted.

## What explains the previous unsuccessful request, and what remains unknown

A client navigation hide request is not proof that the system granted navigation control. The previous tests established that Strato requested hiding the correct window's navigation; they did not establish that navigation disappeared on the Moto G100. The device report demonstrates that the previous change was insufficient there. It does not, by itself, reveal the Eden-side mechanism.

Checked AOSP **Android 12 r34**, not a current Android branch:

- [`InsetsPolicy.getNavControlTarget`](https://android.googlesource.com/platform/frameworks/base/+/android-12.0.0_r34/services/core/java/com/android/server/wm/InsetsPolicy.java#353) returns null whenever the IME window is visible. `BarWindow.updateVisibility` treats a null target as visible. This is unconditional with respect to Activity versus Dialog and legacy versus InsetsController request in this release. It establishes an AOSP constraint, **not the exact Motorola firmware policy**.
- [`ViewRootImpl.controlInsetsForCompatibility`](https://android.googlesource.com/platform/frameworks/base/+/android-12.0.0_r34/core/java/android/view/ViewRootImpl.java#2319) translates legacy HIDE_NAVIGATION/FULLSCREEN bits to the same InsetsController hide API. Thus the missing legacy mask on Strato API 31 is not evidence of a separate policy bypass in Eden. Blindly adding those bits would repeat the previous mistake.
- [`InputMethodService.onEvaluateFullscreenMode`](https://android.googlesource.com/platform/frameworks/base/+/android-12.0.0_r34/core/java/android/inputmethodservice/InputMethodService.java#1747) considers NO_FULLSCREEN in landscape. NO_EXTRACT_UI separately controls extracted-text visibility. These differences can change IME presentation, but this policy source does not make them exceptions to the visible-IME navigation rule. Removing them would change text presentation without proving a fix.
- [WindowInsetsController.hide](https://developer.android.com/reference/android/view/WindowInsetsController#hide(int)) documents that a request waits for control; [IME visibility guidance](https://developer.android.com/develop/ui/views/touch-and-input/keyboard-input/visibility) documents focused-input requirements and the SHOW_FORCED caveat.

The earlier navigation report overreached when it treated a future remaining visible bar as sufficient to attribute the device result to platform policy. **That causal attribution is withdrawn.** The current source does not precisely explain why the user's Eden APK hides navigation, and no confirmed navigation fix is claimed.

To resolve that discrepancy, the minimal missing evidence is the Eden version/build, its Software Keyboard setting (Custom Frontend versus Real Applet), and whether Android's same keyboard app actually appears. A matched device comparison must then capture the focused window, IME target and IME/nav visibility in both apps during entry, with identical navigation mode and IME settings. Those observations distinguish a guest keyboard, an IME presentation difference, a different Eden source revision and a Motorola window-policy difference. None is assumed to be the user's case.

## Changes and validation

Production code, touch visibility, layouts, existing native protocol fixes and lifecycle handling are unchanged. No speculative flags, fullscreen/extraction change, Activity-editor migration, instrumentation, private API or workaround was added.

Added two API 31 regression tests, each run in normal and inline modes, against production editor/host code:

- Use the actual InputConnection to replace text through composition, finish composition, commit accented Unicode plus a supplementary character, move a UTF-16 cursor and submit; verify native event text/cursor payloads.
- Replace a session with the same ID, reject events from the stale dialog, then edit and submit through the current editor's InputConnection.

Existing tests still cover drawn/touchable virtual controls, user-hidden preferences, Back/cancel-disabled, replacement/background, restored dialog identity, text-check retry, IME inset transitions, focus, navigation requests, Unicode limits/spans, PiP and native replies.

Commands:

```sh
./gradlew -p tests/swkbd/ui testDebugUnitTest --no-daemon
./gradlew -p tests/swkbd/android test --no-daemon
bash tests/swkbd/run.sh
```

Result: all **16 API 31 UI tests**, all **9 API 29 tests** and the **native SWKBD suite** (including 30 inline reply-format cases) passed in this revision. `git diff --check` passed, and the production tree/layouts/workflow were verified unchanged against `b58e6874`. Robolectric does not execute system_server or Gboard; no passing test is described as an on-device navigation fix. A push triggers the existing ARM64 DevRelease workflow; it will not be monitored. The rebuilt APK is for this audited revision, **not a new proven navigation fix**.
