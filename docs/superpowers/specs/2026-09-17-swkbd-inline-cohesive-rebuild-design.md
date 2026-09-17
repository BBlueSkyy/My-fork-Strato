# SWKBD inline cohesive rebuild design

## Goal

Rebuild only Strato's inline/custom software-keyboard path (`PartialForeground` and `PartialForegroundWithIndirectDisplay`) as one coherent implementation based on the behavior of Eden's working custom frontend, while leaving Strato's existing `AllForeground` software keyboard unchanged.

Hitman: Blood Money - Reprisal is the primary reproducer because it reaches the inline SWKBD path and works with Eden's custom frontend. Wall World 2 is the secondary regression title. The implementation must be generic and must not contain title-specific conditions.

## Core constraints

- Preserve the current `AllForeground` SWKBD path and its validation/dialog behavior.
- Replace the experimental inline implementation in PR #185 rather than layering more fixes on top of it.
- `SoftwareKeyboardApplet` is the sole owner of HOS SWKBD inline protocol state, request parsing, reply serialization, and lifecycle.
- Android/JNI owns host UI/IME interaction only. It must not manufacture HOS replies or applet state transitions.
- Inline input is asynchronous. Never block guest IPC waiting for Android input.
- No sleeps, polling loops, artificial timeouts, forced completion, global scheduler changes, or game-specific hacks.
- Do not modify GPU, NCE, kernel scheduling, NCA, audio, or unrelated AM services.
- Keep the implementation small enough that protocol, frontend bridge, and Android input can each be reviewed independently.

## Reference behavior

Eden is the behavioral reference for:

- `SwkbdInitializeArg` and old/new `Calc` ABI layouts;
- request IDs and reply IDs;
- state transitions and reply ordering;
- UTF-8/UTF-16 reply variants;
- ChangedString/MovedCursor V2 selection;
- `Appear`, `Disappear`, and `Finalize` semantics;
- the custom frontend's use of a non-zero indirect-layer consumer token;
- the custom frontend's `GetIndirectLayerImageMap` contract.

The implementation must be Strato-native. Do not copy Eden's GPL files into MPL files. Reimplement the externally observable behavior and retain Strato's architecture, types, and ownership model.

## Architecture

### 1. Inline protocol core

`SoftwareKeyboardApplet` owns one inline session state object containing:

- current HOS `SwkbdState`;
- current text and cursor;
- UTF-8 mode;
- ChangedString V2 and MovedCursor V2 flags;
- parsed old/new `Calc` configuration;
- whether the inline frontend has been initialized;
- whether the Android frontend is visible;
- the active frontend callback/session generation.

The existing normal foreground state and code paths are not moved into this object.

The protocol core handles these requests:

- `Finalize (0x4)`;
- `SetUserWordInfo (0x6)`;
- `SetCustomizeDic (0x7)`;
- `Calc (0xA)`;
- `SetCustomizedDictionaries (0xB)`;
- `UnsetCustomizedDictionaries (0xC)`;
- `SetChangedStringV2Flag (0xD)`;
- `SetMovedCursorV2Flag (0xE)`.

Unknown requests are logged and ignored without fabricating successful protocol progress.

Dictionary commands stay ABI-correct and minimal where Eden itself has no complete host dictionary implementation. No dictionary content is invented.

### 2. Exact inline state machine

The state machine is:

`NotInitialized -> InitializedIsHidden -> InitializedIsAppearing -> InitializedIsShown -> InitializedIsDisappearing -> InitializedIsHidden`

`Finalize` returns the session to `NotInitialized` and closes the host frontend.

State changes emit `Default` replies exactly through one helper. Initialization then emits `FinishedInitialize` after the transition to `InitializedIsHidden`.

`set_initialize_arg` is the only operation that initializes the host frontend. `Appear` never performs first-time protocol initialization implicitly.

`SetInputText` and `SetCursorPosition` update the stored state and notify the host frontend only when they are not part of the same `Calc` that performs initial setup. They must not synthesize an unsolicited guest `ChangedString` reply. Guest text replies are generated only from genuine host input callbacks or protocol-required state/reply operations.

`Appear` is honored only from `InitializedIsHidden`. It emits `Default(InitializedIsAppearing)`, shows the host frontend, then emits `Default(InitializedIsShown)`.

`Disappear` is honored only from `InitializedIsShown`. It emits `Default(InitializedIsDisappearing)`, hides the host frontend, then emits `Default(InitializedIsHidden)`.

### 3. Reply serialization

All inline replies are serialized through one protocol helper using the common 8-byte header:

- 32-bit `SwkbdState`;
- 32-bit `SwkbdReplyType`.

Text replies use the ABI-sized text region followed by the correct argument struct:

- UTF-16 text region: `0x3EC` bytes;
- UTF-8 text region: `0x7D4` bytes;
- `ChangedStringArg`: `0x10` bytes;
- `MovedCursorArg`: `0x8` bytes;
- `DecidedEnterArg`: `0x4` bytes.

V2 variants use the same base text payload and the exact trailing V2 extension required by the reference behavior. The implementation must derive sizes from named protocol types/constants and static assertions rather than magic allocation sizes scattered through the applet.

`ChangedString`, `MovedCursor`, `DecidedEnter`, and their UTF-8/V2 variants are selected only in the protocol layer.

### 4. Frontend boundary

Introduce a small Strato-native inline frontend contract under `skyline/applet/swkbd`.

The protocol core calls only these conceptual operations:

- initialize inline session with immutable keyboard parameters and callbacks;
- show with appear parameters;
- update text/cursor from guest state;
- hide;
- close/exit.

The frontend callback reports only host events:

- text changed;
- cursor moved;
- enter/confirm;
- cancel.

The callback carries text and cursor where applicable. It does not expose HOS reply IDs to Android.

This boundary prevents `SoftwareKeyboardApplet` from directly manipulating Java views and prevents `JvmManager`/Kotlin from knowing HOS state-machine details.

### 5. AM accessor integration

`ILibraryAppletAccessor` remains a transport layer:

- push normal storage;
- push interactive storage;
- pop normal/interactive output storage;
- expose the existing output events;
- start the applet;
- expose the indirect-layer consumer token required by the SWKBD custom frontend path.

It must not parse SWKBD `Calc`, flags, text, or state.

For `LibraryAppletSwkbd` only, `GetIndirectLayerConsumerHandle` returns a stable non-zero opaque `u64` token compatible with the custom frontend flow. This value is not registered as a kernel handle and is never exposed as a generic solution for other applets. Other applets keep their existing behavior.

The current `0xDEADBEEF` value may be retained as that SWKBD-scoped opaque compatibility token because the reference frontend requires only a non-zero token and runtime evidence already showed that rejecting command 160 terminates Hitman before the frontend can proceed.

### 6. VI indirect-layer contract

The custom frontend does not render an indirect framebuffer.

`GetIndirectLayerImageRequiredMemoryInfo` retains the existing correct size/alignment calculation and ABI.

`GetIndirectLayerImageMap` consumes the full request ABI:

- width;
- height;
- indirect-layer consumer token;
- ARUID;
- output buffer descriptor if supplied.

For the custom SWKBD path it does not paint or fabricate an image. It returns the same zero size/stride semantics as the reference custom frontend.

No SWKBD-specific rendering backend, compositor object, or fake VI layer is introduced.

### 7. Android/JNI frontend

`JvmManager` owns JNI references and exposes the native implementation of the frontend contract. It does not own HOS state.

The Android inline view is a hidden text editor/InputConnection attached to the active emulation activity. It is responsible only for:

- opening the IME;
- applying guest-provided initial text/cursor/configuration;
- applying guest-driven text/cursor updates;
- enforcing host-side maximum text length and backspace/cancel enablement;
- emitting direct callbacks for text changes, cursor movement, Enter, and Cancel;
- hiding and closing the IME session.

There is no 500 ms polling loop and no inference based on periodic IME visibility checks. Android input methods call native directly from concrete editor/input events.

The frontend must avoid duplicate `ChangedString` callbacks caused by programmatic guest updates. Guest-driven `update(text, cursor)` therefore runs under callback suppression.

IME composition must remain valid: `setComposingText`, `commitText`, deletion, selection changes, editor action, and Back handling update the shared editable state consistently.

### 8. Concurrency and lifecycle

The inline protocol state is protected by one applet-owned mutex. JNI callbacks copy the registered callback under the JNI bridge lock, release that lock, then invoke the applet callback. The applet callback then takes the applet mutex. This lock ordering avoids calling back into the applet while holding the JVM callback mutex.

Every inline session has a generation/lifetime guard so stale Android callbacks from a closed keyboard cannot affect a newly opened keyboard.

Destruction, `Finalize`, and applet exit all clear the registered callback and close the host frontend exactly once. Closing the host frontend must not itself synthesize Enter or Cancel.

`Disappear` hides the frontend but keeps the session alive; a later `Appear` may reopen it with the current text/cursor.

### 9. Migration from the current PR #185

The rebuild keeps only behavior already proven correct or required by the reference flow:

- exact inline protocol type sizes/static assertions;
- SWKBD-scoped non-zero indirect-layer token;
- full `GetIndirectLayerImageMap` input ABI and zero size/stride custom-frontend response;
- direct Android input callbacks instead of polling;
- existing working foreground SWKBD code.

Experimental responsibilities are removed from the wrong layers:

- `ILibraryAppletAccessor` no longer contains SWKBD request diagnostics or request parsing;
- `SoftwareKeyboardApplet` no longer directly treats `JvmManager` as the frontend API;
- Android no longer deals in HOS reply types;
- temporary trace-only logging is removed once equivalent deterministic behavior is represented by the final implementation.

The prior broken NCE/SVC instrumentation remains reverted and no NCE instrumentation is reintroduced as part of this work.

## Expected files

Primary files:

- `app/src/main/cpp/skyline/applet/swkbd/software_keyboard_applet.h`
- `app/src/main/cpp/skyline/applet/swkbd/software_keyboard_applet.cpp`
- `app/src/main/cpp/skyline/applet/swkbd/software_keyboard_inline.h`
- new focused inline frontend header/source under `app/src/main/cpp/skyline/applet/swkbd/`
- `app/src/main/cpp/skyline/jvm.h`
- `app/src/main/cpp/skyline/jvm.cpp`
- `app/src/main/java/org/stratoemu/strato/applet/swkbd/InlineKeyboardInputView.kt`
- `app/src/main/cpp/skyline/services/am/applet/ILibraryAppletAccessor.cpp`
- `app/src/main/cpp/skyline/services/visrv/IApplicationDisplayService.cpp`
- `app/CMakeLists.txt` only if a new C++ source file requires registration.

Files outside this set require concrete evidence that the inline frontend cannot be completed without them.

## Validation

### Static/protocol validation

- all known protocol structs retain exact `static_assert` sizes;
- request/reply values match the reference ABI;
- `git diff --check` is clean;
- no changes appear in NCE, scheduler, kernel, GPU, NCA, audio, or unrelated services;
- no sleep, polling loop, timeout, title ID, or title-name condition is introduced.

### Build validation

- complete Android CI/build succeeds on the branch;
- no warnings/errors introduced by the new frontend boundary or JNI signatures.

### Device validation

Hitman: Blood Money - Reprisal:

1. reach name entry;
2. inline IME opens;
3. type multiple characters;
4. delete characters;
5. move cursor and edit in the middle of the string;
6. confirm and advance past name entry;
7. reopen the keyboard;
8. cancel without hanging or terminating the title.

Normal `AllForeground` SWKBD title:

1. open the existing dialog;
2. type;
3. confirm;
4. reopen and cancel;
5. verify validation behavior is unchanged.

Wall World 2:

1. exercise its keyboard path;
2. verify no regression in opening, input, submit, and return to gameplay.

## Success criteria

The rebuild is complete only when:

- the inline protocol is internally coherent and no longer depends on incremental diagnostic patches;
- Hitman passes the name-entry point using Strato's custom inline frontend;
- normal foreground SWKBD remains behaviorally unchanged;
- Wall World 2 does not regress;
- the implementation has no game-specific hack, artificial wait, polling loop, or unrelated subsystem change.

A green CI build alone is not considered proof of runtime completion; device behavior is the final validation for the Hitman path.

## Non-goals

- implementing Nintendo's real firmware SWKBD applet;
- implementing a real VI indirect-layer renderer;
- modernizing all AM/applets;
- changing scheduler/kernel behavior;
- implementing complete Nintendo user/custom dictionary backends when the reference custom frontend does not provide them;
- changing Strato's existing normal software-keyboard UI.
