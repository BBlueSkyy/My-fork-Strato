# NGC dictionary backend

Run `bash tests/ngc_dictionary/run.sh`. Tests generate synthetic Aho-Corasick
archives in the original firmware serialization format, gzip them, and exercise
the production parser and filter. The fixtures contain invented tokens; no
Nintendo dictionaries are included. No Python packages are required.

The backend takes raw gzip archives named `ac_common_{not_b,b1,b2}_nx` and
`ac_<0..15>_{not_b,b1,b2}_nx`, extracted unchanged from system data title
`0100000000000823`. It loads all three files for each requested region, as the
reference does. Missing or corrupt data is an error, never an all-clean result.
Version and source discovery belong to the content provider and IPC facade.

Implemented matching follows Ryujinx's `Sdk/Ngc/Detail` at commit
`534f92506bf8c6aba29bb86917bf10954a360d8e` (MIT): succinct dictionary decoding,
Aho-Corasick failure and output links, ASCII invariant lowercase, common and
regional Check masks, literal word boundary markers, partial/separator-removed/
whole-word Mask passes, both mask modes, email state machine, and the first
512-character mask limit. Rank/select accelerators are bounds-checked and skipped;
their underlying vectors are decoded directly instead.

Supported input is ASCII. Valid non-ASCII Check input, any non-ASCII Mask input,
canonicalized Mask flags, undefined mask/skip modes, matched spans of 255 bytes
or longer, and whole-word candidates exceeding the reference's 64-byte buffer
return `Unsupported` without committing changed output. Check's flags are unused
in the reference. Check rejects malformed UTF-8 before reading dictionaries.
There is no fallback list, guessed Unicode normalization, or guessed clean result.
The IPC facade maps `Unsupported` to SF NotSupported.

Parser limits are 64 MiB compressed and decompressed per archive. Dictionaries
are loaded for each operation; Reload therefore sees current provider contents.
