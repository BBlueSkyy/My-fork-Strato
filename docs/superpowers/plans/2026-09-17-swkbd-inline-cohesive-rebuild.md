# SWKBD Inline Cohesive Rebuild Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the experimental inline/custom SWKBD path in PR #185 with one coherent Strato-native implementation that matches Eden's externally observable protocol behavior, while leaving Strato's existing `AllForeground` keyboard unchanged.

**Architecture:** `SoftwareKeyboardApplet` owns HOS inline protocol parsing, state, replies, queues, and lifecycle. A focused `InlineKeyboardFrontend` translates protocol-owned host parameters into Strato's existing Android keyboard configuration and delegates UI work to `JvmManager`; JNI/Kotlin only report concrete host input events and never know HOS reply IDs or states. AM/VI remain thin compatibility/transport layers.

**Tech Stack:** C++20, Strato AM applet/storage APIs, JNI, Kotlin/Android `InputConnection`, Gradle/Android NDK 29.

**Spec:** `docs/superpowers/specs/2026-09-17-swkbd-inline-cohesive-rebuild-design.md`

## Global Constraints

- Preserve the current `AllForeground` SWKBD path and its validation/dialog behavior.
- Rebuild only `PartialForeground` / `PartialForegroundWithIndirectDisplay`.
- No sleeps, polling loops, artificial timeouts, forced completion, title-specific conditions, or global scheduler changes.
- Do not modify GPU, NCE, kernel scheduling, NCA, audio, or unrelated AM services.
- Eden is a behavioral reference only; do not copy GPL implementation text into MPL files.
- `SoftwareKeyboardApplet` is the sole owner of HOS SWKBD state/replies.
- Android/JNI owns only host UI/IME behavior.
- `GetIndirectLayerConsumerHandle` remains SWKBD-scoped and returns an opaque non-zero token, not a kernel handle.
- `GetIndirectLayerImageMap` consumes the full ABI and returns zero size/stride for the custom frontend; no fake framebuffer is produced.
- Final branch must contain no temporary SWKBD trace instrumentation.

---

### Task 1: Lock the protocol ABI and introduce the frontend boundary

**Files:**
- Modify: `app/src/main/cpp/skyline/applet/swkbd/software_keyboard_inline.h`
- Create: `app/src/main/cpp/skyline/applet/swkbd/inline_keyboard_frontend.h`
- Create: `app/src/main/cpp/skyline/applet/swkbd/inline_keyboard_frontend.cpp`
- Modify: `app/CMakeLists.txt`

**Interfaces:**
- Produces: `InlineKeyboardFrontendEvent`, `InlineKeyboardInitializeParameters`, `InlineKeyboardAppearParameters`, `InlineKeyboardFrontend`.
- `SoftwareKeyboardApplet` consumes this API in Task 2.
- `InlineKeyboardFrontend` consumes `JvmManager` host-session methods implemented in Task 3.

- [ ] **Step 1: Strengthen compile-time protocol guards**

Add explicit `static_assert`s for request/reply IDs and every wire-size already relied on by the applet, including:

```cpp
static_assert(static_cast<u32>(Request::Calc) == 0xA);
static_assert(static_cast<u32>(Reply::FinishedInitialize) == 0x0);
static_assert(static_cast<u32>(Reply::MovedCursorUtf8V2) == 0x10);
static_assert(sizeof(InitializeArg) == 0x8);
static_assert(sizeof(CalcArgCommon) == 0x18);
static_assert(sizeof(CalcArgCommon) + sizeof(CalcArgOldBody) == 0x4A0);
static_assert(sizeof(CalcArgCommon) + sizeof(CalcArgNewBody) == 0x4E8);
static_assert(sizeof(ChangedStringArg) == 0x10);
static_assert(sizeof(MovedCursorArg) == 0x8);
static_assert(sizeof(DecidedEnterArg) == 0x4);
```

Do not change the established ABI values while adding these guards.

- [ ] **Step 2: Define the Strato-native frontend contract**

Create `inline_keyboard_frontend.h` with no HOS reply/state types exposed to Android:

```cpp
namespace skyline {
class JvmManager;
}

namespace skyline::applet::swkbd {
struct InlineKeyboardFrontendEvent {
    enum class Kind : u32 { TextChanged, CursorMoved, Enter, Cancel };
    Kind kind{};
    std::u16string text{};
    i32 cursor{};
};

struct InlineKeyboardInitializeParameters {
    u32 type{};
    std::u16string okText{};
    char16_t leftOptionalSymbolKey{};
    char16_t rightOptionalSymbolKey{};
    u32 keyDisableFlags{};
    u32 maxTextLength{500};
    u32 minTextLength{};
    i32 initialCursorPosition{};
    bool enableBackspace{true};
    bool enableReturn{};
    bool disableCancel{};
};

struct InlineKeyboardAppearParameters {
    float keyTopScaleX{};
    float keyTopScaleY{};
    float keyTopTranslateX{};
    float keyTopTranslateY{};
    bool keyTopAsFloating{};
};

class InlineKeyboardFrontend {
  public:
    using Callback = std::function<void(InlineKeyboardFrontendEvent)>;

    explicit InlineKeyboardFrontend(JvmManager &jvm);
    void Initialize(InlineKeyboardInitializeParameters parameters, Callback callback);
    void Show(const InlineKeyboardAppearParameters &parameters, std::u16string_view text, i32 cursor);
    void Update(std::u16string_view text, i32 cursor);
    void Hide();
    void Close();
    bool IsInitialized() const;
};
}
```

The implementation stores host configuration/session state only; it must not serialize HOS replies.

- [ ] **Step 3: Register the focused source file**

Add only `${source_DIR}/skyline/applet/swkbd/inline_keyboard_frontend.cpp` next to the existing SWKBD sources in `app/CMakeLists.txt`.

- [ ] **Step 4: Run structural checks**

Run:

```bash
git diff --check
git diff -- app/src/main/cpp/skyline/applet/swkbd/software_keyboard_inline.h \
  app/src/main/cpp/skyline/applet/swkbd/inline_keyboard_frontend.h \
  app/src/main/cpp/skyline/applet/swkbd/inline_keyboard_frontend.cpp app/CMakeLists.txt
```

Expected: no whitespace errors; no JNI/HOS reply code in the frontend header.

- [ ] **Step 5: Commit the protocol/frontend boundary**

```bash
git add app/src/main/cpp/skyline/applet/swkbd/software_keyboard_inline.h \
  app/src/main/cpp/skyline/applet/swkbd/inline_keyboard_frontend.h \
  app/src/main/cpp/skyline/applet/swkbd/inline_keyboard_frontend.cpp app/CMakeLists.txt
git commit -m "swkbd: define cohesive inline frontend boundary"
```

### Task 2: Rebuild the inline protocol core inside `SoftwareKeyboardApplet`

**Files:**
- Modify: `app/src/main/cpp/skyline/applet/swkbd/software_keyboard_applet.h`
- Modify: `app/src/main/cpp/skyline/applet/swkbd/software_keyboard_applet.cpp`

**Interfaces:**
- Consumes: `InlineKeyboardFrontend` from Task 1.
- Produces: exact inline request processing, state transitions, reply serialization, and frontend event mapping.
- Preserves: existing `AllForeground` `Start()`, validation, `SendResult()`, dialog, and normal text-check behavior.

- [ ] **Step 1: Replace direct `JvmManager` ownership in the inline path**

Remove inline-specific `JvmManager::InlineKeyboardUpdate` handling from `SoftwareKeyboardApplet`. Add one frontend member and one inline session state:

```cpp
struct InlineSessionState {
    inline_protocol::State state{inline_protocol::State::NotInitialized};
    std::u16string text{};
    i32 cursor{};
    bool started{};
    bool useUtf8{};
    bool useChangedStringV2{};
    bool useMovedCursorV2{};
    bool enableBackspace{true};
};

std::mutex inlineMutex;
InlineSessionState inlineSession{};
InlineKeyboardFrontend inlineFrontend;
```

Keep old/new parsed `Calc` storage as protocol-owned members. Do not move normal foreground members.

- [ ] **Step 2: Keep `Start()` split cleanly by applet mode**

For `AllForeground`, execute the existing normal code unchanged. For the two inline modes, `StartInline()` must:

1. pop `CommonArguments` and the exact 8-byte `InitializeArg`;
2. validate `libraryAppletModeFlag` against `PartialForeground` vs `PartialForegroundWithIndirectDisplay`;
3. reset only inline session/protocol state;
4. not show Android UI;
5. not emit `FinishedInitialize` before a later `Calc` with `SetInitializeArg`.

Invalid storage/mode returns the existing AM `NotAvailable` result instead of forcing success.

- [ ] **Step 3: Implement request dispatch as a closed protocol switch**

`ProcessInlineRequest()` handles exactly:

```text
0x4 Finalize
0x6 SetUserWordInfo
0x7 SetCustomizeDic
0xA Calc
0xB SetCustomizedDictionaries
0xC UnsetCustomizedDictionaries
0xD SetChangedStringV2Flag
0xE SetMovedCursorV2Flag
```

`0xD` and `0xE` require exactly five bytes (`u32 command + u8 flag`). Unknown commands log a warning and do not create reply/state progress.

- [ ] **Step 4: Parse `Calc` exactly once and dispatch old/new layouts**

Read `CalcArgCommon` at `sizeof(u32)` and require:

```cpp
const size_t expectedSize = sizeof(u32) + common.calcArgSize;
```

Accept only `0x4A0` or `0x4E8` `calcArgSize` wire layouts. Copy the body after `sizeof(u32) + sizeof(CalcArgCommon)`. Unsupported sizes log and return without mutating protocol state.

- [ ] **Step 5: Match Eden ordering for old/new Calc processing**

For both layouts, process in this order:

1. apply `SetInputText`, `SetCursorPosition`, `SetUtf8Mode` to stored guest state;
2. send `UnsetCustomizeDic` / `ReleasedUserWordInfo` only when their flags and state allow it;
3. if state is `NotInitialized` and `SetInitializeArg` is set, build `InlineKeyboardInitializeParameters`, call `inlineFrontend.Initialize(...)`, transition to `InitializedIsHidden`, then send `FinishedInitialize`;
4. only when `SetInitializeArg` is **not** set, guest `SetInputText`/`SetCursorPosition` calls `inlineFrontend.Update(...)` and emits **no** synthetic `ChangedString` reply;
5. `Appear` from hidden: `Default(Appearing)` -> frontend `Show` -> `Default(Shown)`;
6. `Disappear` from shown: `Default(Disappearing)` -> frontend `Hide` -> `Default(Hidden)`.

Do not collapse the state transitions or reorder `FinishedInitialize` before hidden state.

- [ ] **Step 6: Centralize reply serialization**

Use one base helper:

```cpp
void SendInlineReply(inline_protocol::Reply reply, span<const u8> payload = {});
```

Every reply begins with exactly `{u32 state, u32 reply}`. Text replies allocate the named fixed text region plus the matching arg struct. V2 replies append one zero byte exactly as the reference does. Clamp cursor to `[0, text.size()]`, preserve dictionary cursor fields as `-1`, and null-terminate text only when capacity permits.

- [ ] **Step 7: Map only genuine frontend events to text replies**

Frontend callback mapping:

```text
TextChanged -> ChangedString / UTF8 / V2 selector
CursorMoved -> MovedCursor / UTF8 / V2 selector
Enter       -> DecidedEnter / UTF8, then HideInlineKeyboard()
Cancel      -> DecidedCancel, then HideInlineKeyboard()
```

Ignore callbacks unless the session is started and state is `InitializedIsShown`.

- [ ] **Step 8: Make Finalize lifecycle-complete**

`Finalize` must transition to `NotInitialized` through the normal state helper, mark the inline session stopped, close the frontend/callback exactly once, and signal `onAppletStateChanged`. `Disappear` must not finalize the applet.

- [ ] **Step 9: Verify normal foreground code did not move semantically**

Run:

```bash
git diff --word-diff=plain master...HEAD -- \
  app/src/main/cpp/skyline/applet/swkbd/software_keyboard_applet.cpp
```

Review every hunk touching `AllForeground`, `SendResult`, `ValidationRequest`, `ValidationResult`, `ShowKeyboard`, `WaitForSubmitOrCancel`, or text-check logic. Revert unrelated changes.

- [ ] **Step 10: Commit the protocol core**

```bash
git add app/src/main/cpp/skyline/applet/swkbd/software_keyboard_applet.h \
  app/src/main/cpp/skyline/applet/swkbd/software_keyboard_applet.cpp
git commit -m "swkbd: rebuild inline protocol state machine"
```

### Task 3: Implement a generation-safe C++/JNI host session

**Files:**
- Modify: `app/src/main/cpp/skyline/jvm.h`
- Modify: `app/src/main/cpp/skyline/jvm.cpp`
- Modify: `app/src/main/cpp/skyline/applet/swkbd/inline_keyboard_frontend.cpp`

**Interfaces:**
- Produces: opaque host session generation and JNI show/update/hide/close calls.
- Consumes: host-only callback from `InlineKeyboardFrontend`.
- Does not expose HOS states/reply IDs.

- [ ] **Step 1: Replace the global inline callback with a session ID**

Define a host-only session API in `JvmManager`:

```cpp
using InlineKeyboardSessionId = u64;
using InlineKeyboardCallback = std::function<void(InlineKeyboardUpdate)>;

InlineKeyboardSessionId BeginInlineKeyboardSession(InlineKeyboardCallback callback);
void EndInlineKeyboardSession(InlineKeyboardSessionId id);
void ShowInlineKeyboard(InlineKeyboardSessionId id, KeyboardConfig &config,
                        std::u16string_view text, i32 cursor, bool enableBackspace);
void UpdateInlineKeyboard(InlineKeyboardSessionId id, std::u16string_view text, i32 cursor);
void HideInlineKeyboard(InlineKeyboardSessionId id);
void CloseInlineKeyboard(InlineKeyboardSessionId id);
void SubmitInlineKeyboardUpdate(InlineKeyboardSessionId id, InlineKeyboardUpdate update);
```

Store `activeInlineKeyboardSessionId`, `nextInlineKeyboardSessionId`, and the callback under the existing mutex. Never invoke the callback while holding that mutex.

- [ ] **Step 2: Drop stale callbacks deterministically**

`SubmitInlineKeyboardUpdate(id, update)` copies the callback only when `id == activeInlineKeyboardSessionId`; otherwise it returns. `EndInlineKeyboardSession(id)` only clears a matching active session.

This is the lifetime guard required by the spec; no timeout is used.

- [ ] **Step 3: Pass the session ID through JNI**

Change Java signatures so `show/update/hide/close` and native callback carry `jlong sessionId`. JNI callback becomes conceptually:

```cpp
SubmitInlineKeyboardEvent(JNIEnv *env, jlong sessionId, jint kind, jstring text, jint cursor) {
    ...
    os->state.jvm->SubmitInlineKeyboardUpdate(
        static_cast<JvmManager::InlineKeyboardSessionId>(sessionId),
        {.kind = ..., .text = ..., .cursor = static_cast<i32>(cursor)});
}
```

- [ ] **Step 4: Implement `InlineKeyboardFrontend` as the only applet-to-JNI adapter**

`Initialize()` stores the immutable host keyboard config and starts a new `JvmManager` session. `Show()` applies current appear parameters to host config and opens the IME. `Update`, `Hide`, and `Close` delegate to the same session ID. `Close()` is idempotent and ends the callback session.

Convert protocol-facing initialize data into the existing `KeyboardConfigVB`/`JvmManager::KeyboardConfig` only here, not inside `SoftwareKeyboardApplet`.

- [ ] **Step 5: Check lock ordering**

Verify with code review that:

```text
JvmManager mutex -> copy callback -> unlock -> callback -> applet inline mutex
```

No path may hold `inlineKeyboardCallbackMutex` while calling into the applet.

- [ ] **Step 6: Commit the C++ host bridge**

```bash
git add app/src/main/cpp/skyline/jvm.h app/src/main/cpp/skyline/jvm.cpp \
  app/src/main/cpp/skyline/applet/swkbd/inline_keyboard_frontend.cpp
git commit -m "swkbd: add generation-safe inline host bridge"
```

### Task 4: Rebuild the Android inline editor around the host session

**Files:**
- Modify: `app/src/main/java/org/stratoemu/strato/applet/swkbd/InlineKeyboardInputView.kt`

**Interfaces:**
- Consumes: JNI methods carrying `sessionId` and existing serialized `SoftwareKeyboardConfig`.
- Produces: direct events `TextChanged`, `CursorMoved`, `Enter`, `Cancel` with the same `sessionId`.

- [ ] **Step 1: Make the view session-aware**

Store:

```kotlin
private var sessionId: Long = 0L
private var submitted = false
private var suppressCallbacks = false
```

`show(...)` receives `sessionId: Long`; `update/hide/close` ignore calls whose ID is not the active session. Reopening replaces the old host session cleanly.

- [ ] **Step 2: Keep programmatic guest updates callback-free**

`setGuestText(text, cursor)` sets `suppressCallbacks = true`, replaces the editable, clamps text/cursor, then clears suppression. It may restart the `InputMethodManager` but must not submit a native event.

- [ ] **Step 3: Emit concrete editor events only**

The `InputConnection` must handle:

```text
commitText / setComposingText -> TextChanged
deleteSurroundingText         -> TextChanged when backspace enabled
setSelection                  -> CursorMoved
performEditorAction           -> Enter
KEYCODE_ENTER                 -> Enter
KEYCODE_BACK pre-IME          -> Cancel unless cancel is disabled
KEYCODE_DEL                   -> deletion when backspace enabled
```

Every event calls native with the active session ID. `finish()` marks `submitted` before the native callback to prevent duplicates.

- [ ] **Step 4: Preserve IME composition and length limits**

Use the shared `Editable` returned by `BaseInputConnection.getEditable()`. Clamp to `textMaxLength` after input changes without destroying the current selection. Do not poll IME visibility and do not add `Handler.postDelayed`, `Thread.sleep`, timers, or periodic tasks.

- [ ] **Step 5: Verify forbidden patterns are absent**

Run:

```bash
grep -nE 'postDelayed|Thread\.sleep|delay\(|Timer|while.*isKeyboardVisible' \
  app/src/main/java/org/stratoemu/strato/applet/swkbd/InlineKeyboardInputView.kt
```

Expected: no matches.

- [ ] **Step 6: Commit the Android frontend**

```bash
git add app/src/main/java/org/stratoemu/strato/applet/swkbd/InlineKeyboardInputView.kt
git commit -m "swkbd: rebuild inline Android IME session"
```

### Task 5: Reduce AM/VI back to thin protocol adapters

**Files:**
- Modify: `app/src/main/cpp/skyline/services/am/applet/ILibraryAppletAccessor.cpp`
- Modify: `app/src/main/cpp/skyline/services/visrv/IApplicationDisplayService.cpp`

**Interfaces:**
- AM transports storage/events and returns the SWKBD-scoped opaque consumer token.
- VI consumes indirect-layer ABI and returns custom-frontend zero image size/stride.

- [ ] **Step 1: Remove all temporary accessor request tracing/parsing**

Delete `[SWKBD-TRACE]` logs, `<cstring>` added only for diagnostics, command sniffing, and Calc-header inspection. Restore `PushInteractiveInData` to thin transport:

```cpp
applet->PushInteractiveDataToApplet(request.PopService<IStorage>(0, session));
return {};
```

Do not move any SWKBD parser into AM.

- [ ] **Step 2: Retain only the proven command-160 compatibility behavior**

For `LibraryAppletSwkbd`, keep:

```cpp
response.Push<u64>(0xDEADBEEFULL);
return {};
```

Other applets retain `result::ObjectInvalid`. Document it as an opaque compatibility token, not a `KHandle` or VI object.

- [ ] **Step 3: Keep the corrected VI ABI without diagnostic logging**

`GetIndirectLayerImageMap` must pop exactly:

```cpp
i64 width;
i64 height;
u64 indirectLayerConsumerHandle;
u64 aruid;
```

It must not paint the output buffer and returns:

```cpp
response.Push<u64>(0); // size
response.Push<u64>(0); // stride
```

`GetIndirectLayerImageRequiredMemoryInfo` retains `AlignUp(width * height * 4, 0x20000)` and alignment `0x1000` exactly; remove trace-only logging.

- [ ] **Step 4: Commit AM/VI cleanup**

```bash
git add app/src/main/cpp/skyline/services/am/applet/ILibraryAppletAccessor.cpp \
  app/src/main/cpp/skyline/services/visrv/IApplicationDisplayService.cpp
git commit -m "swkbd: finalize inline AM and VI contracts"
```

### Task 6: Perform one consolidated static review and Android build

**Files:**
- Review all implementation files from Tasks 1-5.
- Update: PR #185 description after validation.

**Interfaces:**
- Produces: one clean APK candidate for device validation.
- No diagnostic-only build is produced.

- [ ] **Step 1: Verify diff scope**

Run:

```bash
git status --short
git diff --check master...HEAD
git diff --name-only master...HEAD
```

Implementation files must be limited to the approved SWKBD/JNI/AM/VI/CMake set plus the spec/plan docs. There must be no `nce.cpp`, scheduler, kernel, GPU, NCA, or audio diff.

- [ ] **Step 2: Scan for experimental/debug leftovers**

Run:

```bash
grep -RInE 'SWKBD-TRACE|SWKBD-PATH|postDelayed|Thread\.sleep|sleep_for|titleId|programId.*Hitman' \
  app/src/main/cpp/skyline/applet/swkbd \
  app/src/main/cpp/skyline/services/am/applet/ILibraryAppletAccessor.cpp \
  app/src/main/cpp/skyline/services/visrv/IApplicationDisplayService.cpp \
  app/src/main/cpp/skyline/jvm.cpp \
  app/src/main/java/org/stratoemu/strato/applet/swkbd
```

Expected: no diagnostic polling/hack matches.

- [ ] **Step 3: Review the normal path against master**

Use `git diff master...HEAD` and confirm no semantic changes to normal `AllForeground` dialog/validation behavior. If an earlier #185 edit changed normal behavior incidentally, restore the master form before building.

- [ ] **Step 4: Run the same full Android release build used by CI**

Run:

```bash
./gradlew --no-daemon --stacktrace --build-cache --parallel --configure-on-demand assembleMainlineRelease
```

Expected: `BUILD SUCCESSFUL` and APK under `app/build/outputs/apk/mainline/release/`.

If compilation fails, fix only errors caused by this branch and rerun this same build. Do not add runtime diagnostics to solve compile errors.

- [ ] **Step 5: Finalize PR #185 description**

Replace the stale claim that the PR has no accessor/VI changes. Document the final architecture, the SWKBD-scoped token, corrected VI ABI, generation-safe Android callbacks, preserved normal path, and exact device validation checklist.

- [ ] **Step 6: Commit any final cleanup**

```bash
git add -A
git diff --cached --check
git commit -m "swkbd: finalize cohesive inline frontend rebuild"
```

Skip the commit if the tree is already clean.

### Task 7: One device validation pass

**Files:**
- No code changes unless the consolidated build exposes a concrete defect.

**Interfaces:**
- Consumes: the single APK candidate from Task 6.
- Produces: runtime acceptance evidence for PR #185.

- [ ] **Step 1: Hitman acceptance sequence**

On the APK built from the final branch:

```text
reach name entry
-> IME opens
-> type several characters
-> delete
-> move cursor and edit in the middle
-> confirm and advance
-> reopen keyboard
-> cancel and return without hang/termination
```

- [ ] **Step 2: Normal foreground regression sequence**

Use one title that already exercised Strato's normal dialog:

```text
open -> type -> confirm -> reopen -> cancel -> validation behavior unchanged
```

- [ ] **Step 3: Wall World 2 regression sequence**

Exercise its keyboard path and verify input/submit returns to gameplay.

- [ ] **Step 4: Handle runtime failure by concrete defect, not another instrumentation cycle**

If the consolidated APK still fails, collect the ordinary `emulation.log` from that build and compare the failing protocol operation against the implemented reference semantics. Do not pre-emptively add another tracing APK. Add targeted diagnostics only if the ordinary log plus code/reference comparison cannot identify a concrete boundary.

- [ ] **Step 5: Keep PR draft status until device acceptance**

CI success proves build integrity only. Mark ready for review only after the Hitman acceptance sequence succeeds and the two regression checks are clean.
