# Eden SWKBD Frontend Port Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Port Eden's working Android custom SWKBD frontend behavior into Strato so inline keyboard input works without VI indirect-layer rendering, with Hitman: Blood Money - Reprisal as the primary runtime reproducer.

**Architecture:** Keep `SoftwareKeyboardApplet` as the owner of Horizon ABI/state/replies. Add only the missing inline protocol/types and a small JNI/Android input adapter through the existing `JvmManager`; the Android side emits text/cursor/enter/cancel events and never serializes HOS replies itself. Preserve the existing normal foreground dialog path.

**Tech Stack:** C++20, Strato AM/applet storage, JNI, Kotlin/Android IME, Gradle Android build.

**Spec:** `docs/superpowers/specs/2026-09-16-swkbd-eden-frontend-port-design.md`

## Global Constraints

- Start from current `master` only; do not merge #151, #154, #181 or #182 wholesale.
- No game-specific conditions, sleeps, forced completion, fake fixed handles, or unrelated AM/GPU/kernel changes.
- Do not require VI indirect-layer rendering for the custom frontend path.
- Preserve Strato's existing normal foreground SWKBD behavior unless a concrete bug is required for the port.
- Keep Android input asynchronous from the guest IPC thread.
- Preserve SPDX/copyright notices for directly adapted Eden behavior.

## Execution note

The initial draft proposed polling Java input from C++. That was replaced before implementation with Eden's direct callback model: Android IME events enter native code immediately, `JvmManager` routes them through a lifetime-safe weak applet callback, and `IApplet`'s existing thread-safe interactive output queue signals the guest. This avoids requiring another guest IPC call before a keyboard event can be delivered.

---

### Task 1: Inline SWKBD ABI and state machine

- [x] Add exact 0x8 InitializeArg and 0x4A0/0x4E8 Calc layouts with compile-time assertions.
- [x] Implement Eden ordering for Default/FinishedInitialize, appear/disappear and Finalize.
- [x] Implement UTF-8/UTF-16 and V2 ChangedString/MovedCursor plus Enter/Cancel replies.
- [x] Keep dictionary operations minimal and ABI-correct; no invented dictionary data.

### Task 2: Minimal JNI frontend boundary

- [x] Add a compact `JvmManager::InlineKeyboardUpdate` and callback registration.
- [x] Route Android events directly to the live applet through a weak applet callback.
- [x] Add show/update/hide/close JNI calls without introducing Eden global frontend plumbing.

### Task 3: Android IME adapter

- [x] Add `InlineKeyboardInputView` with a normal Android `InputConnection`.
- [x] Attach it dynamically to the activity content view; no layout XML or normal keyboard dialog changes.
- [x] Emit changed/cursor/enter/cancel events to native code asynchronously.
- [ ] Validate Kotlin/JNI signatures in Android CI.

### Task 4: Validation and PR

- [ ] Run Android PR build and static diff checks.
- [ ] Verify final diff has no scheduler/kernel/GPU/NCA, `ILibraryAppletAccessor` or VI changes.
- [ ] Generate the first testable APK.
- [ ] Device test: Hitman open name-entry -> type -> delete -> confirm -> reopen -> cancel.
- [ ] Regression: normal foreground SWKBD and Wall World 2 keyboard path.
- [ ] If runtime still stops, use the first new request/reply/JNI evidence only; do not add speculative indirect-layer work.
