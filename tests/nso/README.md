# NSO host regressions

```sh
git submodule update --init app/libraries/fmt app/libraries/lz4 app/libraries/zstd
# If the Boost superproject is populated, use its libs/regex/include directory.
# A standalone alternative used for verification:
git clone --depth 1 --branch boost-1.85.0 https://github.com/boostorg/regex.git /tmp/strato-nso-boost-regex
NSO_BOOST_INCLUDE=/tmp/strato-nso-boost-regex/include tests/nso/run.sh
NSO_BOOST_INCLUDE=/tmp/strato-nso-boost-regex/include NSO_SANITIZE=1 tests/nso/run.sh /tmp/strato-nso-asan
```

The script compiles the exact production `nso.cpp`, `nso.h`, `zbic.cpp`,
`zbic_zstd.c`, LZ4 and `Backing` using a symlinked source root. Only the
Android/kernel loader target and mod collectors are shimmed. Tests invoke
`NsoLoader::LoadNso` and inspect the executable delivered to the host loader.
The production Boost regex diagnostic scanner also runs.

Coverage includes valid raw/LZ4/ZBIC routing, malformed and short-output LZ4,
compression flags with zero compressed size, short header/segment reads,
file range checks before allocation, LZ4 signed-size limits, wrapped symbol
ranges, and bounded module-path diagnostics. The ZBIC fixture is a valid raw
block frame; it does not claim coverage of BIC entropy tables. Existing ZBIC
implementation is unchanged.

The default run caps virtual memory at 512 MiB so multi-GiB invalid lengths
produce a failed assertion on the original loader instead of exhausting the
host. ASAN uses its own virtual address space and omits the three giant-length
cases; the default run covers those. All other cases run under ASAN/UBSAN.
To run one case directly, pass its printed name to the built `nso_tests` binary.

Reference comparison: Atmosphere commit
`6b0c077b4c608a3a2ee712b8dc35fee87ceb62e8`,
`stratosphere/loader/source/ldr_process_creation.cpp`,
`LoadAutoLoadModuleSegment` independently checks signed lengths, exact file
reads, and exact LZ4 output. The fixes here follow local failing tests and do
not copy that implementation. Verified standalone Boost.Regex 1.85.0 commit:
`4cbcd3078e6ae10d05124379623a1bf03fcb9350`.

These host tests do not execute guest code or prove an Android game launch.
