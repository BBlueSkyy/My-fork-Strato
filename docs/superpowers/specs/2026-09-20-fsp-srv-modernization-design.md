# fsp-srv Modernization Design

## Goal

Modernize Strato's existing `fsp-srv` implementation by correcting confirmed ABI, validation, error-propagation, read-only, save-data enumeration, and lifecycle defects. The implementation must fit the current Strato VFS and must not change NCA parsing or storage construction.

Success means that supported filesystem operations expose their real behavior and failures, unsupported operations do not report false success, the Android build remains valid, and the protected NCA implementation is absent from the branch diff.

## Scope

The implementation covers:

- `IFileSystem`, including the host-backed VFS operations it consumes;
- `IStorage`, including backing capability checks;
- `ISaveDataInfoReader` and the existing directory-backed save layout;
- the currently registered `IFileSystemProxy` commands;
- `IMultiCommitManager`;
- filesystem IPC structs and FS Results required by those interfaces.

`IFile` and `IDirectory` may receive narrowly required fixes when `IFileSystem`, commit, mode, flush, or iteration correctness depends on them. This is not a general modernization of those interfaces.

## Protected NCA Boundary

This branch must not modify:

- `nca.cpp` or `nca.h`;
- `compressed_storage.*`;
- `sparse_storage.*`;
- BKTR implementation files;
- AES-CTR-Ex implementation;
- Patch Meta Hash implementation;
- `region_backing.*`;
- Program base/update storage construction, ordering, parsing, or bounds.

Existing `currentProcessRomFs`, `patchDataRomFs`, and DLC/Data backings are consumed as already constructed. Suspected defects below this boundary are documented rather than fixed here.

## Confirmed Defects

The current implementation has the following confirmed divergences:

- `SetCurrentProcess` consumes the raw PID placeholder instead of the PID sent in the IPC handle descriptor.
- `CreateFile` parses its size with the wrong width and does not validate signed input.
- host filesystem mutations discard their actual outcome, while recursive operations bypass the backing root entirely;
- guest paths are not consistently bounded, normalized, or confined to the filesystem root;
- free and total space are hardcoded;
- timestamps use nanoseconds only, have the wrong field order, and ignore `stat` failure;
- `FileSystemAttribute` has the wrong ABI size and reports an all-zero fake structure;
- the `IStorage` surface lacks supported commands and signed range validation;
- the read-only save command returns a writable filesystem;
- save-data readers discard their requested filter and report an empty database;
- cache-storage size is hardcoded without parsing its index or establishing the exact data source;
- data-storage lookup incompletely validates `StorageId` and can construct an object with a null backing;
- multi-commit `Add` and `Commit` are no-op successes.

## VFS Design

`vfs::FileSystem` and `vfs::Backing` receive only the capabilities required by the service layer. Existing read-only NCA backings remain read-only by default.

Filesystem operations return a small VFS-local status that can represent success, missing/already-existing paths, invalid paths or modes, permission denial, unsupported operations, non-empty directories, and host I/O failure. `fsp-srv` maps only confirmed statuses to confirmed FS Results. Unknown host failures remain failures; they are not collapsed into success.

The filesystem capability surface includes only what this work needs: rooted path resolution, mutation outcomes, recursive clean/delete, commit/flush, space information, timestamps, and filesystem attributes. Defaults are unsupported. `OsFileSystem` implements the host-backed capabilities.

The backing capability surface includes only explicit writable, resizable, and flushable behavior. `Write` and `SetSize` check those capabilities before invoking implementation methods. NCA-backed `Backing` objects retain their current read-only modes and cannot become writable through this API.

## Path and Host-Filesystem Rules

All guest paths are validated before VFS use. Input buffers must contain a valid bounded path, and normalized resolution must remain below the `OsFileSystem` root. Parent traversal, absolute host paths, invalid buffer layouts, and paths that escape the root are rejected.

Recursive delete and clean are implemented through the rooted VFS operation. Service handlers never pass a guest path directly to `std::filesystem` or host syscalls.

Host errors are captured at the VFS boundary and translated to the closest confirmed FS Result. No mutating operation returns success after the host reports failure.

## IFileSystem

IPC parsing uses the confirmed libnx/Switchbrew layouts and signed widths. Modes are validated before conversion to VFS modes. Read-only instances reject create, delete, rename, clean, writable open, and append operations without changing the original backing.

Free and total space come from the host filesystem for `OsFileSystem`. Filesystems that cannot supply meaningful space information return a confirmed unsupported result rather than an estimate.

Timestamps use POSIX seconds in the ABI order `created`, `modified`, `accessed`, followed by the validity byte and padding. Failure to obtain timestamps is propagated.

`FileSystemAttribute` uses the full ABI layout. A capability flag is set only when the implementation can provide and enforce the corresponding limit. Unknown fields remain undefined and zero.

Commit delegates to the backing filesystem capability. Immediate-write filesystems may flush supported host state, but the service does not claim Horizon journaling semantics.

## IStorage

Every signed offset and size is validated before conversion to `size_t`. Validation rejects negative values, integer overflow, and ranges beyond the backing size. IPC buffer length must be sufficient for the requested transfer.

`Read`, `Write`, `Flush`, `SetSize`, and `GetSize` are exposed only with confirmed ABI. `Write`, `Flush`, and `SetSize` use backing capabilities and return a confirmed failure for read-only or unsupported backings. `OperateRange` is not approximated; it remains unimplemented unless both its requested operation and backing semantics are confirmed during implementation from already selected references.

## Save Data

Save-data IPC enums and structures use their wire widths and explicit padding. Each `ISaveDataInfoReader` owns its enumerated entries, filter, and cursor.

Readers enumerate only directory-backed entries whose required `SaveDataInfo` fields can be reconstructed from the existing Strato path layout and filesystem metadata. Malformed or ambiguous directories do not produce invented records. If the current layout lacks a required identifier for a category, that category remains a documented limitation rather than receiving fabricated metadata.

The unfiltered reader covers supported spaces, the space-specific reader restricts enumeration to its requested `SaveDataSpaceId`, and the cache-only reader emits only reliably reconstructable cache entries. Pagination is bounded by complete `SaveDataInfo` records in the output buffer.

`GetCacheStorageSize` is implemented only after confirming whether the command returns configured capacity, current allocation, or another quantity for the requested cache index. Existing NACP values are used only if they exactly match that confirmed contract.

## IFileSystemProxy

`SetCurrentProcess` records `request.pid`. Save-data commands parse the one-byte space ID plus ABI padding explicitly. Invalid space, type, index, storage ID, missing loader data, or missing backing returns a confirmed failure.

`OpenReadOnlySaveDataFileSystem` creates an `IFileSystem` view that enforces read-only behavior at the service/VFS boundary while sharing the original filesystem object.

Program, patch, DLC, and Data storage commands preserve the existing persistent backings. They validate inputs and never register an `IStorage` around a null backing. No NCA construction logic is moved into `fsp-srv`.

Global access-log mode keeps real per-service state rather than returning an unrelated constant.

## IMultiCommitManager

`Add` consumes and retains valid `IFileSystem` service objects for the lifetime of the manager. Duplicate handling and maximum membership follow confirmed reference behavior when representable.

`Commit` invokes the supported commit operation for each registered filesystem and stops on the first failure. A failure is propagated to the guest. Earlier filesystems may already have committed; the implementation and PR description explicitly state that Strato does not provide Horizon-atomic multi-filesystem transactions.

If the IPC layer cannot safely retain the supplied object, `Add` returns a confirmed failure rather than accepting it as a no-op.

## Modern Commands

No command is added merely because it exists in a newer HOS version. A command is added only when its command ID, input/output ABI, Results, lifecycle, and backend semantics are all known and representable by the current architecture. Otherwise it remains a documented limitation without an approximate response.

## Verification and Delivery

Implementation proceeds in logical commits for ABI/Results, VFS and `IFileSystem`, `IStorage`, save data, proxy behavior, and multi-commit where those blocks produce independent changes.

Before push:

1. inspect `git status`;
2. run `git diff --check`;
3. inspect `git diff --name-only master...HEAD` and the worktree diff;
4. confirm no protected NCA file changed;
5. run the sufficient Android build available in the environment;
6. fix only errors introduced by this branch.

If the full local build is blocked by an environment limitation, a clean semantic review and `git diff --check` are sufficient to push for repository CI validation, provided no concrete code error remains.

After verification, push `fix/fsp-srv-modernization` and open a PR against `master`. The PR records corrected behavior, modernized commands, removed fake-success paths, deliberate limitations, references, build evidence, and the explicit protected-NCA confirmation.
