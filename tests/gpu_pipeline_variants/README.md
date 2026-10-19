# Pipeline variant cache regressions

Run `python3 tests/gpu_pipeline_variants/run.py`. Actual graphics/compute lookup
and graphics transition methods run with compilation-boundary facades. Float,
uint and sint variants must remain distinct and reusable without duplicate
compilation. `PIPELINE_VARIANTS_BASELINE_REF=5bf1a375` supplies the historical
methods and fails the variant-count assertion. No Android or Vulkan device needed.
