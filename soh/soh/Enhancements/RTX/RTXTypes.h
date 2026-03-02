#pragma once
#ifndef RTX_TYPES_H
#define RTX_TYPES_H

#ifdef ENABLE_DX12_RTX

#include <cstdint>
#include <cstddef>
#include <vector>
#include <string>

namespace RTX {

// Maximum number of bindless textures in the SRV descriptor heap.
// Shared between DXRPipeline (root signature descriptor range) and
// TextureManager (heap allocation). Must stay in sync.
constexpr uint32_t MAX_BINDLESS_TEXTURES = 4096;

// Matches the HLSL RTXVertex in Common.hlsli
struct RTXVertex {
    float position[3];  // World-space position
    float normal[3];    // World-space normal (or vertex color encoded as normal)
    float uv[2];        // Texture coordinates (10.5 fixed -> float)
    float color[4];     // Vertex color RGBA [0,1]
};

// RTXVertex must be exactly 48 bytes (12 x float) to match HLSL StructuredBuffer<RTXVertex> layout.
static_assert(sizeof(RTXVertex) == 48, "RTXVertex struct must be 48 bytes to match HLSL layout");

// Matches the HLSL Material in Common.hlsli
struct Material {
    uint32_t textureIndex;   // Index into bindless texture SRV array
    uint32_t combinerMode;   // Simplified N64 combiner mode ID
    uint32_t isAlphaTested;  // 1 if alpha test enabled
    uint32_t isWater;        // 1 if water surface (needs UV scroll)
    // Texture wrap mode flags from N64 G_SETTILE command.
    // Bits 0-1: S wrap mode (0=WRAP, 1=MIRROR, 2=CLAMP, 3=MIRROR+CLAMP)
    // Bits 2-3: T wrap mode (0=WRAP, 1=MIRROR, 2=CLAMP, 3=MIRROR+CLAMP)
    // N64 uses mask_s/mask_t to control wrap (mask=0 means clamp), plus
    // explicit clamp bits. We simplify to: 0=wrap, 2=clamp, 1=mirror.
    uint32_t wrapModeS;      // S (horizontal) wrap mode: 0=WRAP, 1=MIRROR, 2=CLAMP
    uint32_t wrapModeT;      // T (vertical) wrap mode: 0=WRAP, 1=MIRROR, 2=CLAMP
    // Texture dimensions in texels (from G_SETTILESIZE) — used by the shader
    // for texture-size-adaptive UV clamping. The CLAMP mode inset is computed
    // as 0.5/texWidth (half-texel margin) for exact per-texture clamping.
    uint32_t texWidthPx;     // Texture width in texels (e.g., 32)
    uint32_t texHeightPx;    // Texture height in texels (e.g., 32)
    uint32_t isDecal;        // 1 if decal/overlay geometry (ZMODE_DEC, e.g., dirt paths)
    uint32_t _materialPad;   // Padding to maintain 16-byte alignment for GPU upload
};

// Material struct must be exactly 40 bytes (10 x uint32_t) to match HLSL StructuredBuffer<Material> layout.
// If this fails, the HLSL and C++ Material structs are out of sync and the GPU will read garbage.
static_assert(sizeof(Material) == 40, "Material struct must be 40 bytes to match HLSL layout");

// Simplified combiner mode IDs (matches Common.hlsli defines)
enum CombinerMode : uint32_t {
    COMBINER_MODULATE_RGB   = 0, // tex * vtxColor
    COMBINER_MODULATE_RGBA  = 1, // tex * vtxColor (with alpha)
    COMBINER_DECAL          = 2, // tex only
    COMBINER_SHADE          = 3, // vtxColor only
    COMBINER_TEX_ENV_BLEND  = 4, // lerp(tex, envColor, texAlpha)
};

// Matches the HLSL SceneConstants in Common.hlsli
// MUST match the GPU cbuffer layout byte-for-byte.
// Total size: 384 bytes (aligned to 16 bytes, padded for CBV 256-byte alignment).
//
// BYTE OFFSET MAP (for shader matching):
//   offset   0: viewMatrix (float4x4, 64 bytes)
//   offset  64: projMatrix (float4x4, 64 bytes)
//   offset 128: invViewMatrix (float4x4, 64 bytes)
//   offset 192: invProjMatrix (float4x4, 64 bytes)
//   offset 256: sunDirection (float4, 16 bytes)
//   offset 272: sunColor (float4, 16 bytes)
//   offset 288: ambientColor (float4, 16 bytes)
//   offset 304: fogColor (float4, 16 bytes)
//   offset 320: fogStart, fogEnd, sunIntensity, ambientIntensity (4 floats, 16 bytes)
//   offset 336: giIntensity, reflectionIntensity, skyIntensity, exposure (4 floats, 16 bytes)
//   offset 352: frameCount, toneMapMode, skyBlendFactor(float-as-bits), debugMode (16 bytes)
//   offset 368: pad0, pad1, pad2, pad3 (16 bytes padding)
//   Total: 384 bytes
struct SceneConstants {
    // --- Camera matrices (offsets 0-255, 4x float4x4 = 256 bytes) ---
    // Stored as float[16] in ROW-MAJOR order (matching HLSL row_major float4x4).
    // Each matrix is 64 bytes (4 rows x 4 columns x 4 bytes/float).
    float viewMatrix[16];      // offset   0: Camera view matrix (world -> view space)
    float projMatrix[16];      // offset  64: Camera projection matrix (view -> clip space)
    float invViewMatrix[16];   // offset 128: Inverse view matrix (view -> world space)
    float invProjMatrix[16];   // offset 192: Inverse projection matrix (clip -> view space)

    // --- Lighting (offsets 256-319, float4 vectors) ---
    float sunDirection[4];     // offset 256: Directional light direction (normalized, xyz, w=0)
    float sunColor[4];         // offset 272: Directional light color (rgba, a=1)
    float ambientColor[4];     // offset 288: Ambient light color (rgba, a=1)
    float fogColor[4];         // offset 304: Fog color (rgba, a=1)

    // --- Scalar parameters (offsets 320-351) ---
    float fogStart;            // offset 320: Fog start distance
    float fogEnd;              // offset 324: Fog end distance
    float sunIntensity;        // offset 328: Sun light intensity multiplier
    float ambientIntensity;    // offset 332: Ambient light intensity multiplier
    float giIntensity;         // offset 336: GI contribution multiplier
    float reflectionIntensity; // offset 340: Reflection intensity multiplier
    float skyIntensity;        // offset 344: Sky intensity multiplier
    float exposure;            // offset 348: Exposure multiplier for post-process

    // --- Integer/mixed parameters (offsets 352-367) ---
    uint32_t frameCount;       // offset 352: For temporal accumulation + RNG seed
    uint32_t toneMapMode;      // offset 356: 0=ACES, 1=Reinhard, 2=Linear
    float skyBlendFactor;      // offset 360: Sky blend factor [0,1]
    int32_t debugMode;         // offset 364: 0=normal, 1=albedo, 2=normals, 3=lighting, 4=depth

    // --- Padding to 384 bytes (offsets 368-383) ---
    float pad0;                // offset 368
    float pad1;                // offset 372
    float pad2;                // offset 376
    float pad3;                // offset 380
};

// ============================================================================
// Compile-time struct layout verification.
// These static_asserts ensure the C++ SceneConstants struct matches the HLSL
// cbuffer layout defined in Common.hlsli. Any misalignment would cause the
// shader to read wrong values from the constant buffer.
// ============================================================================
static_assert(offsetof(SceneConstants, viewMatrix)          == 0,   "viewMatrix must be at offset 0");
static_assert(offsetof(SceneConstants, projMatrix)          == 64,  "projMatrix must be at offset 64");
static_assert(offsetof(SceneConstants, invViewMatrix)       == 128, "invViewMatrix must be at offset 128");
static_assert(offsetof(SceneConstants, invProjMatrix)       == 192, "invProjMatrix must be at offset 192");
static_assert(offsetof(SceneConstants, sunDirection)        == 256, "sunDirection must be at offset 256");
static_assert(offsetof(SceneConstants, sunColor)            == 272, "sunColor must be at offset 272");
static_assert(offsetof(SceneConstants, ambientColor)        == 288, "ambientColor must be at offset 288");
static_assert(offsetof(SceneConstants, fogColor)            == 304, "fogColor must be at offset 304");
static_assert(offsetof(SceneConstants, fogStart)            == 320, "fogStart must be at offset 320");
static_assert(offsetof(SceneConstants, fogEnd)              == 324, "fogEnd must be at offset 324");
static_assert(offsetof(SceneConstants, sunIntensity)        == 328, "sunIntensity must be at offset 328");
static_assert(offsetof(SceneConstants, ambientIntensity)    == 332, "ambientIntensity must be at offset 332");
static_assert(offsetof(SceneConstants, giIntensity)         == 336, "giIntensity must be at offset 336");
static_assert(offsetof(SceneConstants, reflectionIntensity) == 340, "reflectionIntensity must be at offset 340");
static_assert(offsetof(SceneConstants, skyIntensity)        == 344, "skyIntensity must be at offset 344");
static_assert(offsetof(SceneConstants, exposure)            == 348, "exposure must be at offset 348");
static_assert(offsetof(SceneConstants, frameCount)          == 352, "frameCount must be at offset 352");
static_assert(offsetof(SceneConstants, toneMapMode)         == 356, "toneMapMode must be at offset 356");
static_assert(offsetof(SceneConstants, skyBlendFactor)      == 360, "skyBlendFactor must be at offset 360");
static_assert(offsetof(SceneConstants, debugMode)           == 364, "debugMode must be at offset 364");
static_assert(offsetof(SceneConstants, pad0)                == 368, "pad0 must be at offset 368");
static_assert(offsetof(SceneConstants, pad1)                == 372, "pad1 must be at offset 372");
static_assert(offsetof(SceneConstants, pad2)                == 376, "pad2 must be at offset 376");
static_assert(offsetof(SceneConstants, pad3)                == 380, "pad3 must be at offset 380");
// Total size: 384 bytes (16-byte aligned, suitable for CBV).
static_assert(sizeof(SceneConstants) == 384, "SceneConstants must be exactly 384 bytes to match GPU cbuffer layout");

// Mesh data extracted from a single N64 display list
struct ExtractedMesh {
    std::vector<RTXVertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<uint32_t> materialIDs; // Per-triangle material ID
    std::vector<Material> materials;

    // Parallel to materials[]: stores the full raw texture address (uintptr_t)
    // for each material. Used by RTXRenderer::ResolveMaterialTextures() to
    // compute the correct hash for looking up the SRV index from TextureManager.
    // Material::textureIndex is initially set to 0 (default white) and resolved
    // to the correct SRV index before the material buffer is uploaded to the GPU.
    std::vector<uintptr_t> materialTextureAddrs;

    // Parallel to materials[]: stores durable copies of OTR texture path strings.
    // The pointers in materialTextureAddrs may become invalid after the
    // SceneGeometryExtractor is reused (its internal string storage is cleared).
    // These strings survive for the lifetime of the ExtractedMesh, enabling
    // deferred texture loading during re-resolve passes in DispatchAndPresent.
    std::vector<std::string> materialTexturePaths;

    // Parallel to materials[]: stores durable copies of TLUT (palette) OTR path strings.
    // For CI4/CI8 textures, this is the path of the TLUT resource needed to decode the texture.
    // Empty string means no TLUT was set before this material's texture.
    std::vector<std::string> materialTlutPaths;

    // Parallel to materials[]: stores per-material texture format info from the display list.
    // These are needed by ResolveMaterialTextures to properly decode OTR textures.
    struct MaterialTexInfo {
        uint8_t texFormat;  // N64 image format (G_IM_FMT_*)
        uint8_t texSize;    // N64 image size (G_IM_SIZ_*)
        uint16_t texWidth;  // Texture width in texels (from G_SETTILESIZE)
        uint16_t texHeight; // Texture height in texels (from G_SETTILESIZE)
    };
    std::vector<MaterialTexInfo> materialTexInfos;

    bool hasAlphaTest = false;
};

// Per-room extracted geometry ready for BLAS building
struct RoomGeometry {
    uint32_t roomIndex;
    ExtractedMesh opaqueMesh;   // Opaque geometry (BLAS flag: OPAQUE)
    ExtractedMesh alphaMesh;    // Alpha-tested geometry (BLAS flag: NONE, uses AnyHit)
};

// N64 material state tracked while walking display lists
struct N64MaterialState {
    uint64_t combinerMode;     // Raw G_SETCOMBINE value
    uintptr_t textureAddr;     // OTR path pointer from G_SETTIMG
    uintptr_t prevTextureAddr; // Previous textureAddr before the most recent G_SETTIMG.
                               // Used by G_LOADTLUT: the TLUT load sequence is
                               // G_SETTIMG(texture) → G_SETTIMG(TLUT) → G_LOADTLUT.
                               // After G_LOADTLUT saves the TLUT, we restore textureAddr
                               // to prevTextureAddr so the material references the actual
                               // texture, not the TLUT.
    uint8_t prevTexFormat;     // texFormat saved alongside prevTextureAddr
    uint8_t prevTexSize;       // texSize saved alongside prevTextureAddr
    uintptr_t lastTlutAddr;    // OTR path pointer of the last TLUT set (for CI textures)
    uint32_t geometryMode;     // From G_SETGEOMETRYMODE
    uint32_t otherModeL;       // From G_SETOTHERMODE_L (render mode)
    uint16_t texWidth;         // From G_SETTILE / G_SETTILESIZE
    uint16_t texHeight;
    uint8_t texFormat;         // N64 image format (G_IM_FMT_*: 0=RGBA, 2=CI, 3=IA, 4=I)
    uint8_t texSize;           // N64 image size   (G_IM_SIZ_*: 0=4b, 1=8b, 2=16b, 3=32b)
    bool alphaTest;            // Derived from render mode
    bool lightingEnabled;      // G_LIGHTING flag

    // Texture wrap modes from G_SETTILE (tile 0).
    // N64 tile descriptor has mirror and clamp bits per axis plus a mask field:
    //   mask=0 → clamp (N64 hardware: mask=0 disables wrapping entirely)
    //   mask>0, mirror=0, clamp=0 → wrap (power-of-2 wrap via mask)
    //   mask>0, mirror=1, clamp=0 → mirror (reflect at tile boundary)
    //   mask>0, mirror=0, clamp=1 → clamp (clamp at tile edge)
    //   mask>0, mirror=1, clamp=1 → mirror+clamp (we simplify to CLAMP)
    // We simplify: 0=WRAP, 1=MIRROR, 2=CLAMP
    uint8_t wrapModeS;         // S (horizontal): 0=WRAP, 1=MIRROR, 2=CLAMP
    uint8_t wrapModeT;         // T (vertical):   0=WRAP, 1=MIRROR, 2=CLAMP

    // G_TEXTURE scale factors (Q0.16 fixed-point from the N64's G_TEXTURE command).
    // 0xFFFF = 1.0, 0x8000 = 0.5, 0x0000 = 0.0 (texture disabled).
    // The RSP multiplies each vertex's texture coordinates by these scale factors
    // before rasterization. Most OoT geometry uses 0xFFFF (1.0).
    // We convert to float for UV scaling: scale = rawScale / 65536.0f.
    float texScaleS;           // Texture S axis scale (from G_TEXTURE), default 1.0
    float texScaleT;           // Texture T axis scale (from G_TEXTURE), default 1.0
    bool texEnabled;           // True if texture is enabled (from G_TEXTURE on bit)

    // G_SETTILE shift values (4 bits each, from tile descriptor shift_s/shift_t).
    // The N64 RDP applies a power-of-2 shift to texture coordinates after the
    // RSP's G_TEXTURE scale. This is used for texture LOD and mipmap selection.
    //   shift < 11: right-shift TCs by `shift` bits (divide by 2^shift)
    //   shift >= 11: left-shift TCs by `16-shift` bits (multiply by 2^(16-shift))
    //   shift == 0: no shift (most common in OoT)
    // We convert to a float multiplier in ConvertVertex for UV computation.
    uint8_t tileShiftS;        // Tile shift_s (0-15), default 0
    uint8_t tileShiftT;        // Tile shift_t (0-15), default 0

    void Reset() {
        combinerMode = 0;
        textureAddr = 0;
        prevTextureAddr = 0;
        prevTexFormat = 0;
        prevTexSize = 0;
        lastTlutAddr = 0;
        geometryMode = 0;
        otherModeL = 0;
        texWidth = 32;
        texHeight = 32;
        texFormat = 0;
        texSize = 0;
        alphaTest = false;
        lightingEnabled = false;
        wrapModeS = 0;  // Default: WRAP
        wrapModeT = 0;  // Default: WRAP
        texScaleS = 1.0f; // Default: full scale
        texScaleT = 1.0f; // Default: full scale
        texEnabled = true; // Default: enabled
        tileShiftS = 0;   // Default: no shift
        tileShiftT = 0;   // Default: no shift
    }
};

// Denoise pass constants (matches Denoise.hlsl cbuffer)
// Default constructor initializes to safe defaults. GISystem::GetDenoiseConstants()
// overrides these with per-pass values (stepSize, sigma parameters) when active.
struct DenoiseConstants {
    int32_t stepSize;      // 1, 2, 4, 8 for 4 A-trous passes (power of 2)
    float colorSigma;      // Luminance edge-stopping threshold (0.1 typical)
    float normalSigma;     // Normal edge-stopping exponent (128.0 typical for N64 hard edges)
    float depthSigma;      // Depth edge-stopping threshold (0.02 typical, gradient-aware in shader)

    DenoiseConstants()
        : stepSize(0)       // Default: no filtering (GISystem overrides per-pass)
        , colorSigma(0.0f)  // Default: no color filtering (GISystem overrides)
        , normalSigma(0.0f) // Default: no normal filtering (GISystem overrides)
        , depthSigma(0.0f)  // Default: no depth filtering (GISystem overrides)
    {}
};

// Temporal accumulation constants (matches Accumulate.hlsl cbuffer)
// Default constructor initializes to safe defaults. GISystem::GetAccumulateConstants()
// overrides these with per-frame values (blendAlpha, frameCount) when active.
struct AccumulateConstants {
    uint32_t resolutionX;   // Output width
    uint32_t resolutionY;   // Output height
    uint32_t frameCount;    // Accumulation frame count (0 = reset/camera moved)
    float    blendAlpha;    // EMA blend factor for new frame (0.0 = no history, ~0.1 = smooth)

    AccumulateConstants()
        : resolutionX(0)
        , resolutionY(0)
        , frameCount(0)     // Default: reset (no accumulation until GISystem sets it)
        , blendAlpha(0.0f)  // Default: no temporal blending (GISystem overrides to ~0.1)
    {}
};

// Post-process constants (matches PostProcess.hlsl cbuffer)
// HLSL layout:
//   offset  0: uint2  resolution       (8 bytes)
//   offset  8: float  exposure          (4 bytes)
//   offset 12: uint   toneMapMode       (4 bytes)
//   offset 16: float  vignetteStrength  (4 bytes)
//   offset 20: float  saturation        (4 bytes)
//   offset 24: float  contrast          (4 bytes)
//   offset 28: int    debugMode         (4 bytes)
//   Total: 32 bytes
struct PostProcessConstants {
    uint32_t resolutionX;      // offset  0: Output width
    uint32_t resolutionY;      // offset  4: Output height
    float    exposure;          // offset  8: Exposure multiplier (default 1.0)
    uint32_t toneMapMode;      // offset 12: 0=ACES, 1=Reinhard, 2=Linear
    float    vignetteStrength;  // offset 16: 0.0 = off, 0.3 = subtle
    float    saturation;        // offset 20: Color saturation (1.0 = neutral)
    float    contrast;          // offset 24: Contrast (1.0 = neutral)
    int32_t  debugMode;         // offset 28: 0=normal, 1=albedo, 2=normals, 3=lighting
};
static_assert(sizeof(PostProcessConstants) == 32, "PostProcessConstants must be 32 bytes to match HLSL cbuffer");
static_assert(offsetof(PostProcessConstants, resolutionX) == 0, "resolutionX must be at offset 0");
static_assert(offsetof(PostProcessConstants, resolutionY) == 4, "resolutionY must be at offset 4");
static_assert(offsetof(PostProcessConstants, exposure) == 8, "exposure must be at offset 8");
static_assert(offsetof(PostProcessConstants, toneMapMode) == 12, "toneMapMode must be at offset 12");
static_assert(offsetof(PostProcessConstants, vignetteStrength) == 16, "vignetteStrength must be at offset 16");
static_assert(offsetof(PostProcessConstants, saturation) == 20, "saturation must be at offset 20");
static_assert(offsetof(PostProcessConstants, contrast) == 24, "contrast must be at offset 24");
static_assert(offsetof(PostProcessConstants, debugMode) == 28, "debugMode must be at offset 28");

// ============================================================================
// Texture-related type declarations
// These provide lightweight types for cross-module use without requiring
// DX12 headers. Full texture types with ComPtr members are in TextureManager.h.
// ============================================================================

// Texture format descriptor for N64 texture conversion.
// Used to pass texture format info between SceneGeometryExtractor and TextureManager.
struct TextureFormatDesc {
    uint32_t format;       // N64 image format (G_IM_FMT_*: 0=RGBA, 1=YUV, 2=CI, 3=IA, 4=I)
    uint32_t size;         // N64 image size (G_IM_SIZ_*: 0=4b, 1=8b, 2=16b, 3=32b)
    uint32_t width;        // Texture width in texels
    uint32_t height;       // Texture height in texels
    bool     hasPalette;   // True if format is CI4 or CI8 (needs TLUT)
};

// Lightweight texture reference for use in material tables.
// Does not include DX12 resource handles (those are in TextureManager's RTXTextureInfo).
struct TextureRef {
    uint64_t hash;          // FNV-1a hash of source texel data (cache key)
    uint32_t srvIndex;      // Index into the SRV descriptor heap
    uint32_t width;
    uint32_t height;
    bool     valid;         // True if texture was successfully loaded
};

// ============================================================================
// N64TextureFormat — Phase 6 enumeration of N64 texture formats.
// Combines G_IM_FMT and G_IM_SIZ into a single enum for cleaner API usage.
// Used by TextureManager::GetOrCreateTexture and related methods.
// ============================================================================
enum class N64TextureFormat : uint32_t {
    RGBA16 = 0,   // G_IM_FMT_RGBA + G_IM_SIZ_16b (5-5-5-1, 2 bytes/pixel)
    RGBA32 = 1,   // G_IM_FMT_RGBA + G_IM_SIZ_32b (8-8-8-8, 4 bytes/pixel)
    CI4    = 2,   // G_IM_FMT_CI   + G_IM_SIZ_4b  (4-bit palette index, 0.5 bytes/pixel)
    CI8    = 3,   // G_IM_FMT_CI   + G_IM_SIZ_8b  (8-bit palette index, 1 byte/pixel)
    IA4    = 4,   // G_IM_FMT_IA   + G_IM_SIZ_4b  (3-bit I + 1-bit A, 0.5 bytes/pixel)
    IA8    = 5,   // G_IM_FMT_IA   + G_IM_SIZ_8b  (4-bit I + 4-bit A, 1 byte/pixel)
    IA16   = 6,   // G_IM_FMT_IA   + G_IM_SIZ_16b (8-bit I + 8-bit A, 2 bytes/pixel)
    I4     = 7,   // G_IM_FMT_I    + G_IM_SIZ_4b  (4-bit intensity, 0.5 bytes/pixel)
    I8     = 8,   // G_IM_FMT_I    + G_IM_SIZ_8b  (8-bit intensity, 1 byte/pixel)
    COUNT  = 9
};

// ============================================================================
// RTXMaterial — Phase 6 material structure for ray tracing.
// Holds SRV GPU descriptor handles for diffuse texture and normal map,
// plus PBR material parameters. Used by the DXR hit shader to sample textures.
// The diffuseTextureSRV and normalMapSRV fields are D3D12_GPU_DESCRIPTOR_HANDLE
// values returned by TextureManager::GetOrCreateTexture.
// ============================================================================
struct RTXMaterial {
    // GPU descriptor handles for shader resource views (texture sampling)
    uint64_t diffuseTextureSRV;   // D3D12_GPU_DESCRIPTOR_HANDLE.ptr for diffuse/albedo texture
    uint64_t normalMapSRV;        // D3D12_GPU_DESCRIPTOR_HANDLE.ptr for normal map texture

    // Bindless texture indices (for indexing into SRV descriptor heap arrays)
    uint32_t diffuseTextureIndex; // SRV heap index for diffuse texture
    uint32_t normalMapIndex;      // SRV heap index for normal map

    // PBR material properties
    float baseColor[4];           // Base color multiplier (RGBA)
    float roughness;              // Surface roughness [0, 1]
    float metallic;               // Metalness [0, 1]
    float emissiveStrength;       // Emissive intensity multiplier [0, ...]

    // N64 material classification
    uint32_t combinerMode;        // Simplified combiner mode (CombinerMode enum)
    uint32_t isAlphaTested;       // 1 if alpha test enabled
    uint32_t isWater;             // 1 if water surface
    uint32_t isEmissive;          // 1 if surface emits light

    RTXMaterial()
        : diffuseTextureSRV(0)
        , normalMapSRV(0)
        , diffuseTextureIndex(0)
        , normalMapIndex(0)
        , baseColor{1.0f, 1.0f, 1.0f, 1.0f}
        , roughness(0.5f)
        , metallic(0.0f)
        , emissiveStrength(0.0f)
        , combinerMode(0)
        , isAlphaTested(0)
        , isWater(0)
        , isEmissive(0)
    {}
};

} // namespace RTX

// Provide types in the SOH::RTX namespace as well for Phase 6 task-spec compatibility.
namespace SOH {
namespace RTX {
    using N64TextureFormat = ::RTX::N64TextureFormat;
    using RTXMaterial = ::RTX::RTXMaterial;
} // namespace RTX
} // namespace SOH

#endif // ENABLE_DX12_RTX
#endif // RTX_TYPES_H
