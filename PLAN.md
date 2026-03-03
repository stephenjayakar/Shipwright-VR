# RTX Kokiri Forest - Problem Identification Plan

## Status
- Execution state: `IN_PROGRESS`
- Scope of this file: Identify current problems and define verification-first work order.
- Constraint: Verification-first execution. Implement only after each phase has concrete evidence.

## Current Cycle (2026-03-02)

Completed:
- Workspace migration completed into this repository root (project content now available under `.`).
- Build system regenerated for the new path (`build/x64`) after stale path references were detected.
- Release build completed from regenerated CMake/VS solution.
- `soh.exe` launch validated from `x64/Release` (startup confirmed).

Next:
- Complete Phase 1 with a real 4-view harness (road close-up, grass close-up, water close-up, wide mixed shot) and per-view material attribution from diagnostics.
- Current blocker: road overlay is no longer blown-out white, but remains too flat/pale; deterministic water-focused framing is still missing.
- New user targets (2026-03-02, active now):
  - Fix Kokiri dirt path appearance to match reference (remove pale/chalk overlay look).
  - Make water clearly transparent + reflective + refractive.
  - Upgrade GI from effectively single indirect bounce behavior to true multi-bounce (2+ bounces).

### Phase 1 Evidence (2026-03-02)

Evidence captured and view-checked:
- `validation_screenshots/live_capture_20260302_073812.png`
- `validation_screenshots/live_capture_20260302_073917.png`
- `validation_screenshots/live_capture_20260302_074210.png`
- `validation_screenshots/live_capture_20260302_074316.png`
- `validation_screenshots/live_capture_20260302_074428.png`
- `validation_screenshots/rtx_frame_0060.png`
- `validation_screenshots/rtx_frame_0300.png`
- `validation_screenshots/rtx_frame_1200.png`
- `validation_screenshots/rtx_frame_3000.png`

Observed from current captures:
- Dirt path remains incorrect in live captures (still white/chalky mask-like strip).
- Scene remains over-flattened/olive with weak local material separation in validation frames.
- Grass detail is still not convincingly restored (broad low-frequency terrain look dominates).
- Water verification is still incomplete because current framing does not isolate stream/pond behavior.

### Iteration Log (2026-03-02, continued)

Code changes applied:
- Removed broad Kokiri path recolor pass from `soh/soh/Enhancements/RTX/TextureManager.cpp` so base texture detail is no longer overwritten globally.
- Narrowed `COMBINER_TEX_ENV_BLEND` handling in `soh/soh/Enhancements/RTX/Shaders/ClosestHit.hlsl` for alpha-tested materials to avoid blend-to-vertex whiteout.
- Added targeted retint only for known Kokiri path overlay masks (`spot04_room_0Tex_01A290`, `spot04_room_0Tex_019A90`, `spot04_room_0Tex_019290`, `spot04_sceneTex_00F218`) in `TextureManager.cpp`.
- Relaxed over-aggressive path/decal luminance clamps in `ClosestHit.hlsl`.

Verification evidence from latest run:
- `C:\Users\aj12a\programming\Shipwright-3\screenshots\rtx_20260302_134826_frame300.png`
- `C:\Users\aj12a\programming\Shipwright-3\screenshots\rtx_20260302_134446_frame300.png`

Result:
- Major regression fixed: road overlay is no longer pure white.
- Remaining issue: road/path remains too matte/flat and still reads as a pale overlay rather than detailed dirt.
- Phase 1 still incomplete (no deterministic 4-view harness yet, no dedicated water close-up attribution set).

Decision:
- Phase 1 is partially complete (capture evidence exists) but not complete for attribution quality. Next action is to add deterministic multi-view capture points, then map each problematic region to exact material/texture IDs before Phase 2 shader edits.

### Iteration Log (2026-03-02, user request in progress)

Requested by user:
- Path should match reference image (reduce chalk/pale overlay look).
- Water should be transparent + reflective + refractive.
- GI should support 2+ indirect bounces (not only single-bounce feel).

Implementation started:
- `ClosestHit.hlsl`: increased path overlay dirt blend + tightened decal luma ceiling.
- `ClosestHit.hlsl`: upgraded GI logic from primary-only one-bounce to recursion-budgeted multi-bounce.
- `RTXTypes.h` + `Common.hlsli` + `RTXRenderer.cpp`: added `giMaxBounces` in SceneConstants and wired it from `RTXSceneConfig.maxBounces` (clamped 1..3 for runtime safety).
- `DXRPipeline.cpp`: raised `MaxTraceRecursionDepth` to 4 to support multi-bounce GI chains.
- `ClosestHit.hlsl`: increased water transmission/refraction weight while keeping Fresnel reflections.

Pending verification in this cycle:
- Rebuild and launch Kokiri runtime; capture updated path/water/GI evidence screenshots.

## Problem Inventory

1. Grass/ground detail is effectively gone
- Symptom: Terrain appears as flat beige slabs; expected grassy breakup is missing.
- Evidence: Latest screenshot set shows low-frequency monotone ground with little/no grass texture identity.
- Likely causes:
  - Over-aggressive albedo/finalColor dirt remapping and brightness clamps in `ClosestHit.hlsl`.
  - Path-mask recolor heuristics in `TextureManager.cpp` affecting broader terrain texture set.
  - Alpha-tested mask handling collapsing texture contrast.

2. Dirt path is still visually wrong
- Symptom: Path remains pale/chalky and not matching intended dirt appearance.
- Evidence: White/pale strip persists in frame-300 captures.
- Likely causes:
  - Simplified combiner logic (especially decal/modulate-alpha paths) diverges from N64 primitive/env driven output.
  - Mask overlays are blending to the wrong base (vertex color/luma approximation instead of real combiner state).

3. Scene looks smeared/over-flattened
- Symptom: Surface detail feels blurred and homogenized.
- Evidence: Terrain/walls lose micro-contrast in captures.
- Likely causes:
  - Temporal accumulation + denoise still smoothing too aggressively for current content.
  - Additional shader-side remaps reduce local contrast before tone mapping.

4. Water behavior is not verified as correct
- Symptom: User reports water not visibly moving with convincing reflection/refraction.
- Evidence gap: Current harness camera framing rarely shows water surfaces clearly.
- Likely causes:
  - Water shading may be partially functioning but unverified in representative capture angles.
  - No dedicated automated camera/viewpoint for stream/pond validation.

5. Material response consistency regression risk
- Symptom: In trying to fix path brightness, broad terrain logic may have overridden physically/plausibly distinct material looks.
- Evidence: Grass and road both drifted toward same brown-beige family.
- Likely causes:
  - Global heuristics applied to non-target materials (over-broad conditions on luma/saturation/alpha-tested paths).

## Verification Gaps

1. No material-isolation validation
- Missing: Per-texture/material visual probes that confirm only targeted textures are affected.

2. No multi-view harness for Kokiri
- Missing: Fixed capture points for:
  - road close-up
  - grass patch close-up
  - water stream close-up
  - mixed scene wide shot

3. No explicit pass/fail visual rubric
- Missing: Quantitative/qualitative acceptance checks per issue (path tone, grass detail recovery, water motion/reflection visibility, no flashing).

## Planned Work Order (Do Not Execute Yet)

Phase 1: Baseline capture and attribution
- Capture canonical 4-view screenshot set.
- Map visible problematic regions to exact material IDs and texture paths from diagnostics.

Phase 2: Narrow-scope shader/material correction
- Remove broad terrain clamps/remaps and replace with strict texture/material-targeted logic.
- Restore grass to diffuse, textured response.
- Rebuild path appearance using combiner-faithful handling (not luma-only remap).

Phase 3: Water validation path
- Add/enable deterministic water-focused capture angle.
- Validate motion, reflection, and refraction visibility in captures.

Phase 4: Temporal/denoise retune with guardrails
- Reduce smear while preserving stability.
- Confirm no flashing via frame-diff checks across static-camera intervals.

Phase 5: Final regression sweep
- Re-verify:
  - Kokiri-first load
  - path tone correctness
  - grass texture/detail restored
  - non-water matte response
  - water behavior visible and stable
  - no lighting flash

## Acceptance Criteria
- Grass is clearly visible and no longer flattened into beige terrain.
- Path reads as dirt (not white/pale mask strip) in close and wide shots.
- Water shows visible movement plus plausible reflection/refraction in dedicated water captures.
- Static camera sequence shows no flashing and no temporal shimmer.
- Bilinear filtering remains active without excessive blur.

## Hold Point
- Continue with Phase 1 execution and log evidence in this file.
