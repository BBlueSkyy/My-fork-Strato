# Hitman inline SWKBD: V2 prefix follow-up

Parent: `4ae01ad3045f11fb3937222910ec1e565aa1a365`. Existing Draft PR #486, branch `fix/swkbd-lifecycle-overlay-20261124`. No merge, new PR, title-specific behavior, touch-control change or Android navigation work.

## What the device log establishes

The supplied `emulation.log` has 205 lines, SHA-256 `aca6bdafc9e9dc3a2ea1315c5d0ea2f0efea6fd7dfaf689fc52307a8c27db573`. It identifies title `010083A018262000`, display version `1.0_68328`, mode `3` (PartialForegroundWithIndirectDisplay), CommonArguments version `1` / size `0x20`, API `0x8000D`, and an eight-byte InitializeArg `0x0000010000000000`. The final records are the pre-state-lock admissions of five-byte commands `0xD` and `0xE`, both on HOS-11. No Calc or Android open is recorded.

**The reason for the missing post-lock messages is a confirmed instrumentation gap:** the existing D/E cases only assign their flags and return, without tracing lock acquisition, command processing or handler return. These commands do not send replies or change state, so the existing reply/state traces also stay silent. This silence cannot establish a deadlock. The subsequent E admission on the same HOS thread supports D having returned; it does not prove E's completion. The actual cause of Hitman's missing first Calc remains unconfirmed.

The log did not record either one-byte payload. Five-byte packet size alone does not mean the flag is true. D/E select legacy versus V2 callback formats, not initialization or opening. The initialize block alone creates an inline frontend session; it is not Calc Initialize and cannot justify FinishedInitialize or an Android open.

## Protocol and lock audit

`PushInteractiveInData` resolves the input storage and synchronously calls `PushInteractiveDataToApplet`. For ordinary five-byte storage, the latter acquires `inlineMutex`, reads the four-byte command, reads the following **single byte** if present, and uses `!= 0` for the negotiated flag. D/E do not enter the transfer-memory dictionary early-return path. Truncated packets retain the previous flag. Neither branch sends an ACK, Default, FinishedInitialize or a frontend action.

The state mutex is released before executing a frontend action. On this prefix there is no such action and no JNI call. Other state-lock users reviewed include indirect-image retrieval, frontend callbacks, session closure and Calc. The indirect registry releases its mutex before calling the applet. The diagnostic gate releases its mutex before logging or acquiring applet/kernel/queue locks. No recursive acquisition or inverted lock order was found in the D/E path. The replay passes on the parent implementation; no protocol change or deadlock fix is justified by this evidence.

Output production retains the existing queue-then-signal behavior. Initialize Calc changes Uninitialized to Hidden, queues Default and FinishedInitialize, and signals interactive output. Draining one of multiple replies preserves the event; draining the last clears it. Appear then transitions to Appearing and requests the frontend; FrontendOpened acknowledges the UI and transitions to Shown. A combined Initialize/Appear Calc also works. Finalize signals completion. No unsolicited reply was added before Calc.

### Primary references

| Reference | Relevant behavior |
| --- | --- |
| [Switchbrew Software Keyboard](https://switchbrew.org/wiki/Software_Keyboard), Request / Runtime / Reply sections, retrieved 2026-11-24 through its MediaWiki parse API | D/E have one-byte boolean payloads selecting the reply format. Runtime sends Calc when pending flags exist, then consumes replies. No D/E acknowledgement is specified. |
| [libnx swkbd.c, `b156b4774fb2df7b93402169509266da7547fc4c`](https://github.com/switchbrew/libnx/blob/b156b4774fb2df7b93402169509266da7547fc4c/nx/source/applets/swkbd.c), `_swkbdSendRequestV2Flag`, `swkbdInlineLaunch`, `swkbdInlineUpdate` | For API at least `0x8000D`, sends a raw byte for D/E. Launch leaves Initialize pending; update sends Calc before popping replies. It does not wait for an ACK to the flags. |
| [Eden applet_software_keyboard.cpp, `1cd2ecd562c5679c3220129259b64d3f38b862cf`](https://github.com/eden-emulator/mirror/blob/1cd2ecd562c5679c3220129259b64d3f38b862cf/src/core/hle/service/am/frontend/applet_software_keyboard.cpp) | D/E require a five-byte packet and copy one boolean into callback-format flags, without opening or replying. Initialize is handled by Calc. |
| [Ryujinx/Ryubing SoftwareKeyboardApplet.cs, `eaff2d332fb3010e2e4a43a5f3168432deae3395`](https://git.ryujinx.app/projects/Ryubing/src/commit/eaff2d332fb3010e2e4a43a5f3168432deae3395/src/Ryujinx.HLE/HOS/Applets/SoftwareKeyboard/SoftwareKeyboardApplet.cs) | The UseChangedStringV2 / UseMovedCursorV2 cases are stubs and do not reply. This revision is not evidence of fully implemented V2 negotiation. Its broader opening predicate and Default policy were not copied. |

## Additional evidence from the supplied ExeFS reference

The previously supplied `Hitman_Blood_Money_Reprisal_ExeFS.zip` was inspected read-only (SHA-256 `01f0efbf082ed428028abfeac0385689311359e9294e748f8b8c7bfd9117fe27`). Its metadata identifies this title and main Build ID `688C23D524730AB8DBA9359C6AD5106BD33349E`, SDK Build ID `8618895246B420329B77746B960A66ABCBEDE992` (trailing zero padding omitted). Compressed NSO segments, dynamic symbols, relocations and PLT calls were decoded locally. No game files, disassembly dumps or private logs are committed.

These are **module-relative addresses in that reference binary**, not captured PCs from the current run. The current log does not expose a Build ID, so identical executable revisions cannot be asserted from that log alone.

| Reference call/instruction location | Evidence |
| --- | --- |
| main `0x10D2B18` | Initializes InlineKeyboard; subsequent instructions install its callbacks. |
| main `0x10D2B48`, `0x10D2B58` | Calls the legacy ChangedString and MovedCursor callback setters. SDK implementations at `0x3CEEE8` / `0x3CF084` send D/E with one zero byte. Thus this reference selects **legacy replies**, despite using API `0x8000D`. The next device trace will verify the actual bytes. |
| main `0x10D2DF4` | Calls GetImageMemoryRequirement before Appear or the first Calc. SDK `0x3CF254` calls VI with width 1280 and height 720. |
| main `0x10D2E24` | Calls Appear, which prepares pending Calc data; it does not itself send a Calc request. |
| main `0x10D3C40` | A separate update routine calls InlineKeyboard::Calc after acquiring the game's own synchronization. |
| SDK `0x3CE2CC` / `0x3CE330` | Calc first checks the applet exit event. With a live applet and pending flags, it sends request A with `0x4E8` argument bytes, clears the flags, then tries to pop interactive output. It does not wait for FinishedInitialize before sending that first Calc. |

This narrows the missing-Calc investigation to the return from D/E, subsequent game setup/VI, entry into the game's update routine, the exit-event check or IPC delivery. It does **not** locate the current guest PC or prove which boundary is blocked. A previous `Hitman Pr223 test swkbd.log` also records the same D/E prefix followed by a successful VI memory query (`0x3A0000`, alignment `0x1000`), without a later Calc. That historical run is corroborating evidence of the ordering, not proof of the current runtime's behavior.

`WaitSynchronization` was also reviewed: an unsignalled event with timeout zero returns TimedOut after checking under the sync-object mutex. No kernel/scheduler change was made without a failing reproduction. Current host tests do not execute this guest syscall or the game's own semaphores.

## Changes in this follow-up

- Added bounded markers before/after the state mutex, after request processing, after frontend action and before returning to AM. Dictionary-storage early returns are now explicit.
- D/E log raw byte, resulting flag, size and current state. Missing payload uses sentinel `0x100`, outside the byte range. No text contents are logged.
- Added SWKBD-only AM boundary tracing: before resolving storage, after storage resolution, before applet dispatch, and on handler return for the first eight interactive pushes. Event retrieval, completion/result, output polling and indirect-handle handlers have deduplicated entry/return markers. The applet state-event handle correlates these records.
- Added indirect-image state-lock markers and VI image-memory query entry/result markers because that query is established in the reference's pre-Calc sequence.
- Extended the native harness to compile the actual `ILibraryAppletAccessor.cpp`, in addition to the actual applet and IApplet queues. Added deadline-bounded replay and concurrent indirect-image access.

Protocol assignments, reply/event semantics, Calc parsing, frontend opening, Android code and touch controls are unchanged from `4ae01ad3`. All previous functional fixes, including SetInputText replies, remain. No startup timer, fabricated initialization, title ID branch or forced opening was added.

Diagnostics use Info level and `[SWKBD]`. Applet and AM gates each retain at most 64 distinct signatures, followed by one cap marker; repeated polls do not generate per-frame logs. AM epochs distinguish progress through the first eight input admissions, then stop increasing. The VI query gate is process-wide and bounded; it logs dimensions/results, not a caller's text. Temporary diagnostics intentionally remain for the requested device test and should be removed after the cause is established.

## Validation

`bash tests/swkbd/run.sh` passes normally and with ASan/UBSan (`-fsanitize=address,undefined -fno-omit-frame-pointer`, `ASAN_OPTIONS=detect_leaks=0`). Leak detection is disabled under the managed host's ptrace restriction; no leak-check claim is made. `git diff --check` passes.

The complete new native harness also passes against a separate copy of the production SWKBD/IApplet/accessor sources from parent `4ae01ad3`, without the new diagnostics. Thus these tests establish responsiveness on the observed prefix, rather than a failing-before/passing-after protocol correction.

- **24 prefix replays:** exact observed mode/API/CommonArguments/InitializeArg, five-byte D then E, every 0/1 flag pair, three supported Calc sizes, and separate versus combined Initialize/Appear. The supplied log lacks flag bytes and later Calc arguments; those variants are deliberate test coverage, not claims about observed packets.
- Each replay checks that D/E return within two seconds, do not signal output/completion or open Android, and remain responsive during 32 concurrent indirect-image calls. Subsequent Calc receives Default/FinishedInitialize, Appear reaches the frontend, callbacks have the selected legacy/V2 sizes/codes, toggling flags takes effect, truncated flags preserve negotiation, and Finalize completes.
- **4 AM boundary replays:** real Start/input/interactive-output handlers with the observed `0x4E8`-capable API and all flag pairs. D/E produce NotAvailable on pop; a combined Initialize/Appear Calc is delivered and its replies drain correctly; FrontendOpened and Finalize remain reachable.
- Existing six inline mode/layout flows, 24 guest text-reply sequences, malformed Calc recovery, three normal flows, wire-format/text/session/indirect-handle checks and trace-gate concurrency/cap checks still pass.

The harness replaces IPC storage resolution, kernel event/process/session edges, JVM and logging with host fixtures; it does not boot Hitman or exercise the Android IME, guest scheduler, actual IPC dispatch decoding or VI service object. The new VI markers have been reviewed against the existing handler; that translation unit is validated by the Android workflow, not by this host harness. Kotlin/Android files are unchanged; the previous 15 API 31 UI and nine API 29 tests are historical results from the parent, not rerun results in this follow-up. No hardware-fix claim follows from these passes.

## Next Moto G100 collection

1. Install the APK for this follow-up commit from PR #486, artifact `strato-pr486-arm64-release.apk`. Keep Strato Log Level at **Info**.
2. Start a fresh Hitman process, create a profile and wait about five seconds at ENTER PROFILE NAME. If absent, tap the field once / try A once and note the result. If it opens, test typing and confirm or cancel once.
3. Exit emulation normally; use **Share Logs** immediately and send the complete log before starting another game. No ADB or global per-frame Debug logging is required for this first collection.

| Last relevant new marker | Interpretation to investigate |
| --- | --- |
| AM `enter-before-storage` without `storage-resolved` | Input-object resolution has not been observed to finish; applet state lock has not yet been entered. |
| `packet-before-applet` / old pre-lock input without `state lock acquired` | State-lock acquisition is the outstanding boundary. |
| `state lock acquired` without `processed` | Request handling under the lock is the outstanding boundary. |
| `processed` / `state lock released` without `returning to AM accessor` | Frontend-action/return boundary; D/E should have action None. |
| AM D/E `handler-return` followed by VI query | D/E did return; inspect VI entry/result and later AM calls. It does not prove the guest resumed after the IPC response. |
| VI query returns, no admission of A | No Calc has reached the instrumented accessor. Inspect guest setup/update/exit-event progress next; do not invent a missing D/E ACK. |
| AM packet A appears but no applet Calc | The new resolution/lock/process markers distinguish its routing stage. |
| Calc / Appear / open worker / JNI/Kotlin | Continue the existing stage-by-stage frontend trace. Absence of JNI before Appear is expected. |

The push triggers the existing ARM64 DevRelease PR workflow. Per request, its build is not monitored; APK completion is not asserted.
