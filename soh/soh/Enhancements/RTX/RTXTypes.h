#pragma once
#ifndef RTX_TYPES_H
#define RTX_TYPES_H

#ifdef ENABLE_DX12_RTX

#include <cstdint>
#include <vector>

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

// Matches the HLSL Material in Common.hlsli
struct Material {
    uint32_t textureIndex;   // Index into bindless texture SRV array
    uint32_t combinerMode;   // Simplified N64 combiner mode ID
    uint32_t isAlphaTested;  // 1 if alpha test enabled
    uint32_t isWater;        // 1 if water surface (needs UV scroll)
};

// Simplified combiner mode IDs (matches Common.hlsli defines)
enum CombinerMode : uint32_t {
    COMBINER_MODULATE_RGB   = 0, // tex * vtxColor
    COMBINER_MODULATE_RGBA  = 1, // tex * vtxColor (with alpha)
    COMBINER_DECAL          = 2, // tex only
    COMBINER_SHADE          = 3, // vtxColor only
    COMBINER_TEX_ENV_BLEND  = 4, // lerp(tex, envColor, texAlpha)
};

// Matches the HLSL SceneConstants in Common.hlsli
// Must be 256-byte aligned for CBV
struct alignas(256) SceneConstants {
    // --- Core camera and lighting (240 bytes) ---
    float viewInverse[16];     // Camera view matrix inverse (column-major)
    float projInverse[16];     // Camera projection matrix inverse (column-major)
    float cameraPos[3];        // World-space camera position
    uint32_t frameCount;       // For temporal accumulation + RNG seed
    float ambientColor[3];     // From LightContext
    float fogNear;             // From LightContext
    float fogColor[3];         // From LightContext
    float fogFar;              // From LightContext
    float sunDirection1[3];    // Directional light 1 direction
    float time;                // gameplayFrames for UV scroll
    float sunColor1[3];        // Directional light 1 color
    float dekuTreeAlpha;       // Vegetation alpha (segment 0x0A)
    float sunDirection2[3];    // Directional light 2 direction
    float fogBlendAlpha;       // Atmosphere blending (segment 0x0B)
    float sunColor2[3];        // Directional light 2 color
    float waterScrollOffset;   // For segment 0x0C stream

    // --- Per-scene material overrides from RTXSceneConfig (32 bytes) ---
    float giIntensity;         // GI contribution multiplier [0, 2]
    float baseReflectivity;    // Scene-wide base reflectivity for non-metallic surfaces [0, 1]
    float roughnessScale;      // Multiplier for surface roughness [0, 2]
    float emissiveScale;       // Multiplier for emissive surfaces [0, 5]
    float waterReflectivity;   // Override reflectivity for water surfaces [0, 1]
    float waterRoughness;      // Override roughness for water surfaces [0, 1]
    float aoRadius;            // Ambient occlusion sample radius (world units)
    float aoIntensity;         // AO darkening intensity [0, 2]
};

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
    uint32_t geometryMode;     // From G_SETGEOMETRYMODE
    uint32_t otherModeL;       // From G_SETOTHERMODE_L (render mode)
    uint16_t texWidth;         // From G_SETTILE
    uint16_t texHeight;
    uint8_t texFormat;         // RGBA16, CI8, I4, IA8, etc.
    bool alphaTest;            // Derived from render mode
    bool lightingEnabled;      // G_LIGHTING flag

    void Reset() {
        combinerMode = 0;
        textureAddr = 0;
        geometryMode = 0;
        otherModeL = 0;
        texWidth = 32;
        texHeight = 32;
        texFormat = 0;
        alphaTest = false;
        lightingEnabled = false;
    }
};

// Denoise pass constants
struct DenoiseConstants {
    int32_t stepSize;      // 1, 2, 4 for 3 A-trous passes
    float colorSigma;      // Color weight threshold
    float normalSigma;     // Normal weight threshold
    float _pad;
};

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
