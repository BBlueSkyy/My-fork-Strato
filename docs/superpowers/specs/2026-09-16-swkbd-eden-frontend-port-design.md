# Eden SWKBD frontend port design

## Goal

Port the working Eden Android custom software-keyboard frontend into Strato, adapting it to Strato's existing applet/storage/JNI architecture instead of adding another indirect-layer workaround. Hitman: Blood Money - Reprisal is the primary reproducer because it works in Eden with **Custom frontend**.

## Scope

- Start from current `master` only. Do not merge or reuse #151, #154, #181 or #182 wholesale.
- Keep Strato's existing foreground SWKBD protocol and validation behavior where already correct.
- Add a small frontend boundary modeled on Eden: applet protocol owns HOS state/replies; Android frontend owns UI/IME input.
- Support normal and inline/custom-frontend paths. Inline must be asynchronous from the guest IPC thread.
- Do not require VI indirect-layer rendering for the custom frontend path.
- No game-specific conditions, sleeps, forced completion, fake fixed handles, or unrelated AM/GPU/kernel changes.

## Architecture

### 1. C++ frontend contract

Add a focused SWKBD frontend interface under `skyline/applet/swkbd` with only the data Strato needs:

- initialization parameters;
- inline appear parameters;
- inline text/cursor updates;
- normal submit callback;
- inline submit callback (`ChangedString`, `MovedCursor`, `DecidedEnter`, `DecidedCancel`).

`SoftwareKeyboardApplet` remains responsible for parsing guest storage, the inline state machine and reply serialization. The frontend never fabricates HOS replies itself.

### 2. Android/JNI frontend

Adapt Eden's Android frontend behavior to Strato's package/activity structure:

- normal keyboard uses Strato's existing `SoftwareKeyboardDialog`;
- inline keyboard uses Android IME attached to the emulation/input view;
- JNI exposes only initialize/show/hide/update/exit and submit callbacks;
- callbacks return text/cursor events to the C++ applet without blocking IPC.

Do not copy Eden package names or its global native-system plumbing. Keep Strato's `JvmManager` as the JNI owner.

### 3. Inline protocol

Use Eden/libnx ABI/state ordering as the reference:

- parse `SwkbdInitializeArg` and old/new `Calc` layouts exactly;
- initialize frontend only when `set_initialize_arg` is received;
- state transitions emit the same Default/FinishedInitialize ordering as Eden;
- `appear` shows the Android IME and transitions to shown;
- frontend text changes emit the correct UTF-8/UTF-16 and V2 reply variants;
- enter/cancel produce `DecidedEnter`/`DecidedCancel`;
- `disappear` hides the IME without completing the applet;
- `Finalize` exits cleanly.

Dictionary commands that Eden itself does not implement fully remain minimal but ABI-correct; do not invent dictionary contents.

## Files expected to change

- `app/src/main/cpp/skyline/applet/swkbd/software_keyboard_applet.{h,cpp}`
- one compact SWKBD frontend/types header/source under the same directory
- `app/src/main/cpp/skyline/jvm.{h,cpp}`
- `app/src/main/java/org/stratoemu/strato/EmulationActivity.kt`
- `app/src/main/java/org/stratoemu/strato/applet/swkbd/*` for the inline IME adapter
- `app/CMakeLists.txt` only if a new `.cpp` is added

Avoid changes to `ILibraryAppletAccessor`/VI unless runtime evidence proves the custom frontend still needs them.

## Licensing

Where Eden code is directly adapted, preserve its SPDX/copyright notices and compatible GPL terms in the derived file. Prefer small Strato-native adapters around Eden behavior rather than copying unrelated Eden infrastructure.

## Validation

1. `git diff --check`.
2. Android compile/CI.
3. Normal foreground keyboard regression test.
4. Hitman inline name-entry: open keyboard, type, delete, confirm, cancel, reopen.
5. Wall World 2 keyboard regression test.
6. Verify logs show real inline request/reply progression and no dependency on indirect-layer image mapping.

Success means Hitman passes the same keyboard point as Eden custom frontend without game-specific hacks and normal SWKBD remains functional.