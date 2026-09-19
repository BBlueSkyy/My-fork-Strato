# Focused kernel regression checks

These tests do not establish game compatibility. Run from the repository root.
They exercise the production helper/template code and the actual assembled NCE
save/restore instructions, without replacing AArch64/NCE in the emulator.

## Waits and inheritance

```sh
g++ -std=c++20 -g -pthread -fsanitize=thread -Iapp/src/main/cpp/skyline tests/kernel/condition_variable.cpp -o /tmp/strato-cv-test
/tmp/strato-cv-test
g++ -std=c++20 -g -pthread -fsanitize=undefined -Iapp/src/main/cpp/skyline tests/kernel/synchronization.cpp -o /tmp/strato-sync-test
/tmp/strato-sync-test
```

The CV test forces the production fallback path, including ownership on wake,
1,000 wake cycles and timeout/deadline boundaries. The inheritance test exercises
four-thread chains, donor removal, base-priority updates and cycle termination;
arbiter tests cover signed and wake-count boundaries.

## NCE assembly

Install Python `unicorn` and `pyelftools`, then assemble `guest.S` with the Android
NDK AArch64 compiler or GNU AArch64 assembler:

```sh
aarch64-linux-gnu-as app/src/main/cpp/skyline/nce/guest.S -o /tmp/strato-guest.o
python tests/kernel/nce_context.py /tmp/strato-guest.o
```

The test runs the machine code in Unicorn and checks GP, SP, all 32 vector
registers, NZCV, FPCR/FPSR, and the exact SaveCtx/LoadCtx instruction counts.
It does not test real signal delivery, host/guest races, or pause acknowledgement.

## Android and device validation

Use the existing PR workflow (`assembleDevRelease assembleDevReldebug`), then test
two titles already working on master before investigating 2–3 problem titles.
Record APK commit, title/update, device/driver, settings and the exact failure
point. Include normal ExitThread/ExitProcess and starting a second game in the
same app session. Report game behavior as **IMPLEMENTADO MAS NÃO VALIDADO** until
there is direct evidence of correct behavior and no observed regression.
