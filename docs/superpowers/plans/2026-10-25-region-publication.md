# Region publication continuation from f60370d1

Keep the checkpoint and its existing authority implementation. Replace long component lock retention with resource-first preparation and short version-validated publication.

- Attach resolves/initializes resources without retaining component locks. Collect only regional resources used by this operation (inline capacity; no manager scan).
- At publication acquire state locks then distinct component locks in stable order, after all resource/view acquisition. Reject CpuDirty or mismatched resident/authority versions before any writer or new render-pass nodes are published. Keep the guard through the combined RT/storage claims, outside callbacks that may acquire resources.
- Retained read-only storage descriptors must also be preflighted. A prepared regional host snapshot must survive a later CPU write before SubmitInternal; skip its late inline upload, retain CpuDirty and reconcile on retry/next use after submission.
- RED: keep lock-order oracle. Add CPU changes between preflight/publication, callback exclusion during the short guard, and donor-set replacement while locks wait. Validate actual Vulkan bytes, source lock/submission ordering, failure cleanup and common paths.
- Re-run essential existing tests, independent review, then append commits to the same Draft #400 only if gates pass. No workflow, Android build mode or Actions changes.
