# CPU pushbuffer producer ordering

Run `python3 tests/gpu_pushbuffer_sequence/run.py` (C++20, UBSan).

Uses the unchanged production mapping/flush/materialization block from GPFIFO::Process with host GMMU/executor/interval interfaces. The producer changes guest words during the existing flush. Fragmented snapshots must see the new words; clean and contiguous paths preserve their existing copy/flush costs, and deferred macro dirty flags remain unchanged.

Negative control: `PUSHBUFFER_SEQUENCE_BASELINE_REF=9935f133 python3 tests/gpu_pushbuffer_sequence/run.py` fails because the fragmented snapshot contains old words. Does not execute command parsing or GPU work.
