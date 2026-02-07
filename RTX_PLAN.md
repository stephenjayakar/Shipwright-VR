# RTX Raytracing Implementation Plan for Kokiri Forest Scene
## Ship of Harkinian (Shipwright) - DirectX 12 + DXR

---

## Implementation Progress

| Phase | Status | Notes |
|-------|--------|-------|
| Phase 1: DX12 + DXR Foundation | **DONE** | DX12Device.h/cpp, DXRPipeline.h/cpp fully implemented |
| Phase 2: Geometry Extraction | **DONE** | SceneGeometryExtractor.h/cpp fully implemented |
| Phase 3: Acceleration Structures | **DONE** | AccelerationStructure.h/cpp fully implemented |
| Phase 4: HLSL Shaders | **DONE** | All 6 shaders: Common.hlsli, RayGen, ClosestHit, Miss, AnyHit, Denoise |
| Phase 5: Global Illumination | **DONE** | 1-bounce diffuse path tracing in ClosestHit.hlsl + temporal accumulation in RayGen.hlsl |
| Phase 6: Texture Management | **DONE** | TextureManager.h/cpp fully implemented |
| Phase 7: Scene Integration Hooks | **DONE** | RTXHooks.h/cpp + RTXRenderer.h/cpp orchestrator |
| Phase 8: Kokiri Forest Specifics | **DONE** (partial) | Fog/alpha/water scroll logic in RTXRenderer + shaders; full display list specifics need Phase 2 |
| Phase 9: Build System Integration | **DONE** | CMake: ENABLE_DX12_RTX option, DXC shader compilation, source listing, link libs, preprocessor guards |

---

## Executive Summary

This document outlines a comprehensive plan to add **RTX raytracing with global illumination** to the Kokiri Forest scene (`SCENE_KOKIRI_FOREST`, internal name `spot04`) in Ship of Harkinian. The implementation will:

- Use **DirectX 12** with **DXR (DirectX Raytracing)** as the exclusive renderer for this scene
- Implement **1-bounce diffuse path tracing** for global illumination
- Support **Windows x64** only (no fallback to rasterization)
- Bypass the existing Fast3D rendering pipeline entirely for Kokiri Forest
- Extract geometry from N64 display lists and build DXR acceleration structures
- Handle N64-specific features (scrolling water textures, fog, alpha-tested foliage)

---

## Table of Contents

1. [Current Architecture Analysis](#1-current-architecture-analysis)
2. [High-Level Architecture](#2-high-level-architecture)
3. [Phase 1: DirectX 12 + DXR Foundation](#phase-1-directx-12--dxr-foundation)
4. [Phase 2: Geometry Extraction](#phase-2-geometry-extraction)
5. [Phase 3: Acceleration Structures](#phase-3-acceleration-structures)
6. [Phase 4: HLSL Shaders](#phase-4-hlsl-shaders)
7. [Phase 5: Global Illumination](#phase-5-global-illumination)
8. [Phase 6: Texture Management](#phase-6-texture-management)
9. [Phase 7: Scene Integration Hooks](#phase-7-scene-integration-hooks)
10. [Phase 8: Kokiri Forest Specifics](#phase-8-kokiri-forest-specifics)
11. [Phase 9: Build System Integration](#phase-9-build-system-integration)
12. [Known Limitations and Jank](#known-limitations-and-jank)
13. [File Structure](#file-structure)

---

## 1. Current Architecture Analysis

### 1.1 Rendering Pipeline Overview

**Current Flow:**
```
Game Logic (60/20 fps)
  -> Graph_Update()                         [soh/src/code/graph.c]
    -> Play_Draw()                          [soh/src/code/z_play.c:1354+]
      -> Scene_Draw()                       [soh/src/code/z_scene_table.c:1677]
        -> sSceneDrawHandlers[play->sceneConfig]()
          -> func_8009E0B8() for Kokiri Forest (SDC_KOKIRI_FOREST = 4)
      -> Room_Draw()                        [soh/src/code/z_room.c]
        -> sRoomDrawHandlers[meshHeader->type]()
          -> func_80095AB4() for Type 0 (simple polygon list)
          -> func_80095D04() for Type 2 (sorted/culled polygon list)
            -> gSPDisplayList(POLY_OPA_DISP++, polygonDlist->opa)
            -> gSPDisplayList(POLY_XLU_DISP++, polygonDlist->xlu)
    -> Graph_ProcessGfxCommands()           [soh/soh/OTRGlobals.cpp:1726]
      -> FrameInterpolation_Interpolate()   (for 60+ fps)
      -> RunCommands()                      [soh/soh/OTRGlobals.cpp:1706]
        -> wnd->DrawAndRunGraphicsCommands(Commands, mtx_replacements)
          -> Fast3D renderer (in libultraship submodule)
            -> Translates F3DEX2 GBI commands to DX11/OpenGL draw calls
```

**Key Hook Points for RTX:**
- `func_8009E0B8()` at `z_scene_table.c:1110` - Kokiri Forest scene draw config
- `Room_Draw()` at `z_room.c` - Room geometry submission
- `Graph_ProcessGfxCommands()` at `OTRGlobals.cpp:1726` - Final display list processing and present

### 1.2 Scene Draw Dispatch

The scene draw config system is a simple function pointer table (`z_scene_table.c:1666-1675`):

```c
void (*sSceneDrawHandlers[])(PlayState*) = {
    func_80099550, func_8009DA30, func_8009DD5C, func_8009DE78,
    func_8009E0B8, // <-- Index 4: SDC_KOKIRI_FOREST
    func_8009E54C, func_8009E730, /* ... 52 total entries */
};

void Scene_Draw(PlayState* play) {
    sSceneDrawHandlers[play->sceneConfig](play);
}
```

This is called from `Play_Draw()` at `z_play.c:1549`:
```c
Scene_Draw(play);
Room_Draw(play, &play->roomCtx.curRoom, roomDrawFlags & 3);
Room_Draw(play, &play->roomCtx.prevRoom, roomDrawFlags & 3);
```

### 1.3 Kokiri Forest Scene Details

**Scene Identification:**
- **Scene enum:** `SCENE_KOKIRI_FOREST` (0x55) from `z64scene.h`
- **Scene draw config:** `SDC_KOKIRI_FOREST` (4) from `z64scene.h:352`
- **Internal name:** `spot04`
- **Scene table entry:** `scene_table.h:97`

**Scene Structure:**
- **3 rooms:** `spot04_room_0`, `spot04_room_1`, `spot04_room_2`
- **Scene header:** `soh/assets/scenes/overworld/spot04/spot04_scene.h`
  - 6 scene-level textures (rgba16, i4, ci8)
  - 1 palette (TLUT)
  - 1 collision header at offset 0x8918
  - 2 cutscene data blocks
- **Room 0:** `soh/assets/scenes/overworld/spot04/spot04_room_0.h`
  - ~37 textures
  - ~21 display lists (opaque + translucent pairs)
  - Multiple scene setup variants (Set_0006B0, Set_000940, etc.)
- **Room 1:** `soh/assets/scenes/overworld/spot04/spot04_room_1.h`
- **Room 2:** `soh/assets/scenes/overworld/spot04/spot04_room_2.h`

All assets are referenced by OTR paths like:
```c
#define dspot04_room_0DL_008ED8 "__OTR__scenes/shared/spot04_scene/spot04_room_0DL_008ED8"
```

### 1.4 Kokiri Forest Scene Draw Config

The full function from `z_scene_table.c:1110-1161`:

```c
// Scene Draw Config 4 - Kokiri Forest
void func_8009E0B8(PlayState* play) {
    u32 gameplayFrames;
    u8 spA3;      // Vegetation alpha (for Deku Tree death)
    u16 spA0;     // Fog distance
    Gfx* displayListHead;

    spA3 = 128;
    spA0 = 500;   // Default fog distance
    displayListHead = Graph_Alloc(play->state.gfxCtx, 6 * sizeof(Gfx));

    OPEN_DISPS(play->state.gfxCtx);

    gameplayFrames = play->gameplayFrames;

    // Segment 0x09: Scrolling water texture (slow)
    gSPSegment(POLY_XLU_DISP++, 0x09,
               Gfx_TwoTexScroll(play->state.gfxCtx, 0,
                   127 - gameplayFrames % 128, (gameplayFrames * 1) % 128, 32, 32,
                   1, gameplayFrames % 128, (gameplayFrames * 1) % 128, 32, 32));

    // Segment 0x08: Scrolling water texture (fast)
    gSPSegment(POLY_XLU_DISP++, 0x08,
               Gfx_TwoTexScroll(play->state.gfxCtx, 0,
                   127 - gameplayFrames % 128, (gameplayFrames * 10) % 128, 32, 32,
                   1, gameplayFrames % 128, (gameplayFrames * 10) % 128, 32, 32));

    gDPPipeSync(POLY_OPA_DISP++);
    gDPSetEnvColor(POLY_OPA_DISP++, 128, 128, 128, 128);
    gDPPipeSync(POLY_XLU_DISP++);
    gDPSetEnvColor(POLY_XLU_DISP++, 128, 128, 128, 128);

    // Deku Tree death: fade alpha during cutscene
    if (gSaveContext.sceneSetupIndex == 4) {
        spA3 = 255 - (u8)play->roomCtx.unk_74[0];
    }
    // Deku Tree death: increase fog during cutscene
    else if (gSaveContext.sceneSetupIndex == 6) {
        spA0 = play->roomCtx.unk_74[0] + 500;
    }
    // After Deku Tree is dead: permanent fog distance change
    else if (((gSaveContext.sceneSetupIndex < 4) || LINK_IS_ADULT) &&
             (Flags_GetEventChkInf(EVENTCHKINF_OBTAINED_KOKIRI_EMERALD_DEKU_TREE_DEAD))) {
        spA0 = 2150;
    }

    // Segment 0x0A: Vegetation alpha (env color with spA3)
    gSPSegment(POLY_OPA_DISP++, 0x0A, displayListHead);
    gDPPipeSync(displayListHead++);
    gDPSetEnvColor(displayListHead++, 128, 128, 128, spA3);
    gSPEndDisplayList(displayListHead++);

    // Segment 0x0B: Fog/atmosphere blending
    gSPSegment(POLY_XLU_DISP++, 0x0B, displayListHead);
    gSPSegment(POLY_OPA_DISP++, 0x0B, displayListHead);
    gDPPipeSync(displayListHead++);
    gDPSetEnvColor(displayListHead++, 128, 128, 128, spA0 * 0.1f);
    gSPEndDisplayList(displayListHead);

    // Segment 0x0C: Stream/waterfall scrolling
    gSPSegment(POLY_OPA_DISP++, 0x0C,
               Gfx_TwoTexScroll(play->state.gfxCtx, 0,
                   0, (s16)(-play->roomCtx.unk_74[0] * 0.02f), 32, 16,
                   1, 0, (s16)(-play->roomCtx.unk_74[0] * 0.02f), 32, 16));

    CLOSE_DISPS(play->state.gfxCtx);
}
```

### 1.5 Room Drawing

**Type 0 - Simple Polygon List (`z_room.c:50-86`):**
```c
void func_80095AB4(PlayState* play, Room* room, u32 flags) {
    polygon0 = &room->meshHeader->polygon0;
    polygonDlist = SEGMENTED_TO_VIRTUAL(polygon0->start);

    for (i = 0; i < polygon0->num; i++) {
        if ((flags & 1) && (polygonDlist->opa != NULL))
            gSPDisplayList(POLY_OPA_DISP++, polygonDlist->opa);
        if ((flags & 2) && (polygonDlist->xlu != NULL))
            gSPDisplayList(POLY_XLU_DISP++, polygonDlist->xlu);
        polygonDlist++;
    }
}
```

**Type 2 - Sorted/Culled Polygon List (`z_room.c:98-230`):**
- Sorts polygon chunks by distance from camera
- Culls chunks beyond `play->lightCtx.fogFar`
- Draws front-to-back
- Kokiri Forest uses this for its large outdoor geometry

**Mesh Header Structures (from `z64scene.h`):**
```c
typedef struct {
    MeshHeaderBase base;     // .headerType = 0 or 2
    u8 numEntries;
    Gfx* dListStart;
    Gfx* dListEnd;
} MeshHeader0;               // Also MeshHeader2

typedef struct {
    Gfx* opaqueDList;
    Gfx* translucentDList;
} MeshEntry0;                 // PolygonDlist for Type 0

typedef struct {
    s16 playerXMax, playerZMax;
    s16 playerXMin, playerZMin;
    Gfx* opaqueDList;
    Gfx* translucentDList;
} MeshEntry2;                 // PolygonDlist2 for Type 2 (includes bounds)
```

### 1.6 Lighting System

**Light Types (`z_lights.c`):**
- `LIGHT_POINT_NOGLOW` - Point light, no glow sprite
- `LIGHT_POINT_GLOW` - Point light with lens flare
- `LIGHT_DIRECTIONAL` - Directional light (sun/moon)

**Lights_Draw (`z_lights.c:65-92`):**
```c
void Lights_Draw(Lights* lights, GraphicsContext* gfxCtx) {
    gSPNumLights(POLY_OPA_DISP++, lights->numLights);  // Max 7
    gSPNumLights(POLY_XLU_DISP++, lights->numLights);

    for (i = 0; i < lights->numLights; i++) {
        gSPLight(POLY_OPA_DISP++, &lights->l.l[i], i + 1);
        gSPLight(POLY_XLU_DISP++, &lights->l.l[i], i + 1);
    }

    // Ambient light is numLights + 1
    gSPLight(POLY_OPA_DISP++, &lights->l.a, lights->numLights + 1);
    gSPLight(POLY_XLU_DISP++, &lights->l.a, lights->numLights + 1);
}
```

**Point Light Binding (`z_lights.c:102-137`):**
```c
void Lights_BindPoint(Lights* lights, LightParams* params, Vec3f* vec) {
    xDiff = params->point.x - vec->x;
    yDiff = params->point.y - vec->y;
    zDiff = params->point.z - vec->z;
    scale = params->point.radius;
    posDiff = SQ(xDiff) + SQ(yDiff) + SQ(zDiff);

    if (posDiff < SQ(scale)) {
        // Attenuation: 1 - (dist/radius)^2
        posDiff = sqrtf(posDiff);
        scale = posDiff / scale;
        scale = 1 - SQ(scale);

        light->l.col[0] = params->point.color[0] * scale;
        // ...

        // Convert point to pseudo-directional (N64 RSP trick)
        scale = (posDiff < 1.0f) ? 120.0f : 120.0f / posDiff;
        light->l.dir[0] = xDiff * scale;
        // ...
    }
}
```

**Scene Light Settings (`z64scene.h:263-272`):**
```c
typedef struct {
    u8 ambientColor[3];
    s8 diffuseDir1[3];
    u8 diffuseColor1[3];
    s8 diffuseDir2[3];
    u8 diffuseColor2[3];
    u8 fogColor[3];
    u16 fogNear;
    u16 fogFar;
} LightSettings;  // Interpolated by time of day
```

### 1.7 Build System - Windows x64

**Compile Definitions (`soh/CMakeLists.txt:332-352`):**
```cmake
if("${CMAKE_VS_PLATFORM_NAME}" STREQUAL "x64")
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        "$<$<CONFIG:Debug>:"
            "ENABLE_DX11;"
        ">"
        "F3DEX_GBI_2"
        "UNICODE;"
        "_UNICODE"
        NOMINMAX
    )
endif()
```

**Link Dependencies (`soh/CMakeLists.txt:638-657`):**
```cmake
set(ADDITIONAL_LIBRARY_DEPENDENCIES
    "libultraship;"
    "ZAPDLib;"
    "glu32;"
    "SDL2::SDL2;"
    "SDL2::SDL2main;"
    "glfw;"
    "winmm;"
    "imm32;"
    "version;"
    "setupapi"
    "Ogg::ogg"
    "Opus::opus"
    "Vorbis::vorbis"
    "Vorbis::vorbisenc"
    "Vorbis::vorbisfile"
    "OpusFile::opusfile"
)
```

**Submodules (currently empty, must be initialized):**
- `libultraship/` - Graphics, window, input, audio, resource management
- `ZAPDTR/` - Asset format support (ZAPD)
- `OTRExporter/` - ROM asset extraction

### 1.8 Frame Interpolation

The existing `frame_interpolation.h/cpp` records matrix operations during game logic and replays them with interpolated values for higher FPS. This **will not work** with the RTX path since we bypass display lists entirely. Camera interpolation must be handled in the RTX renderer directly.

---

## 2. High-Level Architecture

### 2.1 Pipeline Replacement Strategy

**New RTX Pipeline (Kokiri Forest only):**
```
Scene Load:
  OTR resource load -> Extract vertex/index buffers from display lists
                    -> Decode N64 textures to DX12 SRVs
                    -> Build BLAS per room (static)
                    -> Build material table

Per Frame:
  func_8009E0B8() [scene draw config]
    -> Check RTX active AND scene == KOKIRI_FOREST
    -> Skip GBI commands
    -> Call RTXRenderer::DrawScene(play)
      -> Extract camera from play->view
      -> Extract lights from play->lightCtx / play->envCtx
      -> Update per-frame constants (time, fog, alpha)
      -> Update actor TLAS instances (dynamic geometry)
      -> DispatchRays() [ray generation shader]
        -> Primary ray hits -> ClosestHit shader
          -> Sample texture, apply combiner, compute direct light
          -> Launch 1 GI bounce ray (cosine hemisphere)
        -> Miss -> sky/fog color
      -> Temporal accumulation (running average over N frames)
      -> Spatial denoise (A-trous wavelet, edge-aware)
      -> Copy to swap chain back buffer

  Graph_ProcessGfxCommands()
    -> Check RTX active
    -> Skip RunCommands() / Fast3D path
    -> Call RTXRenderer::Present()
```

### 2.2 Key Components

| Component | File | Purpose |
|-----------|------|---------|
| DX12Device | `DX12Device.h/cpp` | Device, command queue, swap chain, descriptor heaps |
| DXRPipeline | `DXRPipeline.h/cpp` | State object, root signature, shader tables |
| SceneGeometryExtractor | `SceneGeometryExtractor.h/cpp` | Walk N64 display lists, extract geometry |
| AccelerationStructure | `AccelerationStructure.h/cpp` | Build/update BLAS and TLAS |
| TextureManager | `TextureManager.h/cpp` | Decode N64 textures -> DX12 SRV heap |
| RTXRenderer | `RTXRenderer.h/cpp` | Orchestrator: extract, trace, denoise, present |
| Shaders | `shaders/*.hlsl` | RayGen, ClosestHit, Miss, AnyHit, Denoise |

---

## Phase 1: DirectX 12 + DXR Foundation -- DONE

### 1.1 DX12Device

**Responsibilities:**
- Create `ID3D12Device5` (minimum for DXR Tier 1.0)
- Enumerate adapters, prefer discrete GPU with DXR support
- Create command queue (DIRECT type), command allocators, command list (`ID3D12GraphicsCommandList4`)
- Create swap chain (`IDXGISwapChain3`) bound to the game's HWND
- Create descriptor heaps:
  - RTV heap (2 descriptors, double-buffered)
  - SRV heap (1024 descriptors, shader-visible) for textures
  - UAV heap (16 descriptors, shader-visible) for output buffers + GI accumulation
- Create fence + event for CPU/GPU synchronization
- `BeginFrame()` / `EndFrame()` / `Present()` lifecycle

**HWND Acquisition:**
- The game window is created by libultraship's `Fast3dWindow`
- We need to get the HWND from the existing SDL2 window: `SDL_GetWindowWMInfo()`
- Or create a separate DX12 swap chain that targets the same window

**Key Requirement:** `D3D12_FEATURE_D3D12_OPTIONS5::RaytracingTier >= D3D12_RAYTRACING_TIER_1_0`

### 1.2 DXRPipeline

**Root Signature:**
```
Global Root Signature:
  [0] CBV (b0) - SceneConstants (camera, lights, fog, time)
  [1] SRV (t0) - Acceleration structure (TLAS)
  [2] UAV (u0) - Output buffer (RWTexture2D<float4>)
  [3] UAV (u1) - GI accumulation buffer (RWTexture2D<float4>)
  [4] Descriptor Table - SRV range (textures, t4+)
  [5] Sampler (s0) - Bilinear wrap sampler

Local Root Signature (per hit group):
  [0] SRV (t0, space1) - Vertex buffer (StructuredBuffer<Vertex>)
  [1] SRV (t1, space1) - Index buffer (StructuredBuffer<uint>)
  [2] SRV (t2, space1) - Material ID buffer (StructuredBuffer<uint>)
  [3] SRV (t3, space1) - Material table (StructuredBuffer<Material>)
```

**State Object:**
```
D3D12_STATE_OBJECT_DESC:
  - DXIL Libraries:
    - RayGen.hlsl  -> "RayGen" export
    - ClosestHit.hlsl -> "ClosestHit" export
    - Miss.hlsl -> "Miss" export
    - AnyHit.hlsl -> "AnyHit" export
  - Hit Group: "HitGroup" = { ClosestHit, AnyHit }
  - Shader Config: MaxPayloadSize = sizeof(RayPayload), MaxAttributeSize = 8
  - Pipeline Config: MaxTraceRecursionDepth = 2 (primary + 1 GI bounce)
  - Global Root Signature
  - Local Root Signature (associated with "HitGroup")
```

**Shader Tables:**
- Ray Generation table: 1 record (shader ID only)
- Miss table: 1 record (shader ID only)
- Hit Group table: N records (shader ID + local root arguments per geometry)

**Output Buffer:**
- `RWTexture2D<float4>` at render resolution (e.g., 1280x960 or 1920x1080)
- Used as UAV during ray tracing, copied to swap chain back buffer for present

---

## Phase 2: Geometry Extraction

### 2.1 SceneGeometryExtractor

**Purpose:** Walk N64 F3DEX2 display lists and extract vertex/index buffers with material metadata.

**N64 Vertex Structure (`Vtx`):**
```c
typedef struct {
    s16 ob[3];       // Position (x, y, z)
    u16 flag;        // Unused
    s16 tc[2];       // Texture coords (S, T) in 10.5 fixed-point
    u8  cn[4];       // Color/Normal: (r,g,b,a) or (nx,ny,nz,alpha)
} Vtx;
```

**Extracted Vertex (for DXR):**
```cpp
struct RTXVertex {
    float position[3];  // ob[i] as float (no division needed for OoT)
    float normal[3];    // cn[0..2] as signed normalized (cn[i]/127.0 - 1.0)
    float uv[2];        // tc[i] / 32.0 (10.5 fixed to float)
    float color[4];     // cn[0..3] / 255.0 (when vertex colors, not normals)
};
```

**Display List Commands to Parse:**

| GBI Opcode | Purpose | Data Extracted |
|------------|---------|----------------|
| `G_VTX` | Load vertex buffer | Vertex positions, normals, UVs, colors |
| `G_TRI1` | 1 triangle | 3 vertex indices |
| `G_TRI2` | 2 triangles | 6 vertex indices |
| `G_SETCOMBINE` | Set color combiner | Material combiner mode |
| `G_SETTIMG` | Set texture image | Texture pointer/path |
| `G_LOADBLOCK` / `G_LOADTLUT` | Load texture data | Texture format, size |
| `G_SETGEOMETRYMODE` | Geometry flags | Lighting on/off, backface cull |
| `G_SETOTHERMODE_L` | Render mode | Alpha test, Z buffer, blend mode |
| `G_DL` | Branch to child DL | Recurse |
| `G_ENDDL` | End display list | Stop |

**Material State Machine:**
While walking the display list, track current material state:
```cpp
struct N64MaterialState {
    uint64_t combinerMode;     // From G_SETCOMBINE
    uintptr_t textureAddr;     // From G_SETTIMG (OTR path pointer)
    uint32_t geometryMode;     // From G_SETGEOMETRYMODE
    uint32_t otherModeL;       // From G_SETOTHERMODE_L (render mode)
    uint16_t texWidth;         // From G_SETTILE
    uint16_t texHeight;
    uint8_t texFormat;         // RGBA16, CI8, I4, IA8, etc.
    bool alphaTest;            // Derived from render mode (G_RM_AA_TEX_EDGE)
    bool lightingEnabled;      // G_LIGHTING flag in geometry mode
};
```

**Extraction Flow:**
1. For each room (0, 1, 2):
   - Get `room->meshHeader`
   - Get polygon list: `SEGMENTED_TO_VIRTUAL(polygon->start)`
   - For each `PolygonDlist` entry:
     - Resolve OTR display list via `ResourceMgr_GetResourceByName()`
     - Walk the Gfx command array
     - On `G_VTX`: store pointer to vertex buffer + count
     - On `G_TRI1`/`G_TRI2`: emit triangles using current vertex buffer
     - On `G_SETCOMBINE`/`G_SETTIMG`/etc.: update material state
     - On `G_DL`: recurse into child display list
   - Deduplicate materials, assign material IDs
2. Upload vertex buffer, index buffer, material ID buffer to GPU

**Kokiri Forest Geometry Estimate:**
- Room 0: ~21 display list pairs = ~42 display lists
- Room 1: similar scale
- Room 2: smaller
- Total: likely 5,000-15,000 triangles (N64 geometry is very low poly)

---

## Phase 3: Acceleration Structures

### 3.1 BLAS (Bottom-Level Acceleration Structure)

**One BLAS per room** (static geometry, built once at scene load):
- `spot04_room_0_BLAS` - Main Kokiri Forest area
- `spot04_room_1_BLAS` - Near Deku Tree
- `spot04_room_2_BLAS` - Smaller section

**Build flags:** `D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE` (static, optimize for trace speed)

**Geometry description:**
```
D3D12_RAYTRACING_GEOMETRY_DESC:
  Type = TRIANGLES
  VertexBuffer = room vertex buffer (float3 position at offset 0, stride = sizeof(RTXVertex))
  VertexCount = N
  VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT
  IndexBuffer = room index buffer
  IndexCount = M
  IndexFormat = DXGI_FORMAT_R32_UINT
  Flags = OPAQUE (for room geometry) or NONE (for alpha-tested foliage)
```

**For alpha-tested geometry (foliage):**
- Use `D3D12_RAYTRACING_GEOMETRY_FLAG_NO_DUPLICATE_ANYHIT_INVOCATION`
- Separate geometry desc within the same BLAS
- AnyHit shader checks texture alpha

### 3.2 TLAS (Top-Level Acceleration Structure)

**Rebuilt every frame** (actors move):
```
D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS:
  Type = TOP_LEVEL
  Flags = PREFER_FAST_BUILD (rebuild every frame)
  NumDescs = 3 (rooms) + N (actors)
```

**Instance layout:**
```
Instance 0: spot04_room_0_BLAS, transform = Identity, InstanceID = 0
Instance 1: spot04_room_1_BLAS, transform = Identity, InstanceID = 1
Instance 2: spot04_room_2_BLAS, transform = Identity, InstanceID = 2
Instance 3: Actor BLAS (Link),  transform = actor world matrix, InstanceID = 100
Instance 4: Actor BLAS (NPC),   transform = actor world matrix, InstanceID = 101
...
```

**Actor Geometry (stretch goal, very janky):**
- Actors use `SkelAnime` which generates display lists dynamically each frame
- To include actors in RT: intercept the generated display lists, extract geometry, build per-actor BLAS each frame
- This is expensive. Initial implementation may skip actors entirely and only raytrace static room geometry.
- Actors could be composited via a raster pass on top of the RT output as a compromise.

---

## Phase 4: HLSL Shaders -- DONE

### 4.1 Common.hlsli

```hlsl
// Shared structures and constants

struct SceneConstants {
    float4x4 viewInverse;        // Camera view matrix inverse
    float4x4 projInverse;        // Camera projection matrix inverse
    float3   cameraPos;          // World-space camera position
    uint     frameCount;         // For temporal accumulation
    float3   ambientColor;       // From LightContext
    float    fogNear;            // From LightContext
    float3   fogColor;           // From LightContext
    float    fogFar;             // From LightContext
    float3   sunDirection1;      // Directional light 1
    float    time;               // gameplayFrames for UV scroll
    float3   sunColor1;          // Directional light 1 color
    float    dekuTreeAlpha;      // Vegetation alpha (segment 0x0A)
    float3   sunDirection2;      // Directional light 2
    float    fogBlendAlpha;      // Atmosphere blending (segment 0x0B)
    float3   sunColor2;          // Directional light 2 color
    float    waterScrollOffset;  // For segment 0x0C stream
};

struct RayPayload {
    float3 color;
    float  distance;
    bool   hit;
    uint   recursionDepth;
};

struct RTXVertex {
    float3 position;
    float3 normal;
    float2 uv;
    float4 color;
};

struct Material {
    uint  textureIndex;    // Index into bindless texture array
    uint  combinerMode;    // Simplified combiner ID
    uint  isAlphaTested;   // 1 if alpha test enabled
    uint  isWater;         // 1 if this is a water surface (needs UV scroll)
};

// Common combiner mode IDs (simplified from N64's massive combiner space)
#define COMBINER_MODULATE_RGB     0   // tex * vtxColor
#define COMBINER_MODULATE_RGBA    1   // tex * vtxColor (with alpha)
#define COMBINER_DECAL            2   // tex only
#define COMBINER_SHADE            3   // vtxColor only
#define COMBINER_TEX_ENV_BLEND    4   // lerp(tex, envColor, texAlpha)

// PCG hash for random numbers
uint PCGHash(uint input) {
    uint state = input * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float Random01(uint seed) {
    return float(PCGHash(seed)) / 4294967295.0;
}

float2 RandomInUnitDisk(uint2 pixel, uint frame) {
    uint seed = pixel.x + pixel.y * 8192 + frame * 65536;
    float r = sqrt(Random01(seed));
    float theta = 2.0 * 3.14159265 * Random01(seed + 1);
    return float2(r * cos(theta), r * sin(theta));
}

float3 SampleCosineHemisphere(float3 normal, float2 rand) {
    // Cosine-weighted hemisphere sampling
    float phi = 2.0 * 3.14159265 * rand.x;
    float cosTheta = sqrt(1.0 - rand.y);
    float sinTheta = sqrt(rand.y);

    float3 tangent = abs(normal.y) < 0.999 ?
        normalize(cross(float3(0, 1, 0), normal)) :
        normalize(cross(float3(1, 0, 0), normal));
    float3 bitangent = cross(normal, tangent);

    return normalize(tangent * cos(phi) * sinTheta +
                     bitangent * sin(phi) * sinTheta +
                     normal * cosTheta);
}
```

### 4.2 RayGen.hlsl

```hlsl
#include "Common.hlsli"

RaytracingAccelerationStructure g_scene : register(t0, space0);
RWTexture2D<float4> g_output : register(u0);
RWTexture2D<float4> g_giAccum : register(u1);
ConstantBuffer<SceneConstants> g_constants : register(b0);

[shader("raygeneration")]
void RayGen() {
    uint2 launchIndex = DispatchRaysIndex().xy;
    uint2 launchDim = DispatchRaysDimensions().xy;

    // Camera ray from inverse matrices
    float2 pixelCenter = (float2)launchIndex + 0.5;
    float2 ndc = pixelCenter / (float2)launchDim * 2.0 - 1.0;
    ndc.y = -ndc.y;

    float4 target = mul(g_constants.projInverse, float4(ndc, 1, 1));
    target /= target.w;
    float4 direction = mul(g_constants.viewInverse, float4(target.xyz, 0));

    RayDesc ray;
    ray.Origin = g_constants.cameraPos;
    ray.Direction = normalize(direction.xyz);
    ray.TMin = 1.0;
    ray.TMax = 100000.0;

    RayPayload payload;
    payload.color = float3(0, 0, 0);
    payload.distance = 0;
    payload.hit = false;
    payload.recursionDepth = 0;

    TraceRay(g_scene, RAY_FLAG_NONE, 0xFF, 0, 0, 0, ray, payload);

    // Apply distance fog
    float fogRange = max(g_constants.fogFar - g_constants.fogNear, 1.0);
    float fogFactor = saturate((payload.distance - g_constants.fogNear) / fogRange);
    float3 finalColor = lerp(payload.color, g_constants.fogColor, fogFactor);

    // Temporal accumulation for GI noise reduction
    if (g_constants.frameCount > 0) {
        float3 prevColor = g_giAccum[launchIndex].rgb;
        float weight = 1.0 / min((float)g_constants.frameCount + 1.0, 128.0);
        finalColor = lerp(prevColor, finalColor, weight);
    }

    g_giAccum[launchIndex] = float4(finalColor, 1.0);
    g_output[launchIndex] = float4(finalColor, 1.0);
}
```

### 4.3 ClosestHit.hlsl

```hlsl
#include "Common.hlsli"

RaytracingAccelerationStructure g_scene : register(t0, space0);
ConstantBuffer<SceneConstants> g_constants : register(b0);

StructuredBuffer<RTXVertex> g_vertices : register(t0, space1);
StructuredBuffer<uint> g_indices : register(t1, space1);
StructuredBuffer<uint> g_materialIDs : register(t2, space1);
StructuredBuffer<Material> g_materials : register(t3, space1);
Texture2D g_textures[] : register(t4, space1);
SamplerState g_sampler : register(s0);

[shader("closesthit")]
void ClosestHit(inout RayPayload payload, in BuiltInTriangleIntersectionAttributes attribs) {
    uint primitiveIndex = PrimitiveIndex();
    uint i0 = g_indices[primitiveIndex * 3 + 0];
    uint i1 = g_indices[primitiveIndex * 3 + 1];
    uint i2 = g_indices[primitiveIndex * 3 + 2];

    RTXVertex v0 = g_vertices[i0];
    RTXVertex v1 = g_vertices[i1];
    RTXVertex v2 = g_vertices[i2];

    float3 bary = float3(
        1.0 - attribs.barycentrics.x - attribs.barycentrics.y,
        attribs.barycentrics.x,
        attribs.barycentrics.y
    );

    float3 worldPos = v0.position * bary.x + v1.position * bary.y + v2.position * bary.z;
    float3 normal   = normalize(v0.normal * bary.x + v1.normal * bary.y + v2.normal * bary.z);
    float2 uv       = v0.uv * bary.x + v1.uv * bary.y + v2.uv * bary.z;
    float4 vtxColor = v0.color * bary.x + v1.color * bary.y + v2.color * bary.z;

    // Get material
    uint matID = g_materialIDs[primitiveIndex];
    Material mat = g_materials[matID];

    // Apply water UV scrolling if needed
    if (mat.isWater) {
        uv.y += g_constants.time * 0.01;
    }

    // Sample texture
    float4 texColor = g_textures[NonUniformResourceIndex(mat.textureIndex)].SampleLevel(g_sampler, uv, 0);

    // Apply N64 combiner (simplified for Kokiri Forest's common modes)
    float3 albedo;
    switch (mat.combinerMode) {
        case COMBINER_MODULATE_RGB:
        case COMBINER_MODULATE_RGBA:
            albedo = texColor.rgb * vtxColor.rgb;
            break;
        case COMBINER_DECAL:
            albedo = texColor.rgb;
            break;
        case COMBINER_SHADE:
            albedo = vtxColor.rgb;
            break;
        case COMBINER_TEX_ENV_BLEND:
            albedo = lerp(texColor.rgb, float3(0.5, 0.5, 0.5), texColor.a);
            break;
        default:
            albedo = texColor.rgb * vtxColor.rgb;
            break;
    }

    // Direct lighting
    float NdotL1 = max(dot(normal, -g_constants.sunDirection1), 0.0);
    float NdotL2 = max(dot(normal, -g_constants.sunDirection2), 0.0);
    float3 directLight = g_constants.sunColor1 * NdotL1 + g_constants.sunColor2 * NdotL2;
    float3 ambient = g_constants.ambientColor;
    float3 directShading = albedo * (directLight + ambient);

    // Shadow ray for primary directional light
    RayDesc shadowRay;
    shadowRay.Origin = worldPos + normal * 0.1;
    shadowRay.Direction = -g_constants.sunDirection1;
    shadowRay.TMin = 0.1;
    shadowRay.TMax = 100000.0;

    RayPayload shadowPayload;
    shadowPayload.color = float3(0, 0, 0);
    shadowPayload.hit = false;
    shadowPayload.recursionDepth = payload.recursionDepth + 1;

    // Only trace shadow if we have recursion budget
    float shadow = 1.0;
    if (payload.recursionDepth == 0) {
        TraceRay(g_scene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER,
                 0xFF, 0, 0, 0, shadowRay, shadowPayload);
        shadow = shadowPayload.hit ? 0.3 : 1.0;
    }

    directShading = albedo * (directLight * shadow + ambient);

    // Global illumination bounce (only from primary rays)
    float3 giContribution = float3(0, 0, 0);
    if (payload.recursionDepth == 0) {
        uint2 pixel = DispatchRaysIndex().xy;
        float2 rand = RandomInUnitDisk(pixel, g_constants.frameCount);
        float3 giDir = SampleCosineHemisphere(normal, rand);

        RayDesc giRay;
        giRay.Origin = worldPos + normal * 0.1;
        giRay.Direction = giDir;
        giRay.TMin = 0.1;
        giRay.TMax = 10000.0;

        RayPayload giPayload;
        giPayload.color = float3(0, 0, 0);
        giPayload.recursionDepth = 1;
        giPayload.distance = 0;
        giPayload.hit = false;

        TraceRay(g_scene, RAY_FLAG_NONE, 0xFF, 0, 0, 0, giRay, giPayload);

        if (giPayload.hit) {
            giContribution = giPayload.color * albedo * 0.5;
        }
    }

    payload.color = directShading + giContribution;
    payload.distance = RayTCurrent();
    payload.hit = true;
}
```

### 4.4 Miss.hlsl

```hlsl
#include "Common.hlsli"

ConstantBuffer<SceneConstants> g_constants : register(b0);

[shader("miss")]
void Miss(inout RayPayload payload) {
    // Return fog/sky color
    // Could add a simple sky gradient based on ray direction
    float3 rayDir = WorldRayDirection();
    float t = saturate(rayDir.y * 0.5 + 0.5);

    // Blend between fog color (horizon) and a slightly brighter sky
    float3 skyColor = lerp(g_constants.fogColor, g_constants.fogColor * 1.3, t);

    payload.color = skyColor;
    payload.distance = 100000.0;
    payload.hit = false;
}
```

### 4.5 AnyHit.hlsl

```hlsl
#include "Common.hlsli"

StructuredBuffer<RTXVertex> g_vertices : register(t0, space1);
StructuredBuffer<uint> g_indices : register(t1, space1);
StructuredBuffer<uint> g_materialIDs : register(t2, space1);
StructuredBuffer<Material> g_materials : register(t3, space1);
Texture2D g_textures[] : register(t4, space1);
SamplerState g_sampler : register(s0);
ConstantBuffer<SceneConstants> g_constants : register(b0);

[shader("anyhit")]
void AnyHit(inout RayPayload payload, in BuiltInTriangleIntersectionAttributes attribs) {
    uint primitiveIndex = PrimitiveIndex();
    uint matID = g_materialIDs[primitiveIndex];
    Material mat = g_materials[matID];

    // Only do alpha test for marked materials
    if (!mat.isAlphaTested) return;

    // Interpolate UVs
    uint i0 = g_indices[primitiveIndex * 3 + 0];
    uint i1 = g_indices[primitiveIndex * 3 + 1];
    uint i2 = g_indices[primitiveIndex * 3 + 2];

    float3 bary = float3(
        1.0 - attribs.barycentrics.x - attribs.barycentrics.y,
        attribs.barycentrics.x,
        attribs.barycentrics.y
    );

    float2 uv = g_vertices[i0].uv * bary.x +
                g_vertices[i1].uv * bary.y +
                g_vertices[i2].uv * bary.z;

    float alpha = g_textures[NonUniformResourceIndex(mat.textureIndex)].SampleLevel(g_sampler, uv, 0).a;

    // Apply Deku Tree death alpha fade
    alpha *= g_constants.dekuTreeAlpha;

    if (alpha < 0.5) {
        IgnoreHit();
    }
}
```

### 4.6 Denoise.hlsl (Compute Shader)

```hlsl
// Edge-aware A-trous wavelet denoiser (3 passes)

RWTexture2D<float4> g_input : register(u0);
RWTexture2D<float4> g_output : register(u1);

cbuffer DenoiseConstants : register(b0) {
    int stepSize;       // 1, 2, 4 for 3 passes
    float colorSigma;   // Color weight threshold
    float normalSigma;  // Normal weight threshold (if we had G-buffer)
};

// 5x5 A-trous kernel weights
static const float kernel[5] = { 1.0/16.0, 4.0/16.0, 6.0/16.0, 4.0/16.0, 1.0/16.0 };

[numthreads(8, 8, 1)]
void Denoise(uint3 DTid : SV_DispatchThreadID) {
    float4 centerColor = g_input[DTid.xy];
    float4 sum = float4(0, 0, 0, 0);
    float weightSum = 0.0;

    for (int y = -2; y <= 2; y++) {
        for (int x = -2; x <= 2; x++) {
            int2 offset = int2(x, y) * stepSize;
            int2 samplePos = (int2)DTid.xy + offset;

            // Clamp to image bounds
            samplePos = clamp(samplePos, int2(0, 0), int2(1279, 959)); // TODO: pass dimensions

            float4 sampleColor = g_input[samplePos];

            // Edge-aware weight: reduce weight for large color differences
            float3 colorDiff = centerColor.rgb - sampleColor.rgb;
            float colorDist = dot(colorDiff, colorDiff);
            float colorWeight = exp(-colorDist / (colorSigma * colorSigma + 0.0001));

            float spatialWeight = kernel[x + 2] * kernel[y + 2];
            float weight = spatialWeight * colorWeight;

            sum += sampleColor * weight;
            weightSum += weight;
        }
    }

    g_output[DTid.xy] = sum / max(weightSum, 0.0001);
}
```

---

## Phase 5: Global Illumination -- DONE (in shaders)

### 5.1 Algorithm: 1-Bounce Diffuse Path Tracing with Temporal Accumulation

**Per-pixel, per-frame:**
1. Trace primary ray from camera
2. On hit: compute direct lighting (2 directional lights + ambient + shadow ray)
3. Fire 1 random ray into cosine-weighted hemisphere from hit point
4. If GI ray hits geometry: evaluate that surface's direct lighting
5. Multiply GI result by primary surface albedo
6. Add GI contribution to primary shading result

**Temporal Accumulation:**
- Maintain a running average buffer (`g_giAccum`)
- Each frame: `accumulated = lerp(previous, current, 1.0 / min(frameCount, 128))`
- This converges to a noise-free image over ~64-128 frames when camera is static
- On camera movement: reset accumulation (or use reprojection)

**Camera Motion Detection:**
```cpp
// In RTXRenderer::DrawScene():
bool cameraMoved = (currentViewMatrix != previousViewMatrix);
if (cameraMoved) {
    m_accumulationFrameCount = 0;  // Reset accumulation
} else {
    m_accumulationFrameCount++;
}
```

### 5.2 Expected Visual Results

With 1-bounce GI in Kokiri Forest:
- **Soft indirect light** bouncing off the ground onto tree trunks
- **Color bleeding** from green grass onto nearby surfaces
- **Ambient occlusion** naturally falls out of the path tracing (occluded areas get less indirect light)
- **Light filtering through canopy** - dappled light patterns on the ground
- **Proper shadows** from trees and structures (the N64 original had no real shadows)

### 5.3 Performance Expectations

At 1 SPP (sample per pixel) + 1 GI bounce:
- **RTX 3060 and above:** 30-60 fps at 1080p (N64 geometry is very simple, ~10K triangles)
- **RTX 2060:** 20-40 fps at 1080p
- **Denoiser overhead:** ~1-2ms per frame (3 A-trous passes)
- **TLAS rebuild:** negligible for static scene (no actors initially)

The low polygon count of N64 geometry is actually an advantage here - the acceleration structure is tiny, so ray traversal is very fast.

---

## Phase 6: Texture Management

### 6.1 N64 Texture Formats

Kokiri Forest uses these texture formats (from XML definitions):

| Format | Description | Conversion |
|--------|-------------|------------|
| `rgba16` | 16-bit RGBA (5551) | Unpack to RGBA8 |
| `ci8` | 8-bit color indexed (palette) | Lookup via TLUT, then expand |
| `i4` | 4-bit intensity | Expand to RGBA8 (I,I,I,1) |
| `i8` | 8-bit intensity | Expand to RGBA8 (I,I,I,1) |
| `ia4` | 4-bit intensity + alpha | Expand to RGBA8 (I,I,I,A) |
| `ia8` | 8-bit intensity + alpha | Expand to RGBA8 (I,I,I,A) |
| `ia16` | 16-bit intensity + alpha | Expand to RGBA8 |

### 6.2 TextureManager

**Responsibilities:**
1. Load textures from OTR resource archive by path
2. Decode from N64 format to RGBA8
3. Upload to DX12 committed resources
4. Create SRV descriptors in the shader-visible heap
5. Return texture index for material table

**CI8 Handling (palette textures):**
- CI8 textures reference a TLUT (texture lookup table / palette)
- Palette is typically loaded via `G_LOADTLUT` GBI command
- Must track current palette state while walking display lists
- Expand each CI8 pixel by looking up the palette color
- Kokiri Forest's `spot04_sceneTLUT_00E010` is the scene palette

**Texture Wrapping:**
- N64 uses tile descriptors to set wrap/mirror/clamp
- Track from `G_SETTILE` commands
- Set corresponding DX12 sampler state (wrap by default for Kokiri Forest)

### 6.3 HD Texture Support

Ship of Harkinian supports alternate assets via `CVAR_SETTING("AltAssets")`. If HD textures are loaded:
- The OTR resource manager returns the HD version automatically
- These are typically pre-decoded PNG/DDS
- The TextureManager should handle both N64 and HD texture formats

---

## Phase 7: Scene Integration Hooks -- DONE

### 7.1 Hook 1: Scene Draw Config (z_scene_table.c)

**Location:** `func_8009E0B8()` at `soh/src/code/z_scene_table.c:1110`

**Strategy:** Add an early-out check at the top of the function:

```c
void func_8009E0B8(PlayState* play) {
    // RTX HOOK: If RTX renderer is active, extract scene parameters
    // and skip all GBI commands
    if (RTX_IsActive() && RTX_IsKokiriForest(play)) {
        RTX_UpdateSceneParams(play);
        return;  // Skip all display list setup
    }

    // ... original function body unchanged ...
}
```

**RTX_UpdateSceneParams extracts:**
- `play->gameplayFrames` (for water UV scroll time)
- `gSaveContext.sceneSetupIndex` (for Deku Tree death effects)
- `play->roomCtx.unk_74[0]` (for alpha fade and fog distance)
- Fog distance (spA0): 500 default, 2150 after Deku Tree death
- Vegetation alpha (spA3): 128 default, fading during cutscene

### 7.2 Hook 2: Room Draw Suppression (z_room.c)

**Location:** `Room_Draw()` function, or intercept at `z_play.c:1550-1551`

**Strategy:** Skip room draw calls when RTX is active:

```c
// In z_play.c Play_Draw(), around line 1549:
Scene_Draw(play);
if (!RTX_IsActive()) {
    Room_Draw(play, &play->roomCtx.curRoom, roomDrawFlags & 3);
    Room_Draw(play, &play->roomCtx.prevRoom, roomDrawFlags & 3);
}
// RTX renderer handles all room geometry via acceleration structures
```

### 7.3 Hook 3: Frame Present (OTRGlobals.cpp)

**Location:** `Graph_ProcessGfxCommands()` at `soh/soh/OTRGlobals.cpp:1726`

**Strategy:** Redirect to DX12 present when RTX is active:

```cpp
extern "C" void Graph_ProcessGfxCommands(Gfx* commands) {
    // ... audio sync code (keep) ...

    if (RTX::Renderer::IsActive()) {
        // RTX path: dispatch rays and present via DX12 swap chain
        RTX::Renderer::Instance()->DispatchAndPresent();
    } else {
        // Original Fast3D path
        std::vector<std::unordered_map<Mtx*, MtxF>> mtx_replacements;
        // ... original interpolation and RunCommands code ...
        RunCommands(commands, mtx_replacements);
    }

    // ... rest of function (alt assets check, etc.) ...
}
```

### 7.4 Hook 4: Scene Load (z_scene_otr.cpp)

**Location:** `OTRfunc_800973FC()` - After room resources are loaded

**Strategy:** Trigger geometry extraction when Kokiri Forest loads:

```cpp
// After room load completes:
if (play->sceneNum == SCENE_KOKIRI_FOREST) {
    RTX::Renderer::Instance()->OnRoomLoaded(play, roomNum);
    // This triggers:
    //   1. Display list geometry extraction
    //   2. Texture loading
    //   3. BLAS building
}
```

### 7.5 Hook 5: Scene Transition

When leaving Kokiri Forest:
```cpp
// In scene transition code:
if (RTX::Renderer::IsActive()) {
    RTX::Renderer::Instance()->OnSceneUnload();
    // Releases BLAS/TLAS, textures, GPU buffers
}
```

---

## Phase 8: Kokiri Forest Specifics

### 8.1 Water/Stream Animation

The scene draw config scrolls textures on segments 0x08, 0x09, 0x0C:

| Segment | Surface | Scroll Speed | Texture Size |
|---------|---------|-------------|-------------|
| 0x08 | Fast water | `gameplayFrames * 10 % 128` | 32x32 |
| 0x09 | Slow water | `gameplayFrames * 1 % 128` | 32x32 |
| 0x0C | Stream/waterfall | `unk_74[0] * 0.02` | 32x16 |

**RT Implementation:**
- Tag water triangles with `isWater = 1` in material table
- Pass `time = play->gameplayFrames` in SceneConstants
- In ClosestHit shader: offset UVs based on time and material type
- Different scroll speeds per material (slow water, fast water, waterfall)

### 8.2 Deku Tree Death Effects

**Alpha Fade (sceneSetupIndex == 4):**
- `dekuTreeAlpha = (255 - play->roomCtx.unk_74[0]) / 255.0`
- Applied to foliage in AnyHit shader
- Vegetation on segment 0x0A gradually becomes transparent

**Fog Distance Change:**
- Default fog: near=500
- After Deku Tree death: near=2150
- During cutscene (setupIndex == 6): `fog = unk_74[0] + 500`
- Pass as `fogNear` in SceneConstants

### 8.3 Foliage / Tree Canopy

Kokiri Forest has heavy tree canopy with alpha-tested textures:
- CI8 format textures with palette-based alpha
- Bound to segment 0x0A with env color alpha
- Must use AnyHit shader for correct alpha testing
- Separate geometry desc in BLAS with `D3D12_RAYTRACING_GEOMETRY_FLAG_NO_DUPLICATE_ANYHIT_INVOCATION`

With RT, the canopy will cast **dappled shadows** on the ground - a major visual improvement over the N64 original, which had no real shadows.

### 8.4 Time-of-Day Lighting

Kokiri Forest's lighting changes with time of day via `EnvLightSettings`:
- `ambientColor` shifts between warm daylight and cool night
- `diffuseDir1/2` rotate with sun/moon position
- `diffuseColor1/2` change color temperature
- `fogColor` shifts with atmosphere

**RT Implementation:**
- Read current interpolated values from `play->envCtx`
- Pass as directional light parameters in SceneConstants
- The RT renderer gets physically accurate sun/moon shadows for free

---

## Phase 9: Build System Integration

### 9.1 CMake Changes (soh/CMakeLists.txt)

**New compile definition:**
```cmake
if("${CMAKE_VS_PLATFORM_NAME}" STREQUAL "x64")
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        # ... existing definitions ...
        "ENABLE_DX12_RTX"
    )
endif()
```

**New link libraries:**
```cmake
if("${CMAKE_VS_PLATFORM_NAME}" STREQUAL "x64")
    list(APPEND ADDITIONAL_LIBRARY_DEPENDENCIES
        "d3d12"
        "dxgi"
        "dxcompiler"
    )
endif()
```

**Shader compilation (DXC):**
```cmake
# Compile HLSL shaders to DXIL
find_program(DXC dxc)

set(RTX_SHADER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/soh/Enhancements/rtx/shaders)
set(RTX_SHADER_OUTPUT_DIR ${CMAKE_BINARY_DIR}/soh/shaders)

# Compile each shader
foreach(SHADER RayGen ClosestHit Miss AnyHit)
    add_custom_command(
        OUTPUT ${RTX_SHADER_OUTPUT_DIR}/${SHADER}.dxil
        COMMAND ${DXC} -T lib_6_3 -Fo ${RTX_SHADER_OUTPUT_DIR}/${SHADER}.dxil
                ${RTX_SHADER_DIR}/${SHADER}.hlsl
        DEPENDS ${RTX_SHADER_DIR}/${SHADER}.hlsl ${RTX_SHADER_DIR}/Common.hlsli
    )
endforeach()

# Denoise is a compute shader
add_custom_command(
    OUTPUT ${RTX_SHADER_OUTPUT_DIR}/Denoise.dxil
    COMMAND ${DXC} -T cs_6_0 -E Denoise -Fo ${RTX_SHADER_OUTPUT_DIR}/Denoise.dxil
            ${RTX_SHADER_DIR}/Denoise.hlsl
    DEPENDS ${RTX_SHADER_DIR}/Denoise.hlsl
)
```

### 9.2 Preprocessor Guards

All RTX code is guarded by `#ifdef ENABLE_DX12_RTX`:

```cpp
// In z_scene_table.c:
#ifdef ENABLE_DX12_RTX
#include "soh/Enhancements/rtx/RTXRenderer.h"
#endif

void func_8009E0B8(PlayState* play) {
#ifdef ENABLE_DX12_RTX
    if (RTX_IsActive()) {
        RTX_UpdateSceneParams(play);
        return;
    }
#endif
    // ... original code ...
}
```

### 9.3 Required Windows SDK

- Minimum: Windows SDK 10.0.19041.0 (Windows 10 May 2020 Update)
- DXR requires: `d3d12.h` with `ID3D12Device5`, `ID3D12GraphicsCommandList4`
- Shader Model 6.3+ for DXR HLSL intrinsics

### 9.4 Submodule Initialization

Before any build, submodules must be initialized:
```bash
git submodule update --init --recursive
```

This pulls in `libultraship`, `ZAPDTR`, and `OTRExporter` which are required even though RTX bypasses Fast3D - the resource manager, window system, input, and audio all live in libultraship.

---

## Known Limitations and Jank

### Things That Will Be Janky

1. **No actor rendering in RT path (initially)**
   - Link, Kokiri NPCs, Navi, etc. use `SkelAnime` which generates display lists dynamically per frame
   - Extracting their geometry every frame for BLAS rebuilds is expensive
   - **Mitigation:** Composite actors via a raster pass on top of RT output, or skip them initially

2. **Shadow artifacts**
   - The N64 original had zero shadows. Adding raytraced shadows means every tree casts shadows that were never designed for
   - Tree shadow patterns on the ground will look "wrong" compared to what players expect
   - Self-shadowing on low-poly geometry will produce hard edges

3. **GI noise at low sample counts**
   - 1 SPP GI will be very noisy per frame
   - Temporal accumulation helps when camera is still, but motion causes ghosting
   - The denoiser is basic (A-trous only, no motion vectors for reprojection)

4. **N64 combiner approximation**
   - The N64 RDP color combiner is extremely flexible (4-stage, per-cycle)
   - We approximate the ~5-10 combiner modes actually used in Kokiri Forest
   - Some materials may look slightly wrong

5. **Two swap chains**
   - The game uses Fast3D's swap chain for all other scenes
   - Kokiri Forest uses DX12's swap chain
   - Transitioning between them on scene change may cause flicker

6. **Frame interpolation incompatibility**
   - The existing `frame_interpolation.cpp` system won't work with RT
   - Camera movement will be at game logic rate (20 fps) unless we add our own interpolation

7. **Window handle sharing**
   - DX12 and the existing DX11/OpenGL context both need the same HWND
   - May need to destroy the old swap chain before creating the DX12 one

8. **CI8 texture palette tracking**
   - Must correctly track which TLUT is active when geometry references CI8 textures
   - If we miss a palette load command, textures will have wrong colors

### Things That Will Look Great

1. **Soft indirect illumination** - Light bouncing off grass onto tree trunks
2. **Proper tree shadows** - Dappled sunlight through canopy
3. **Ambient occlusion** - Natural darkening in crevices and under structures
4. **Color bleeding** - Green tint from grass reflecting onto nearby surfaces
5. **Accurate fog** - Ray-marched distance fog instead of per-vertex

---

## File Structure

```
soh/soh/Enhancements/rtx/
├── DX12Device.h              # DX12 device, swap chain, command queue
├── DX12Device.cpp
├── DXRPipeline.h             # State object, root signature, shader tables
├── DXRPipeline.cpp
├── SceneGeometryExtractor.h  # Walk N64 display lists -> vertex/index buffers
├── SceneGeometryExtractor.cpp
├── AccelerationStructure.h   # BLAS/TLAS build and management
├── AccelerationStructure.cpp
├── TextureManager.h          # N64 texture decode -> DX12 SRV heap
├── TextureManager.cpp
├── RTXRenderer.h             # Orchestrator: extract, trace, denoise, present
├── RTXRenderer.cpp
├── RTXHooks.h                # C-callable hooks for scene_table, room, graph
├── RTXHooks.cpp
└── shaders/
    ├── Common.hlsli           # Shared structures, RNG, hemisphere sampling
    ├── RayGen.hlsl            # Primary ray generation + fog + accumulation
    ├── ClosestHit.hlsl        # Material eval + direct light + GI bounce
    ├── Miss.hlsl              # Sky/fog color
    ├── AnyHit.hlsl            # Alpha test for foliage
    └── Denoise.hlsl           # Edge-aware A-trous wavelet denoiser (compute)
```

**Modified existing files:**
- `soh/src/code/z_scene_table.c` - Hook in `func_8009E0B8()` (line 1110)
- `soh/src/code/z_play.c` - Suppress `Room_Draw()` calls (line 1550-1551)
- `soh/soh/OTRGlobals.cpp` - Hook in `Graph_ProcessGfxCommands()` (line 1726)
- `soh/soh/z_scene_otr.cpp` - Trigger geometry extraction on room load
- `soh/CMakeLists.txt` - Add DX12 compile defs + link libraries + shader compilation

---

## Implementation Order

1. ~~**Initialize submodules** - Get the project building~~
2. ~~**DX12Device** - Get a DX12 device + swap chain working, present a solid color~~ **DONE**
3. ~~**DXRPipeline** - Create state object, verify DXR support, dispatch empty rays~~ **DONE**
4. ~~**SceneGeometryExtractor** - Extract geometry from one room display list~~ **DONE**
5. ~~**AccelerationStructure** - Build BLAS from extracted geometry, verify structure~~ **DONE**
6. ~~**Shaders (basic)** - RayGen + ClosestHit (no textures, flat color) + Miss~~ **DONE**
7. **TextureManager** - Decode and upload textures, bind to SRV heap
8. ~~**Shaders (textured)** - Add texture sampling and combiner logic~~ **DONE**
9. ~~**Scene hooks** - Wire up func_8009E0B8, Room_Draw suppression, present hook~~ **DONE**
10. ~~**GI** - Add bounce ray in ClosestHit, temporal accumulation in RayGen~~ **DONE**
11. ~~**Denoise** - A-trous wavelet passes~~ **DONE**
12. ~~**Kokiri specifics** - Water scroll, fog, Deku Tree alpha, foliage AnyHit~~ **DONE** (in shaders + renderer)
13. ~~**Polish** - Shadow rays, camera motion detection, accumulation reset~~ **DONE** (in RTXRenderer.cpp)
14. **CMake integration** - Compile flags, shader compilation, conditional build
