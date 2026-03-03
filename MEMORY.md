## Session Notes (2026-03-02)

- Repository is on `master` with **no initial commit yet** (`git rev-parse --verify HEAD` fails).
- `critique --web` currently returns `No changes to display` in this unborn-branch state (no baseline commit to diff against).
- User priorities for current RTX pass:
  1. Match Kokiri dirt path appearance to reference (remove pale/chalk strip)
  2. Water should be transparent + reflective + refractive
  3. GI should feel like multi-bounce (2+), not only single indirect bounce
- Implemented WIP changes for path tuning, water optics weighting, and GI recursion budgeting.
- Added `giMaxBounces` to SceneConstants (C++ + HLSL) and wired it from scene config.

## Bounce Count Change + Build/Launch Workflow Notes (2026-03-03)

- Requested change "4 bounces" requires updating all three layers together:
  1. Scene config value (`RTXSceneConfig.cpp`)
  2. Runtime clamp to scene constants (`RTXRenderer.cpp`)
  3. DXR recursion depth budget (`DXRPipeline.cpp`) with primary + N bounce depth math
- For 4 GI bounces, `MaxTraceRecursionDepth` must be `5` (primary depth 0 + bounce depths 1..4).
- Keep combined fallback shader constant aligned (`Shaders/RTXRaytracing.hlsl` `MAX_TRACE_RECURSION_DEPTH = 5`) so fallback behavior matches C++ pipeline config.
- In this unborn-branch repository state, `bunx critique --web` may show "No changes to display"; piping explicit diff via `--stdin` works for shareable URLs.
