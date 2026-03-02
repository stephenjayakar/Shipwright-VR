# RTX Module — Ship of Harkinian

## Overview

This module adds real-time raytracing (DXR 1.0) support to Ship of Harkinian. It runs alongside the existing OpenGL/Vulkan rasterization pipeline and is gated behind the `ENABLE_DX12_RTX` preprocessor define. When the define is not set, all RTX code compiles to no-ops via inline stubs in `RTXHooks.h`.

**Target platform:** Windows (DX12 + DXR 1.0 hardware required)

## Architecture

```
RTXHooks (C API)
    │
    ▼
RTXRenderer (singleton)
    ├── DX12Device          — DX12 device, swap chain, command infrastructure
    ├── DXRPipeline         — Root signatures, state object, shader tables, output buffers
    ├── AccelerationStructure — BLAS per room, TLAS rebuilt each frame
    ├── SceneGeometryExtractor — Walks N64 display lists → RTXVertex/Material meshes
    ├── GISystem            — Temporal accumulation management, denoise parameters
    └── TextureManager      — N64 texture format conversion, GPU upload, SRV cache
```

## File Reference

### Core

| File | Description |
|------|-------------|
| `RTXTypes.h` | Shared data structures: `RTXVertex`, `Material`, `SceneConstants`, `DenoiseConstants`, etc. Must stay in sync with `Common.hlsli`. |
| `RTXHooks.h/cpp` | C-callable entry points for game integration. Provides no-op stubs when `ENABLE_DX12_RTX` is not defined. |
| `RTXRenderer.h/cpp` | Main singleton orchestrator. Owns all subsystems, handles lifecycle and per-frame dispatch. |
| `RTXSceneConfig.h/cpp` | Per-scene RTX configuration: sky color, sun direction/intensity/color, ambient color, fog settings, GI parameters (intensity, bounce count, probe density), material overrides (reflectivity, roughness, emissive scale), water/AO settings. Includes scene-ID-based config lookup for key scenes (Kokiri Forest, Temple of Time, Hyrule Field, etc.), file-based save/load, time-of-day modulation, and convenience accessor methods. |

### DX12 Infrastructure

| File | Description |
|------|-------------|
| `DX12Device.h/cpp` | DX12 device creation, swap chain, command queue/list, descriptor heaps, fence synchronization. Uses `ID3D12Device5` for DXR support. |
| `DXRPipeline.h/cpp` | DXR state object, global/local root signatures, shader table management, output buffer creation (UAV textures), denoise compute pipeline, scene constant buffer. |
| `RTXShaderCompiler.h/cpp` | DXC (DirectX Shader Compiler) wrapper. Compiles HLSL to DXIL at runtime, loads precompiled `.dxil` blobs from disk, caches compiled shaders. Used by DXRPipeline. |

### Geometry & Acceleration

| File | Description |
|------|-------------|
| `SceneGeometryExtractor.h/cpp` | Walks OoT F3DEX2 display lists (including OTR multi-word variants). Extracts vertices, indices, and materials. Handles G_VTX, G_TRI1/2, G_SETCOMBINE, G_SETTIMG, geometry mode, and render mode tracking. |
| `AccelerationStructure.h/cpp` | Builds BLAS per room (opaque + alpha-tested geometry), rebuilds TLAS each frame. Manages per-geometry GPU buffers for shader table binding. |

### Rendering

| File | Description |
|------|-------------|
| `GISystem.h/cpp` | CPU-side GI management: light probe grid placement within scene AABB, SH-based color accumulation from directional/point lights, nearest-probe lookup with distance-weighted interpolation, temporal accumulation frame counting with camera-motion detection, A-trous denoise parameter generation. Integrates with RTXSceneConfig for GI intensity, probe density, and bounce count. |
| `TextureManager.h/cpp` | N64→RGBA8 texture format conversion (RGBA16, RGBA32, CI4, CI8, IA4, IA8, IA16, I4, I8), GPU upload with staging buffer, SRV descriptor cache with bindless texture array, FNV-1a hashing. |

### Shaders (`Shaders/`)

| File | Shader Type | Description |
|------|-------------|-------------|
| `Common.hlsli` | Include | Shared structures (`SceneConstants`, `RayPayload`, `RTXVertex`, `Material`), combiner mode defines, RNG (PCG hash), cosine-weighted hemisphere sampling. |
| `RayGen.hlsl` | `[shader("raygeneration")]` | Camera ray generation via inverse view/proj matrices, primary ray dispatch, distance fog, temporal accumulation blend. |
| `ClosestHit.hlsl` | `[shader("closesthit")]` | Material evaluation (texture sampling + N64 combiner), direct lighting with shadow ray, 1-bounce cosine-weighted GI. |
| `Miss.hlsl` | `[shader("miss")]` | Sky color gradient blending with fog color. |
| `AnyHit.hlsl` | `[shader("anyhit")]` | Alpha test for foliage (texture alpha sampling with Deku Tree death fade). |
| `Denoise.hlsl` | Compute `[numthreads(8,8,1)]` | Edge-aware A-trous wavelet spatial denoiser. 3 passes with step sizes 1, 2, 4. |
| `Accumulate.hlsl` | Compute `[numthreads(8,8,1)]` | Temporal accumulation (exponential moving average, resets on camera movement). |
| `RTXGlobalIllumination.hlsl` | Compute `[numthreads(8,8,1)]` | Screen-space GI accumulation and temporal filtering post-process. Spatial 3×3 bilateral pre-filter + temporal blend + AO darkening. |
| `RTXRaytracing.hlsl` | DXR Library (`lib_6_3`) | Combined raytracing library containing all RT shader stages (RayGen, ClosestHit, AnyHit, Miss). Alternative to compiling individual shader files; useful for single-library compilation. |

## Preprocessor Guards

All RTX C/C++ code is wrapped in `#ifdef ENABLE_DX12_RTX` / `#endif`:

- **Headers (`.h`):** Use both `#ifndef` include guards and `#ifdef ENABLE_DX12_RTX` inside.
- **Source files (`.cpp`):** Entire file content wrapped in `#ifdef ENABLE_DX12_RTX`.
- **`RTXHooks.h`:** Special case — provides `extern "C"` function declarations when RTX is enabled, and `static inline` no-op stubs in the `#else` branch, allowing any C or C++ file to include it safely regardless of the RTX build flag.
- **`RTXTypes.h`:** Safe to include from any RTX file; all types are inside the `#ifdef` guard.

## Initialization & Shutdown

- **Lazy initialization:** The DX12 device and DXR pipeline are initialized lazily on the first load of an RTX-enabled scene (`RTX_OnSceneLoaded`). This retrieves the game window's HWND via `SDL_GetWindowWMInfo` and creates the full rendering stack (DX12Device → DXRPipeline → AccelerationStructure → GISystem → TextureManager).
- **Shutdown:** `RTX_Shutdown()` is called from `DeinitOTR()` in `OTRGlobals.cpp`, ensuring all DX12 resources are released before the window is destroyed.

### Modified Game Files

The following files outside `soh/soh/Enhancements/RTX/` have RTX integration hooks, all guarded by `#ifdef ENABLE_DX12_RTX`:

| File | Hook | Purpose |
|------|------|---------|
| `soh/src/code/z_scene_table.c` | `func_8009E0B8()` | Early-out for Kokiri Forest scene draw config — extracts scene params for RTX |
| `soh/src/code/z_play.c` | `Play_Draw()` / `Play_Destroy()` | Suppresses `Room_Draw()` when RTX active; calls `RTX_OnSceneUnload()` on destroy |
| `soh/soh/OTRGlobals.cpp` | `Graph_ProcessGfxCommands()` / `DeinitOTR()` | Routes frame present through RTX dispatch; calls `RTX_Shutdown()` on exit |
| `soh/soh/z_scene_otr.cpp` | Room load completion | Calls `RTX_OnRoomLoaded()` for RTX-enabled scenes |
| `soh/soh/z_play_otr.cpp` | `OTRPlay_SpawnScene()` | Calls `RTX_OnSceneLoaded()` on scene init |

## Data Flow (Per Frame)

1. **Scene hooks** call `RTX_OnSceneLoaded` / `RTX_OnRoomLoaded` → triggers geometry extraction → BLAS build.
2. **`RTX_UpdateSceneParams`** extracts camera, lighting, fog from `PlayState` → fills `SceneConstants` → uploads to GPU constant buffer. View/projection matrices are converted from fixed-point `Mtx` to float via `Matrix_MtxToMtxF`.
3. **`RTX_DispatchAndPresent`**:
   - `BeginFrame()` (reset command allocator/list)
   - Rebuild TLAS (all loaded rooms)
   - `DispatchRays` (primary + shadow + GI bounce)
   - UAV barrier
   - 3× `DispatchDenoise` (A-trous wavelet passes with ping-pong buffers)
   - Copy output → back buffer
   - `Present()`

## Root Signature Layout

### Global Root Signature

| Slot | Type | Register | Description |
|------|------|----------|-------------|
| 0 | CBV | b0 | `SceneConstants` |
| 1 | SRV | t0 | TLAS |
| 2 | UAV | u0 | Output buffer |
| 3 | UAV | u1 | GI accumulation buffer |
| 4 | Descriptor Table | t4+ | Bindless texture array (4096 SRVs, matches `MAX_BINDLESS_TEXTURES`) |
| — | Static Sampler | s0 | Bilinear wrap |

### Local Root Signature (per hit group, space1)

| Slot | Type | Register | Description |
|------|------|----------|-------------|
| 0 | SRV | t0, space1 | Vertex buffer |
| 1 | SRV | t1, space1 | Index buffer |
| 2 | SRV | t2, space1 | Material ID buffer (per-triangle) |
| 3 | SRV | t3, space1 | Material table |

## N64 Combiner Mapping

The module classifies OoT's complex N64 color combiner into 5 simplified modes:

| ID | Mode | HLSL Behavior |
|----|------|---------------|
| 0 | `MODULATE_RGB` | `tex.rgb * vtxColor.rgb` |
| 1 | `MODULATE_RGBA` | `tex * vtxColor` (with alpha) |
| 2 | `DECAL` | `tex.rgb` (texture only) |
| 3 | `SHADE` | `vtxColor.rgb` (vertex color only) |
| 4 | `TEX_ENV_BLEND` | `lerp(tex, envColor, tex.a)` |

## Building

The RTX module is only compiled when `ENABLE_DX12_RTX` is defined in the CMake configuration. On non-Windows platforms or when DX12 is not available, the entire module compiles to empty translation units.

HLSL shaders are compiled separately using DXC (DirectX Shader Compiler) and loaded at runtime as DXIL bytecode.
