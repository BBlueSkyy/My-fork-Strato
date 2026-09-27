# fsp-srv Modernization Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace confirmed `fsp-srv` ABI errors, unsafe host/VFS behavior, hardcoded responses, and no-op lifecycle behavior with bounded, capability-aware implementations that preserve all existing NCA storage code.

**Architecture:** Keep IPC structs and Result mapping in `services/fssrv`, and add only the host-filesystem/backing capabilities that those handlers must call. `OsFileSystem` owns rooted host-path resolution and reports `std::error_code`; `IFileSystem` provides a read-only view without cloning the VFS; `Backing` enforces its existing mode before write/resize/flush.

**Tech Stack:** C++20, Strato CMIF service framework, POSIX filesystem APIs, existing host VFS test runner, Android Gradle/NDK build.

**Spec:** `docs/superpowers/specs/2026-09-20-fsp-srv-modernization-design.md`

## Global Constraints

- Work only on `fix/fsp-srv-modernization`, based on the current `master`.
- Do not modify NCA parsing, Sparse Storage, Compressed Storage, BKTR, AES-CTR-Ex, Patch Meta Hash, RegionBacking, Program NCA layering, storage ordering, bounds, or backing creation.
- Do not modify scheduler/kernel, NCE, GPU/texman, nvdrv, HID, audio/hwopus, SSL, NIFM, AM/applet, SWKBD, settings, or timesrv.
- Keep `vfs::FileSystem` and `vfs::Backing` extensions minimal and capability-oriented.
- Validate signed IPC values, addition overflow, backing bounds, and IPC-buffer bounds before unsigned conversion.
- Never turn unsupported behavior into success and never approximate `OperateRange`, cache sizes, save metadata, or atomic multi-commit semantics.
- Preserve the existing `currentProcessRomFs`, `patchDataRomFs`, and DLC/Data backing objects.
- Commit each independently reviewable block and run `git diff --check` plus the protected-file audit before push.

## Review Focus

- Guest paths containing `..`, absolute host syntax, embedded data after a missing NUL, or symlink escapes must fail without touching anything outside the mount root; Task 2 tests each case.
- Signed offsets/sizes at `-1`, `INT64_MAX`, exact end-of-storage, and one byte past end must be rejected or accepted according to the exact range; Task 4 tests the shared validator and read-only backing behavior.
- Read-only save filesystem views must reject every mutation and writable/append open while preserving reads through the same filesystem object; Task 3 tests both sides.
- Repeated directory and save-info reads must advance cursors without underflow, duplication, or partial-record writes; Tasks 3 and 5 test pagination.
- A multi-commit failure after an earlier success must return that failure and must not claim rollback or atomicity; Task 7 tests ordered stop-on-first-failure behavior.

---

### Task 1: ABI structs, Results, and pure validation helpers

**Files:**
- Create: `app/src/main/cpp/skyline/services/fssrv/types.h`
- Create: `app/src/main/cpp/skyline/services/fssrv/validation.h`
- Modify: `app/src/main/cpp/skyline/services/fssrv/results.h`
- Create: `tests/fssrv/fssrv_tests.cpp`
- Create: `tests/fssrv/host/common.h`
- Create: `tests/fssrv/CMakeLists.txt`
- Create: `tests/fssrv/run.sh`

**Interfaces:**
- Consumes: existing `Result`, fixed-width Strato integer aliases, and `span`.
- Produces: ABI-checked `SaveDataSpaceId : u8`, `SaveDataAttribute`, `SaveDataInfo`, `FileTimeStampRaw`, and `FileSystemAttribute`; `ValidateRange(i64 offset, i64 size, size_t extent)`; `ReadPath(span<u8>)`; confirmed FS Results used by later tasks.

- [ ] **Step 1: Add failing ABI and range tests**

```cpp
static_assert(sizeof(SaveDataAttribute) == 0x40);
static_assert(sizeof(SaveDataInfo) == 0x60);
static_assert(sizeof(FileTimeStampRaw) == 0x20);
static_assert(sizeof(FileSystemAttribute) == 0xC0);
Check(!ValidateRange(-1, 1, 4), "negative offset accepted");
Check(!ValidateRange(0, -1, 4), "negative size accepted");
Check(ValidateRange(4, 0, 4), "zero-size end range rejected");
Check(!ValidateRange(4, 1, 4), "past-end range accepted");
Check(!ValidateRange(INT64_MAX, 1, SIZE_MAX), "offset+size overflow accepted");
```

- [ ] **Step 2: Run the host test and verify the new headers are missing**

Run: `tests/fssrv/run.sh /tmp/strato-fssrv-tests`

Expected: compilation fails because `services/fssrv/types.h` and `validation.h` do not exist.

- [ ] **Step 3: Define the exact wire layouts and validators**

```cpp
enum class SaveDataSpaceId : u8 { System = 0, User = 1, SdSystem = 2, Temporary = 3, SdCache = 4, ProperSystem = 100 };

struct FileTimeStampRaw {
    u64 created;
    u64 modified;
    u64 accessed;
    u8 isValid;
    u8 padding[7];
};

struct FileSystemAttribute {
    std::array<bool, 13> hasValue;
    u8 reserved1[0x1B];
    std::array<i32, 13> value;
    u8 reserved2[0x64];
};

constexpr bool ValidateRange(i64 offset, i64 size, size_t extent) {
    if (offset < 0 || size < 0)
        return false;
    const auto unsignedOffset = static_cast<u64>(offset);
    const auto unsignedSize = static_cast<u64>(size);
    return unsignedOffset <= extent && unsignedSize <= extent - unsignedOffset;
}
```

`ReadPath` accepts one input buffer no larger than `0x301`, requires a NUL byte within that buffer, rejects embedded host separators/backslashes and guest paths that cannot be normalized, and returns `result::InvalidPath` instead of throwing. Add only Results whose FS module/description is confirmed by the selected references: path already exists, target/path missing, not implemented, invalid argument/path/offset/size/open mode, read/write not permitted, permission denied, and unexpected host I/O failure.

- [ ] **Step 4: Run the host tests**

Run: `tests/fssrv/run.sh /tmp/strato-fssrv-tests`

Expected: all ABI `static_assert`s and validator cases pass.

- [ ] **Step 5: Commit the ABI block**

```bash
git add app/src/main/cpp/skyline/services/fssrv/{types.h,validation.h,results.h} tests/fssrv
git commit -m "fix(fssrv): correct filesystem IPC layouts"
```

### Task 2: Rooted host filesystem capabilities

**Files:**
- Modify: `app/src/main/cpp/skyline/vfs/filesystem.h`
- Modify: `app/src/main/cpp/skyline/vfs/os_filesystem.h`
- Modify: `app/src/main/cpp/skyline/vfs/os_filesystem.cpp`
- Modify: `tests/fssrv/fssrv_tests.cpp`
- Modify: `tests/fssrv/CMakeLists.txt`
- Modify: `tests/fssrv/run.sh`

**Interfaces:**
- Consumes: guest paths already bounded by Task 1.
- Produces: mutation methods returning `std::error_code`; `DeleteDirectoryRecursively`, `CleanDirectoryRecursively`, `Commit`, `GetSpace`, `GetFileTimeStamp`, and `GetFileSystemAttribute`; one root-confined resolver used by every `OsFileSystem` operation.

- [ ] **Step 1: Add failing rooted-filesystem tests**

```cpp
TempDirectory root;
OsFileSystem fs(root.path);
Check(!fs.CreateDirectory("inside", true), "rooted create failed");
Check(fs.CreateFile("inside/file", 4) == std::error_code{}, "rooted file create failed");
Check(fs.CreateFile("../escape", 1) == std::errc::invalid_argument, "parent escape accepted");
Check(fs.DeleteDirectoryRecursively("inside") == std::error_code{}, "recursive delete failed");
Check(std::filesystem::exists(root.path) && !std::filesystem::exists(root.path / "inside"), "recursive delete escaped root");
```

Create a symlink below the root that points outside it and verify create/open/delete through that link returns an error. Also verify missing delete, non-empty non-recursive delete, free/total space, timestamp ordering/validity, and clean-recursive preserving the selected directory.

- [ ] **Step 2: Run tests and observe current path escape/outcome failures**

Run: `tests/fssrv/run.sh /tmp/strato-fssrv-tests`

Expected: tests fail because mutations do not return errors, recursive methods are absent, and paths are concatenated directly.

- [ ] **Step 3: Add minimal capability methods to `FileSystem`**

```cpp
virtual std::error_code CreateFileImpl(const std::string &, size_t) { return std::errc::operation_not_supported; }
virtual std::error_code CommitImpl() { return std::errc::operation_not_supported; }
virtual std::error_code GetSpaceImpl(const std::string &, u64 &, u64 &) { return std::errc::operation_not_supported; }
virtual std::error_code GetFileTimeStampImpl(const std::string &, FileTimeStamp &) { return std::errc::operation_not_supported; }
virtual std::error_code GetFileSystemAttributeImpl(FileSystemAttribute &) { return std::errc::operation_not_supported; }
```

Use the same `std::error_code` return shape for create/delete/rename/recursive operations. Keep `OpenFile`, `OpenDirectory`, and `GetEntryType` signatures intact to avoid reworking read-only ROM filesystems; host implementations return `nullptr`/`nullopt` on ordinary missing paths and throw only for unrecoverable programmer errors.

- [ ] **Step 4: Implement a single component-wise resolver in `OsFileSystem`**

```cpp
std::pair<std::filesystem::path, std::error_code> ResolvePath(std::string_view guestPath) const;
```

The resolver strips only the guest leading slash, rejects `..`, backslashes, NULs, overlong components, and paths whose `weakly_canonical(root / relative)` is not component-wise below the canonical root. Every operation, including directory enumeration and recursion, uses the resolved host path. Use `std::filesystem` overloads with `std::error_code`; use `statvfs` for real free/total bytes with multiplication overflow checks; use `stat`/available birth-time support for POSIX-second timestamps without treating `ctime` as creation time; use `pathconf`-derived name limits and only set attribute flags for enforced limits.

- [ ] **Step 5: Run rooted-filesystem tests**

Run: `tests/fssrv/run.sh /tmp/strato-fssrv-tests`

Expected: all root confinement, mutation outcome, recursion, space, and timestamp tests pass.

- [ ] **Step 6: Commit the VFS block**

```bash
git add app/src/main/cpp/skyline/vfs/{filesystem.h,os_filesystem.h,os_filesystem.cpp} tests/fssrv
git commit -m "fix(vfs): confine and report host filesystem operations"
```

### Task 3: Correct `IFileSystem`, read-only views, and directory iteration

**Files:**
- Modify: `app/src/main/cpp/skyline/services/fssrv/IFileSystem.h`
- Modify: `app/src/main/cpp/skyline/services/fssrv/IFileSystem.cpp`
- Modify: `app/src/main/cpp/skyline/services/fssrv/IDirectory.cpp`
- Modify: `tests/fssrv/fssrv_tests.cpp`

**Interfaces:**
- Consumes: Task 1 ABI/types/path parser and Task 2 VFS error-returning capabilities.
- Produces: `IFileSystem(..., bool readOnly = false)` and public `Result CommitBacking()` for multi-commit; complete command behavior for IDs 0–14 and 16.

- [ ] **Step 1: Add failing tests for mode, read-only, error mapping, and pagination helpers**

```cpp
Check(MapVfsError(std::errc::no_such_file_or_directory) == result::PathDoesNotExist, "missing path mapped incorrectly");
Check(MapVfsError(std::errc::file_exists) == result::PathAlreadyExists, "existing path mapped incorrectly");
Check(!IsOpenModeValid(0), "empty open mode accepted");
Check(!IsOpenModeValid(8), "unknown open-mode bit accepted");
Check(IsMutationAllowed(false, {true, false, false}), "read-only open rejected on writable view");
Check(!IsMutationAllowed(true, {true, true, false}), "write open accepted on read-only view");
```

Add a three-entry pagination fixture and assert reads of capacity two return entries `[0,1]`, then `[2]`, then zero without unsigned underflow.

- [ ] **Step 2: Run tests and verify helper/behavior failures**

Run: `tests/fssrv/run.sh /tmp/strato-fssrv-tests`

Expected: compilation or assertions fail before service changes.

- [ ] **Step 3: Implement exact IPC parsing and read-only enforcement**

```cpp
struct CreateFileInput { u32 option; u32 padding; i64 size; };
static_assert(sizeof(CreateFileInput) == 0x10);

IFileSystem(std::shared_ptr<vfs::FileSystem> backing, const DeviceState &state,
            ServiceManager &manager, bool readOnly = false);
Result CommitBacking();
```

Reject negative create sizes before `size_t` conversion, reject unsupported create-option bits, validate every path buffer through `ReadPath`, validate file/directory modes, and reject all mutations plus write/append opens on a read-only instance with `WriteNotPermitted`. Map each VFS failure once in `MapVfsError`; do not push output on failed queries. Delegate recursive delete/clean, commit, space, timestamp, and attributes to the VFS. Serialize `FileTimeStampRaw` in created/modified/accessed order and the full `0xC0` attribute layout.

- [ ] **Step 4: Fix `IDirectory` cursor arithmetic**

Use `entries.at(remainingReadCount + i)`, clamp remaining count before subtraction, copy at most `name.size()` into the zero-initialized `0x301` name array, and return zero cleanly after exhaustion.

- [ ] **Step 5: Run host tests and compile the affected Android target**

Run: `tests/fssrv/run.sh /tmp/strato-fssrv-tests`

Run: `./gradlew :app:externalNativeBuildDebug --no-daemon`

Expected: host tests pass; native build compiles the changed service/VFS code.

- [ ] **Step 6: Commit the filesystem-service block**

```bash
git add app/src/main/cpp/skyline/services/fssrv/{IFileSystem.h,IFileSystem.cpp,IDirectory.cpp} tests/fssrv
git commit -m "fix(fssrv): implement filesystem operations faithfully"
```

### Task 4: Enforce backing capabilities and implement bounded storage/file I/O

**Files:**
- Modify: `app/src/main/cpp/skyline/vfs/backing.h`
- Modify: `app/src/main/cpp/skyline/vfs/os_backing.h`
- Modify: `app/src/main/cpp/skyline/vfs/os_backing.cpp`
- Modify: `app/src/main/cpp/skyline/services/fssrv/IStorage.h`
- Modify: `app/src/main/cpp/skyline/services/fssrv/IStorage.cpp`
- Modify: `app/src/main/cpp/skyline/services/fssrv/IFile.cpp`
- Modify: `tests/fssrv/fssrv_tests.cpp`

**Interfaces:**
- Consumes: Task 1 `ValidateRange` and FS Results.
- Produces: guarded `Backing::Write`, `Resize`, and `Flush`; `IStorage` commands Read(0), Write(1), Flush(2), SetSize(3), GetSize(4); corrected `IFile` buffer-length behavior. `OperateRange` remains unregistered because no representable semantics were confirmed.

- [ ] **Step 1: Add failing read-only and range tests**

```cpp
ReadOnlyMemoryBacking ro(4);
Check(!ro.IsWritable(), "read-only backing reports writable");
Check(ro.Write(span(input), 0).error == Backing::Error::WriteNotPermitted, "read-only write reached WriteImpl");
Check(ro.Resize(8).error == Backing::Error::WriteNotPermitted, "read-only resize reached ResizeImpl");
Check(!ValidateRange(3, 2, 4), "cross-end storage range accepted");
```

Use a writable fixture to assert requested size, rather than full IPC-buffer length, is read/written; exact-end zero-size operations succeed; short host writes fail; resize updates `size`; flush reaches the implementation.

- [ ] **Step 2: Run tests and observe current read-only/range failures**

Run: `tests/fssrv/run.sh /tmp/strato-fssrv-tests`

Expected: tests fail because `Backing::Write` calls `WriteImpl` after only logging and no flush capability exists.

- [ ] **Step 3: Add explicit backing operation results**

```cpp
enum class OperationResult { Success, ReadNotPermitted, WriteNotPermitted, OutOfRange, Unsupported, IoError };
OperationResult Write(span<u8> input, size_t offset);
OperationResult Resize(size_t size);
OperationResult Flush();
virtual OperationResult FlushImpl() { return OperationResult::Unsupported; }
```

Check mode/capability before implementation calls. Do not change any derived NCA backing: their existing non-writable modes cause rejection in `Backing` itself. `OsBacking::FlushImpl` calls `fsync`; resize is allowed only when the open mode is writable.

- [ ] **Step 4: Implement `IStorage` with exact signed ABI**

For Read/Write, pop `i64 offset` then `i64 size`, validate with `ValidateRange`, require an output/input buffer of at least `size`, operate on exactly the first `size` bytes, and propagate backing operation failures. `Flush` delegates to `Backing::Flush`; `SetSize` pops `i64`, rejects negatives and unrepresentable values, then calls `Resize`; `GetSize` pushes `i64`. Register only commands 0–4.

- [ ] **Step 5: Correct the same size/capability defects in `IFile`**

Read and write only the requested subspan, validate buffer presence/size and range overflow, delegate flush, parse `SetSize` as signed, and return read/write permission Results rather than logging success.

- [ ] **Step 6: Run tests and native compile**

Run: `tests/fssrv/run.sh /tmp/strato-fssrv-tests`

Run: `./gradlew :app:externalNativeBuildDebug --no-daemon`

Expected: backing capability/range tests pass and changed service code compiles.

- [ ] **Step 7: Commit the storage block**

```bash
git add app/src/main/cpp/skyline/vfs/{backing.h,os_backing.h,os_backing.cpp} app/src/main/cpp/skyline/services/fssrv/{IStorage.h,IStorage.cpp,IFile.cpp} tests/fssrv
git commit -m "fix(fssrv): validate and enforce storage operations"
```

### Task 5: Save-data ABI, path validation, and honest reader state

**Files:**
- Modify: `app/src/main/cpp/skyline/services/fssrv/IFileSystemProxy.h`
- Modify: `app/src/main/cpp/skyline/services/fssrv/IFileSystemProxy.cpp`
- Modify: `app/src/main/cpp/skyline/services/fssrv/ISaveDataInfoReader.h`
- Modify: `app/src/main/cpp/skyline/services/fssrv/ISaveDataInfoReader.cpp`
- Modify: `tests/fssrv/fssrv_tests.cpp`

**Interfaces:**
- Consumes: Task 1 save-data wire structs and Task 3 read-only `IFileSystem` constructor.
- Produces: validated `GetSaveDataPath`; reader constructor taking optional `SaveDataSpaceId` and cache-only flag; per-instance `entries` and `cursor`; complete-record bounded pagination.

- [ ] **Step 1: Add failing save-data tests**

```cpp
Check(GetSaveDataPath(SaveDataSpaceId::User, account, programId).has_value(), "valid account path rejected");
Check(!GetSaveDataPath(static_cast<SaveDataSpaceId>(0xFF), account, programId), "invalid space accepted");
Check(!GetSaveDataPath(SaveDataSpaceId::User, unknownType, programId), "unknown save type accepted");
SaveDataInfoReaderState state({entry0, entry1});
Check(state.Read(span<SaveDataInfo>(one)).count == 1, "first page count wrong");
Check(state.Read(span<SaveDataInfo>(one)).entries[0].saveDataId == entry1.saveDataId, "reader cursor did not advance");
```

Add cases for a buffer smaller than `sizeof(SaveDataInfo)`, a non-multiple trailing region, space filtering, and cache-only filtering.

- [ ] **Step 2: Run tests and observe absent validation/state**

Run: `tests/fssrv/run.sh /tmp/strato-fssrv-tests`

Expected: compilation/assertions fail because path construction throws and readers have no state.

- [ ] **Step 3: Make save path construction non-throwing and explicit**

Return `std::optional<std::string>` (or a small result carrying the confirmed FS error), validate `SaveDataSpaceId`, `SaveDataType`, rank, index, and the required user/program/save IDs before formatting. Parse proxy IPC as `{u8 spaceId; u8 padding[7]; SaveDataAttribute attribute}` instead of relying on a `u64` enum.

- [ ] **Step 4: Implement reader state without fabricated records**

```cpp
ISaveDataInfoReader(const DeviceState &, ServiceManager &,
                    std::optional<SaveDataSpaceId> spaceFilter, bool onlyCache);
std::vector<SaveDataInfo> entries;
size_t cursor{};
```

Scan only the existing Strato save roots selected by the instance filter. Emit a record only if every field in `SaveDataInfo`—including save-data ID, raw image size, index, rank, program/user IDs, and type—is recoverable from durable existing metadata. The current directory-only layout does not preserve save-data ID, raw image size, index, or rank for account/cache saves, so those directories are skipped rather than guessed. `ReadSaveDataInfo` writes `floor(buffer_size / 0x60)` complete records, advances `cursor`, and pushes a `u64` count; an empty reliable set returns successful zero.

- [ ] **Step 5: Wire the three reader factories and read-only saves**

Command 60 constructs an unfiltered reader, command 61 pops/validates one `u8` space ID, and command 62 constructs a cache-only reader. Command 53 parses the same save-open input as command 51 but registers `IFileSystem(backing, ..., true)` around the same `shared_ptr<vfs::FileSystem>`.

- [ ] **Step 6: Run tests and native compile**

Run: `tests/fssrv/run.sh /tmp/strato-fssrv-tests`

Run: `./gradlew :app:externalNativeBuildDebug --no-daemon`

Expected: save path/filter/pagination tests pass and service code compiles.

- [ ] **Step 7: Commit the save-data block**

```bash
git add app/src/main/cpp/skyline/services/fssrv/{IFileSystemProxy.h,IFileSystemProxy.cpp,ISaveDataInfoReader.h,ISaveDataInfoReader.cpp} tests/fssrv
git commit -m "fix(fssrv): validate save data and preserve reader state"
```

### Task 6: Correct proxy process/storage behavior and remove fake cache success

**Files:**
- Modify: `app/src/main/cpp/skyline/services/fssrv/IFileSystemProxy.h`
- Modify: `app/src/main/cpp/skyline/services/fssrv/IFileSystemProxy.cpp`
- Modify: `tests/fssrv/fssrv_tests.cpp`

**Interfaces:**
- Consumes: Task 4 bounded/read-only `IStorage` and Task 5 save-data parsing.
- Produces: PID from `request.pid`; strict storage-ID/backing validation; persistent program/patch/DLC backing use; non-success for unimplemented cache-size semantics; stored global access-log mode only if its setter is implemented.

- [ ] **Step 1: Add failing pure proxy-input tests**

```cpp
Check(IsDataStorageIdAllowed(StorageId::NandSystem), "system archive storage rejected");
Check(IsDataStorageIdAllowed(StorageId::NandUser), "DLC storage rejected");
Check(!IsDataStorageIdAllowed(StorageId::None), "None storage accepted");
Check(!IsDataStorageIdAllowed(static_cast<StorageId>(0xFF)), "unknown storage accepted");
```

- [ ] **Step 2: Run tests and verify validation is absent**

Run: `tests/fssrv/run.sh /tmp/strato-fssrv-tests`

Expected: compilation/assertion failure.

- [ ] **Step 3: Correct current-process and storage commands**

`SetCurrentProcess` consumes/ignores the raw placeholder and records `request.pid`. Current-process and patch commands register the existing backing object unchanged. Data-ID lookup validates its one-byte `StorageId` plus seven bytes of padding, restricts the search to storage types represented by the current Strato sources, skips non-files in the system archive directory, catches/propagates missing or invalid archives without manufacturing storage, and never registers an `IStorage` with `nullptr`.

- [ ] **Step 4: Remove fake cache-size and access-log responses**

Parse command 34 as `u16 index`. Because selected Strato/Eden/Ryujinx/Switchbrew references establish the ABI but the existing Strato architecture has neither a cache-save index nor exact allocated data/journal sizes, return the confirmed FS `NotImplemented` Result without pushing sizes; do not substitute NACP maxima. Keep command 1005 at its real initialized service state only if command 1004 is added with confirmed ABI; otherwise return `NotImplemented` rather than an unrelated constant.

- [ ] **Step 5: Run tests and native compile**

Run: `tests/fssrv/run.sh /tmp/strato-fssrv-tests`

Run: `./gradlew :app:externalNativeBuildDebug --no-daemon`

Expected: input tests pass and proxy code compiles without changing NCA construction.

- [ ] **Step 6: Commit the proxy block**

```bash
git add app/src/main/cpp/skyline/services/fssrv/{IFileSystemProxy.h,IFileSystemProxy.cpp} tests/fssrv
git commit -m "fix(fssrv): validate filesystem proxy storage requests"
```

### Task 7: Implement non-atomic multi-commit lifecycle

**Files:**
- Modify: `app/src/main/cpp/skyline/services/fssrv/IMultiCommitManager.h`
- Modify: `app/src/main/cpp/skyline/services/fssrv/IMultiCommitManager.cpp`
- Modify: `tests/fssrv/fssrv_tests.cpp`

**Interfaces:**
- Consumes: Task 3 `IFileSystem::CommitBacking()` and existing `IpcRequest::PopService<IFileSystem>`.
- Produces: retained filesystem vector and sequential stop-on-first-error commit.

- [ ] **Step 1: Add failing ordered-commit tests**

```cpp
CommitSequence sequence({Result{}, result::UnexpectedFailure, Result{}});
Check(sequence.Commit() == result::UnexpectedFailure, "first commit failure not propagated");
Check(sequence.calls == 2, "commit continued after failure");
Check(sequence.committed[0] && !sequence.committed[2], "commit order changed");
```

Also assert manager-owned `shared_ptr`s keep filesystems alive until manager destruction.

- [ ] **Step 2: Run tests and observe the no-op behavior**

Run: `tests/fssrv/run.sh /tmp/strato-fssrv-tests`

Expected: tests fail because no filesystem collection/sequence exists.

- [ ] **Step 3: Retain input service objects and commit sequentially**

```cpp
std::vector<std::shared_ptr<IFileSystem>> fileSystems;

Result IMultiCommitManager::Add(...request...) {
    auto filesystem = request.PopService<IFileSystem>(0, session);
    if (!filesystem)
        return result::InvalidArgument;
    fileSystems.emplace_back(std::move(filesystem));
    return {};
}
```

`Commit` calls `CommitBacking()` in insertion order and returns immediately on the first failure. Do not roll back prior commits and do not expose any success flag that implies Horizon transaction atomicity.

- [ ] **Step 4: Run tests and native compile**

Run: `tests/fssrv/run.sh /tmp/strato-fssrv-tests`

Run: `./gradlew :app:externalNativeBuildDebug --no-daemon`

Expected: lifecycle/order tests pass and multi-commit code compiles.

- [ ] **Step 5: Commit the multi-commit block**

```bash
git add app/src/main/cpp/skyline/services/fssrv/{IMultiCommitManager.h,IMultiCommitManager.cpp} tests/fssrv
git commit -m "fix(fssrv): retain and commit registered filesystems"
```

### Task 8: Final scoped audit, verification, push, and PR

**Files:**
- Modify only concrete defects found in already touched `services/fssrv`, `vfs/filesystem`, `vfs/os_*`, and `tests/fssrv` files.
- Do not modify protected NCA files.

**Interfaces:**
- Consumes: all prior task outputs.
- Produces: clean logical history, pushed branch, and PR against `master`.

- [ ] **Step 1: Search the final scope once for remaining concrete stubs/hardcodes**

Run: `rg -n "TODO|STUB|return \{\};|90000000|remove_all|Push<u64>\(0\)|Push<u32>\(0\)" app/src/main/cpp/skyline/services/fssrv app/src/main/cpp/skyline/vfs/{filesystem.h,backing.h,os_filesystem.*,os_backing.*}`

Expected: each hit is either implemented, an intentional success path, or recorded as a deliberate unsupported limitation; no fake-success remains in commands changed by this branch.

- [ ] **Step 2: Run focused tests**

Run: `tests/fssrv/run.sh /tmp/strato-fssrv-tests`

Expected: all tests pass.

- [ ] **Step 3: Run Android build validation**

Run: `./gradlew :app:assembleDebug --no-daemon`

Expected: build succeeds. If the environment lacks the Android SDK/NDK or another external prerequisite, capture that concrete blocker, keep `git diff --check` clean, and continue to push as explicitly authorized.

- [ ] **Step 4: Run repository hygiene and protected-file checks**

```bash
git status --short --branch
git diff --check master...HEAD
git diff --name-only master...HEAD
git diff --name-only
```

Reject the branch if either name-only output contains `nca.cpp`, `nca.h`, `sparse_storage`, `compressed_storage`, `bktr`, `ctr_encrypted_backing`, `region_backing`, or another protected NCA implementation file.

- [ ] **Step 5: Review the complete diff and create any final scoped fix commit**

Run: `git diff --stat master...HEAD && git diff master...HEAD -- app/src/main/cpp/skyline/services/fssrv app/src/main/cpp/skyline/vfs tests/fssrv`

Expected: every change maps to a confirmed divergence in the spec; no cosmetic or unrelated changes.

- [ ] **Step 6: Push and open the PR**

```bash
git push -u origin fix/fsp-srv-modernization
gh pr create --base master --head fix/fsp-srv-modernization --title "Modernize fsp-srv semantics and validation" --body-file /tmp/fsp-srv-pr.md
```

The PR body lists corrected old behavior, modernized commands, removed fake-success/stubs, deliberate limitations (save metadata, cache-size semantics, non-atomic sequential multi-commit, unimplemented `OperateRange`), reference order, build evidence, and an explicit statement that NCA Sparse/Compressed/BKTR/AES-CTR-Ex/Patch Meta Hash/RegionBacking were not modified.
