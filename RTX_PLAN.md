# RTX Kokiri Verification Plan (Execution-Driven)

Date: 2026-03-02
Owner: RTX integration
Scope: Shipwright-3 Kokiri Forest visual correctness and stability

## Objective
Ship `SCENE_KOKIRI_FOREST` (`0x55`) with stable RTX and verifiable visual quality:
1. Boot directly into Kokiri Forest.
2. Material diversity works (no flat single-material look).
3. GI is active but not over-bright/yellow.
4. Water has motion + reflection/refraction behavior.
5. Dirt path is no longer white fallback texture.

## Verification Strategy
Every visual task must have both:
- Runtime evidence in `x64/Release/logs/rtx_diag.log` / `Ship of Harkinian.log`.
- A stability check (no `DEVICE_HUNG`, no `Present FAILED`).

## Tasks

### T1. Boot into Kokiri First
Status: DONE

Implementation:
- `shipofharkinian.json`
  - `CVars.gSettings.BootSequence = 4`
  - `WarpPoints.Kokiri Forest.bootToPoint = true`
  - `WarpPoints.Kokiri Forest.entranceId = 238 (0xEE)`
- `x64/Release/shipofharkinian.json`
  - Added/confirmed same effective values under runtime config.

Verification (pass criteria):
- `Ship of Harkinian.log` contains:
  - `Scene Init - sceneNum: 0x55, entranceIndex: 0xee`

---

### T2. Restore Real Material Diversity (Fix White/Fallback Paths)
Status: DONE

Implementation:
- Fixed unresolved-material lifecycle:
  - `SceneGeometryExtractor.cpp`: unresolved materials now start with fallback index `0` (not checkerboard `1`).
  - `RTXRenderer.cpp`: material resolve logic treats `<=2` as fallback and re-resolves; only `>2` is considered fully resolved.
- Updated diagnostics to report fallback path explicitly (`idx<=2`).

Verification (pass criteria):
- `rtx_diag.log` shows many `CreateMaterial ... [RESOLVED]` with distinct `texIdx` values.
- `ResolveMaterialTextures ... unresolved(white)=0`.
- `DispatchRays ... materials: ... fallback(idx<=2)=0`.

---

### T3. Reduce Yellow Glow / Excess Bounce
Status: DONE

Implementation:
- Scene tuning (`RTXSceneConfig.cpp`):
  - Kokiri `giIntensity = 0.14`.
  - Cooler sky/fog/ambient preserved.
- Shader tuning (`Miss.hlsl`):
  - Cooler sky gradient and reduced sun halo intensity.

Verification (pass criteria):
- `GISystem::ApplySceneConfig() ... giIntensity=0.14` in `rtx_diag.log`.
- No warm overglow regressions after rebuild/run (checked with live runtime + logs).

---

### T4. GI Works Properly Without TDR
Status: DONE

Implementation:
- `ClosestHit.hlsl`:
  - Kept one-bounce GI active on primary rays.
  - Added stochastic GI sampling and conservative energy.
  - Reduced expensive secondary work to prevent GPU timeout:
    - Shadow rays only for primary rays.
    - GI/reflection trace distances reduced.
    - Reflection trace frequency reduced.

Verification (pass criteria):
- `DispatchRays` continuous during runtime.
- `Present #N OK` continues well past startup frames.
- No `DEVICE_HUNG` / `Present FAILED hr=0x887A0005` in current run logs.

---

### T5. Water Motion + Reflection/Refraction
Status: DONE

Implementation:
- `ClosestHit.hlsl`:
  - Animated UV offsets based on frame and world position.
  - Ripple-perturbed normals (`WaterRippleNormal`).
  - Fresnel-based reflection/refraction blend with sky fallback.
- Water material classification already active from extractor/runtime flags.

Verification (pass criteria):
- `rtx_diag.log` contains repeated `WATER SURFACE #... detected` entries.
- Water materials have `water=1` and are included in resolved material set.

---

### T6. Dirt Path No Longer White
Status: DONE

Implementation:
- Same root fix as T2 (correct fallback-to-resolved transitions and re-resolve policy).

Verification (pass criteria):
- No fallback textures remain in active material dispatch (`fallback(idx<=2)=0`).
- Dirt path uses resolved texture SRV like other Kokiri opaque materials.

---

## Additional Fixes Added

### A1. GPU Hang Regression Guard
Status: DONE

Reason:
- During this cycle, new shading workload caused `DXGI_ERROR_DEVICE_HUNG` after a few frames.

Fix:
- Reduced secondary-ray cost in `ClosestHit.hlsl` while preserving GI/water behavior.

Verification:
- 25s runtime test remains alive; `Present` remains OK; no `DEVICE_HUNG` in current log.

## Repro/Validation Commands
From repo root:

```powershell
& 'C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe' build\soh\soh.vcxproj /p:Configuration=Release /p:Platform=x64 /m
powershell -ExecutionPolicy Bypass -File x64\Release\test-launch.ps1
rg -n "sceneNum: 0x55|entranceIndex: 0xee" "x64/Release/logs/Ship of Harkinian.log"
rg -n "giIntensity=0.14|WATER SURFACE|fallback\(idx<=2\)|Present #[0-9]+ OK|DEVICE_HUNG|FAILED hr=0x887A0005" "x64/Release/logs/rtx_diag.log"
```

## Exit Criteria
All criteria met:
- Boots to Kokiri (`0x55`, `0xEE`).
- Materials resolved (no fallback in active dispatch).
- GI tuned and active (`0.14`) without yellow overglow regression.
- Water motion/reflection/refraction path active.
- No current-run `DEVICE_HUNG`/failed present regression.
