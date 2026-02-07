#ifndef RTX_TYPES_H
#define RTX_TYPES_H

#ifdef ENABLE_DX12_RTX

#include <cstdint>

namespace RTX {

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
};

// Mesh data extracted from a single N64 display list
struct ExtractedMesh {
    std::vector<RTXVertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<uint32_t> materialIDs; // Per-triangle material ID
    std::vector<Material> materials;
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

} // namespace RTX

#endif // ENABLE_DX12_RTX
#endif // RTX_TYPES_H
