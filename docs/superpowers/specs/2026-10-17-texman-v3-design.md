# TextureManager v3, stage 1

Start at master 66637da0; branch wip/texman-v3, separate draft PR; leave #244 unchanged. Preserve existing format, view/subresource, alias reconciliation and Free Guest Texture Memory behavior. No new CopyOnly routes or other subsystem rewrites.

UsageTracker owns two private physical byte range sets: uncompleted GPU writes and writes sequenced in the current execution. Use half-open integer address intervals, coalesce adjacent regions, retain vector capacity and use binary overlap queries. GPU writes affect both sets. Submission clears only execution history; completed wait clears only pending GPU hazards. Consumer changes are API adaptations only.

Texture authority must survive failed transfers. Guest-to-host upload failure leaves guest authoritative and retryable; host-to-guest failure leaves the GPU writer authoritative, including its freed-page status and readback serial. A stale alias cannot claim a new write without reconciliation. Metadata publication occurs only after successful encoding. Continue using existing fences, lock ordering, exact ordered mappings and conservative reversible-layout gates.

Validate production methods and the new tracker, including boundary/empty/fragmented intervals, submission/wait lifetimes, transfer failures, alternating alias writers, CPU traps, view reuse and Free Guest Texture Memory. Build ARM64 APK through PR CI; stop for device tests without merge. Performance on device remains to be measured.
