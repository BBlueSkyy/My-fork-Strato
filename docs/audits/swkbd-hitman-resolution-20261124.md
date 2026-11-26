# Hitman SWKBD resolution and diagnostic cleanup

The user confirmed that Hitman: Blood Money — Reprisal now opens the inline Android keyboard and saves a profile on the Moto G100 after commit `c5f4c6dd43fe94c57b7ca5a6a58c1cabc88d39b9`. That commit only removed the unconditional success assignment in SVC ResetSignal, preserving InvalidState for an unsignalled event. The original Skyline commit `11c5f50d37faf3ef798d5e6efb9d6324019c3f58` introduced the conditional result but left the final success assignment in place.

This cleanup removes temporary SWKBD diagnostics and their dedicated trace tests, preserving functional protocol, lifecycle, frontend, VI IPC and regression coverage. Previous audit documents are historical and describe the instrumented builds; they are not instructions to collect traces from the clean build. No hardware validation of the cleaned APK or additional kernel conformance testing is claimed here. Existing CI must validate this revision before merge.
