# Translation identity receipts

`ASAN_OPTIONS=detect_leaks=0 python3 tests/gpu_mapping_identity/run.py`

Tests interval split-preserved generations, IOVA ABA, publication/mutation
serialization, owner destruction, the 65,536-cell ceiling, failed-split identity
preservation/recovery, the 4,096-fragment receipt ceiling and reclamation.
A temporary test copy of the production header adds one scheduler boundary for
the concurrent validity-cache race; production contains no hook. Set
`IDENTITY_VALIDITY_BASELINE=1` for the negative control, which must fail.

These identities describe translations. They do not identify kernel physical
allocation lifetimes; see `docs/region-stage-b.md` for the full B1 blocker.
