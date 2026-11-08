# NGC firmware content reader

Run `bash tests/ngc_content/run.sh`. The production reader and host filesystem/
RomFS implementations run with AddressSanitizer and UndefinedBehaviorSanitizer.
LeakSanitizer defaults off because this environment disallows its thread inspection.

Coverage includes lazy missing-data lookup without creating directories, original
file bytes and big-endian version, malformed filenames, bounded reads, reload,
extracted-source precedence, correct title/type selection, flat and directory
registered archives, ambiguous firmware rejection, and malformed RomFS cycles,
oversized names and table/file ranges. The sibling-cycle test hung before the
scoped RomFS prewalk was added; it now fails closed.

The NCA header/decryption is a host facade here, explicitly using an invented
envelope. It exercises source selection without keys; the enclosed RomFS and
its file reads are real. `bash tests/vfs/run.sh` separately covers the production
NCA parser, including encrypted and compressed/sparse storage. No console keys
or Nintendo dictionary files are bundled. Actual firmware import and phone
validation remain pending.
