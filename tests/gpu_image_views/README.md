# Image-view descriptor components

Run `python3 tests/gpu_image_views/run.py` to compile the exact production
`TextureView::GetView` body with real Vulkan structures. Only the Vulkan object
creation boundary is a host facade; the test observes the actual create-info
components and checks sampled/storage cache isolation, backing cache reuse,
hot-path allocation counts, and generation invalidation. UBSan is enabled.

Negative control:

```sh
TEXTURE_IDENTITY_BASELINE_REF=5bf1a37572c531b7a3213b749b97113907ae6e60 python3 tests/gpu_image_views/run.py
```

This substitutes the complete baseline production body, adapting only its
no-argument signature. It must fail with `storage view reused a sampled TIC
swizzle`. This is a host behavior regression, not a mobile-driver validation
or game-acceptance run.
