# SWKBD — Hitman profile-name investigation

Starting HEAD: `813dbc935a96dbc7c4426cc6a999bf2041c21b27`, existing Draft PR #486, branch `fix/swkbd-lifecycle-overlay-20261124`. No new PR and no merge. The Android navigation investigation remains closed.

## Observed evidence and current status

The user reports that Hitman: Blood Money — Reprisal stays at ENTER PROFILE NAME without an Android keyboard, including before this PR. The screenshot shows that screen and the emulator's 30 FPS counter. **It does not establish the SWKBD mode/version, that the game called Start, that it sent Appear, or that a request reached Android.** No Hitman execution log or game binary is available here. Those facts remain to be measured with this diagnostic APK; no claim is made that Hitman is fixed.

The audit found and reproduced a separate, concrete protocol defect already present in the master implementation: a guest SetInputText command could update native inline text without returning its expected ChangedString callback. That defect is fixed generically. It is a possible explanation for a guest waiting before Appear, not a demonstrated diagnosis of this title.

## Creation and execution path reviewed

`ILibraryAppletCreator::CreateLibraryApplet[/Ex]` constructs `ILibraryAppletAccessor`, which routes the software-keyboard applet ID through `applet_creator.cpp` to `SoftwareKeyboardApplet`. The accessor forwards normal/interactive input storage, calls Start, exposes the output/state events and returns output storage through PopOutData/PopInteractiveOutData.

Strato selects normal for AllForeground (`0`), inline for PartialForeground (`1`) or PartialForegroundWithIndirectDisplay (`3`). Other modes are rejected. Normal consumes CommonArguments plus configuration, selects V0/V7/VB using apiVersion (`<0x30007`, `<0x6000B`, otherwise VB), bounds-checks optional initial UTF-16 storage and queues an Android open immediately. Inline consumes CommonArguments and an 8-byte InitializeArg, creates a frontend session but **does not open Android yet**.

Inline then expects Calc (`0xA`): Initialize flag `0x1` transitions Uninitialized → Hidden and sends Default plus FinishedInitialize; Appear flag `0x4` transitions Hidden → Appearing and queues Android open. FrontendOpened acknowledges actual dialog creation and transitions Appearing → Shown. Disappear `0x80`, submit and cancel hide the frontend; Finalize (`0x4`) unregisters/closes it and signals applet completion. ChangedStringV2/MovedCursorV2 commands (`0xD`/`0xE`) select reply formats.

The Android route is the existing asynchronous frontend task queue → `JvmManager::ShowSoftwareKeyboard` → `EmulationActivity.openSoftwareKeyboard` → `SoftwareKeyboardHost.open` → `DialogFragment.showNow` → focused SoftwareKeyboardEditText → IME request. An accepted JNI return only establishes admission to this route, not actual IME visibility. The new stage logs distinguish worker admission, JNI delivery, Kotlin argument/lifetime rejection, UI creation, IME request and observed IME visibility.

## Reference comparison

Immutable source references inspected:

- Ryujinx/Ryubing [`eaff2d332fb3010e2e4a43a5f3168432deae3395`](https://git.ryujinx.app/projects/Ryubing/src/commit/eaff2d332fb3010e2e4a43a5f3168432deae3395/src/Ryujinx.HLE/HOS/Applets/SoftwareKeyboard): SoftwareKeyboardApplet.cs, SoftwareKeyboardCalc.cs, SoftwareKeyboardCalcEx.cs, KeyboardCalcFlags.cs and InlineResponses.cs.
- Eden [`1cd2ecd562c5679c3220129259b64d3f38b862cf`](https://github.com/eden-emulator/mirror/blob/1cd2ecd562c5679c3220129259b64d3f38b862cf/src/core/hle/service/am/frontend/applet_software_keyboard.cpp): applet_software_keyboard.cpp, applet_software_keyboard_types.h, frontend/software_keyboard.h and the Android JNI/SoftwareKeyboard.kt/InputOverlay frontend reviewed earlier.
- libnx [`b156b4774fb2df7b93402169509266da7547fc4c`](https://github.com/switchbrew/libnx/blob/b156b4774fb2df7b93402169509266da7547fc4c/nx/include/switch/applets/swkbd.h#L289): ChangedString callback contract and swkbdInlineSetInputText; matching [swkbd.c](https://github.com/switchbrew/libnx/blob/b156b4774fb2df7b93402169509266da7547fc4c/nx/source/applets/swkbd.c) for flags/request/reply handling. No Eden code was copied.

| Protocol point | Comparison and decision |
| --- | --- |
| Normal vs inline | Eden selects by library-applet mode; Ryujinx distinguishes an 8-byte initialize block from normal configuration. Strato selects by mode and validates the corresponding block. Actual Hitman mode is not inferred from the screenshot. |
| Inline initialize | Eden and Strato transition Hidden before FinishedInitialize. Ryujinx sends FinishedInitialize using its prior state then returns Default after Calc. No speculative reordering was applied. |
| Calc layout | Strato accepts `0x4A0` old, `0x4C8` legacy extended and `0x4E8` new, requiring declared/actual size equality. Eden has old/new typed layouts, with new appear at `0x18` and cursor at `0x8C`. Ryujinx also accepts a padded alternative and reads an extended layout retaining volume/cursor before appear. A padded or differently declared packet could therefore be rejected or misinterpreted; the APK logs actual/declared size, flags, selected offsets and numeric fields. Parser rules were not changed without the game's packet. |
| Guest text changes | libnx explicitly documents ChangedString callbacks for swkbdInlineSetInputText. Eden's non-initialize SetInputText/SetCursor path calls InlineTextChanged; its Android frontend returns ChangedString. Strato previously changed native text, and only updated Android when shown; Kotlin deliberately suppresses watcher echo, so **no callback was returned**, especially while hidden. Fixed for SetInputText, with the configured encoding/V2 format, without opening a hidden editor. Cursor-only guest semantics were not broadened by assumption. |
| Opening flags | Ryujinx's MustShow also includes SetInputText/AppearTrigger. Strato and Eden use explicit Appear for showing. Copying the broader predicate would force UI while a guest is preparing hidden text. This was not adopted; the missing text callback is acknowledged independently. |
| Default acknowledgement | Ryujinx responds Default after Calc; Eden responds through state changes/other replies. Strato has no universal Default for otherwise inert Calc. A discrepancy alone is insufficient to prove a missing hardware-required acknowledgement, so no blanket reply was added. |
| Finalize/events | Strato signals applet completion, closes the native/frontend session and returns to Uninitialized. Actual IApplet output producers enqueue then signal; pops clear the corresponding event only when its queue becomes empty. These functions are exercised in the new applet tests. |
| Concurrency | Existing native state/queue locks, released registry lock before callbacks, copied JNI buffers/strings and asynchronous worker/UI dispatch remain. The diagnostic gate releases its own lock before logging or entering applet state locks. No added wait, delay or insets-driven completion was introduced. |

## Confirmed correction and regression

For an initialized inline session, a non-initialize Calc carrying SetInputText now sends exactly one ChangedString / ChangedStringV2 / ChangedStringUtf8 / ChangedStringUtf8V2 reply using native text and cursor. It works while Hidden, Appearing or Shown. A simultaneous Appear still opens through the ordinary path; SetInputText alone does not force opening. Initialization with SetInputText does not gain an extra callback. The existing programmatic Android update suppression remains, preventing an echo/duplicate reply from its text watcher.

Before the fix, the new production-applet regression failed at `reply must signal interactive event` after a hidden SetInputText. After the fix, it passes across both inline modes, all three layouts, both encodings and both callback versions, including Unicode payload/UTF-16 cursor assertions, hidden updates, combined SetInputText+Appear and shown updates.

No title ID, game name, forced profile value, timer, fake confirmation, parser fallback or keyboard-opening workaround was added. Dark Souls' actual game execution was not run here; both normal and inline paths are retained and tested. Touch controls, their preferences, physical dispatch and navigation handling are unchanged from `813dbc93`.

## Temporary diagnostics intentionally retained for collection

All stages use `[SWKBD]` in the ordinary shareable emulation log at Info level. No ADB is needed for the initial collection. Logs include:

- Creation/mode; queued common version/declared size/API version; Start; normal configuration family/size, limits, encoding, text-check and initial-text length.
- Inline InitializeArg raw numeric bytes; interactive command/storage size and transfer-memory kind before the state lock; Calc declared/actual size, flags, state, initialization bytes, selected text/cursor/encoding offsets, input length/cursor and normalized appear limits.
- State transitions; output reply code/state/size and producer event-signaling point; Finalize/completion; native/frontend session correlation and frontend events.
- Native open queued/worker entered/accepted/skipped/exception; Kotlin/UI stage names, rejection reasons, dialog start/dismiss and close; IME request (`detail=1` means editor has focus) and IME visibility changes (`1` visible, `0` hidden).

Repeated command/Calc/reply signatures are deduplicated by a thread-safe per-applet gate with 64 distinct detail slots and one limit notification. Lifecycle/open-close transitions remain logged. The existing pre-draw observer only logs IME visibility transitions; it never cancels/submits/reopens based on insets. No new polling timer or per-frame log was added. Text contents, profile names and text-check messages are not dumped by this instrumentation; only lengths/numeric protocol fields are recorded. JNI exception details still use the existing ExceptionDescribe/logcat path, with an exception marker also written to the shareable log.

These diagnostics are deliberately temporary and must be removed after the Hitman trace establishes the remaining cause. The PR remains Draft for this follow-up; the earlier integration-ready assessment described the pre-instrumentation scope.

## Moto G100 collection

1. Install the APK from **this new commit** in PR #486. The workflow artifact is `strato-pr486-arm64-release.apk`; previous commit artifacts lack these diagnostics/fix. Keep Log Level at **Info** in Strato's settings (not Warning/Disabled). Debug is unnecessary for the first run.
2. Start a fresh Hitman emulation session, enter Profile Manager → New → ENTER PROFILE NAME, and wait about five seconds. If the keyboard stays absent, tap the white input field once and try A once, then wait another five seconds. Note which action, if any, makes it appear. If it appears, test typing and confirmation/back once.
3. Exit emulation normally. On Strato's main screen use **Share Logs** and save/send the full `emulation.log`, named `Hitman-SWKBD.log` if convenient. **Share it before starting another game**, since the next emulation run replaces the log.
4. For a regression/control comparison, run Dark Souls separately, open name input, type, confirm or cancel and reopen; share its log immediately as a separate `DarkSouls-SWKBD.log`. Do not mix the two runs into one overwritten log.

Interpretation guide:

| Last relevant evidence | What it narrows down |
| --- | --- |
| No `[SWKBD] create` | No SWKBD construction was observed; this does not prove a JNI bug or identify another input mechanism. Check that the correct APK/log was used and that the field was actually activated. |
| Create/common input but no Start | Failure/wait occurs before applet execution. |
| Inline initialized, then SetInputText/ChangedString, no Appear | Guest-side progress/opening command must be examined; Android has not yet been requested. |
| Rejected Calc with sizes/flags | Need the captured ABI variant; do not guess a layout or force opening. |
| Appear/state=2, no open-worker entry | Frontend queue/session path. |
| JNI accepted but UI rejected/missing | Activity/FragmentManager/argument delivery path. |
| showNow/FrontendOpened and IME requested, visibility remains 0 | Android focus/IME activation path, distinct from missing guest commands. A request is not proof of a visible IME. |

The trace records producer event calls; it does not itself prove guest event consumption/wakeup. That would require a subsequent focused trace if the first log reaches this boundary without explaining the wait. Mode/API/Calc version used by **Hitman remain unknown until its log is supplied**.

## Validation

**Final local results: 15 API 31 UI tests and 9 API 29 tests passed; the extended native suite passed normally and with ASan/UBSan.** `git diff --check` passed. The pre-fix regression failure and post-fix reply assertions were observed locally. The existing UI suite now also checks diagnostic delivery and rejected-open stages without extra guest terminal events. The native suite now compiles **SoftwareKeyboardApplet.cpp and the actual IApplet.cpp queues**, replacing only kernel/storage/JVM/logger boundary dependencies with host fixtures; this is not a boot of Android, Horizon or Hitman.

The native cases cover six inline mode/layout combinations through initialization, appear/open acknowledgement, cursor V2, explicit disappear, submit, cancel, reopen and finalize; 24 guest text-reply sequences; malformed/truncated/unsupported Calc followed by valid recovery; three normal submit/cancel/text-check flows; and the existing 30 reply wire-format cases. The diagnostic gate is checked against 10,000 repeated signatures, its detail cap and concurrent duplicate calls. A pre-existing unreachable switch-enum warning is made explicit; unchanged IApplet constructor warnings are disabled only for its host-test object.

Commands: `bash tests/swkbd/run.sh`, the same harness with ASan/UBSan, `./gradlew -p tests/swkbd/ui testDebugUnitTest --no-daemon`, and `./gradlew -p tests/swkbd/android test --no-daemon --rerun-tasks`. Leak detection is disabled under the managed host's ptrace restriction, so no leak-check claim is made.

The push triggers the existing ARM64 DevRelease PR workflow. As requested, the APK build will not be monitored or described as completed without evidence.
