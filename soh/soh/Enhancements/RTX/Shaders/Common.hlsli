#ifndef COMMON_HLSLI
#define COMMON_HLSLI

// ============================================================================
// RTX Common Shader Include
// Shared structures, constants, and utility functions for DXR shaders.
// Must match the C++ structures in RTXTypes.h.
// ============================================================================

// --- Scene Constants (matches RTX::SceneConstants in RTXTypes.h) ---
struct SceneConstants {
    // Core camera and lighting
    float4x4 viewInverse;        // Camera view matrix inverse
    float4x4 projInverse;        // Camera projection matrix inverse
    float3   cameraPos;          // World-space camera position
    uint     frameCount;         // For temporal accumulation + RNG seed
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

    // Per-scene material overrides from RTXSceneConfig
    float    giIntensity;        // GI contribution multiplier [0, 2]
    float    baseReflectivity;   // Scene-wide base reflectivity [0, 1]
    float    roughnessScale;     // Surface roughness multiplier [0, 2]
    float    emissiveScale;      // Emissive surface multiplier [0, 5]
    float    waterReflectivity;  // Water surface reflectivity [0, 1]
    float    waterRoughness;     // Water surface roughness [0, 1]
    float    aoRadius;           // AO sample radius (world units)
    float    aoIntensity;        // AO darkening intensity [0, 2]
};

// --- Ray Payload ---
struct RayPayload {
    float3 color;
    float  distance;
    bool   hit;
    uint   recursionDepth;
};

// --- Vertex (matches RTX::RTXVertex) ---
struct RTXVertex {
    float3 position;
    float3 normal;
    float2 uv;
    float4 color;
};

// --- Material (matches RTX::Material) ---
struct Material {
    uint textureIndex;     // Index into bindless texture array
    uint combinerMode;     // Simplified combiner ID
    uint isAlphaTested;    // 1 if alpha test enabled
    uint isWater;          // 1 if water surface (needs UV scroll)
};

// --- Combiner Mode IDs (must match RTX::CombinerMode enum) ---
#define COMBINER_MODULATE_RGB     0   // tex * vtxColor
#define COMBINER_MODULATE_RGBA    1   // tex * vtxColor (with alpha)
#define COMBINER_DECAL            2   // tex only
#define COMBINER_SHADE            3   // vtxColor only
#define COMBINER_TEX_ENV_BLEND    4   // lerp(tex, envColor, texAlpha)

// ============================================================================
// Random Number Generation (PCG Hash)
// ============================================================================

uint PCGHash(uint input) {
    uint state = input * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float Random01(uint seed) {
    return float(PCGHash(seed)) / 4294967295.0;
}

float2 RandomInUnitDisk(uint2 pixel, uint frame) {
    uint seed = pixel.x + pixel.y * 8192u + frame * 65536u;
    float r = sqrt(Random01(seed));
    float theta = 2.0 * 3.14159265 * Random01(seed + 1u);
    return float2(r * cos(theta), r * sin(theta));
}

// ============================================================================
// Hemisphere Sampling
// ============================================================================

// Cosine-weighted hemisphere sampling around a normal
float3 SampleCosineHemisphere(float3 normal, float2 rand) {
    float phi = 2.0 * 3.14159265 * rand.x;
    float cosTheta = sqrt(1.0 - rand.y);
    float sinTheta = sqrt(rand.y);

    // Build orthonormal basis around normal
    float3 tangent = abs(normal.y) < 0.999 ?
        normalize(cross(float3(0, 1, 0), normal)) :
        normalize(cross(float3(1, 0, 0), normal));
    float3 bitangent = cross(normal, tangent);

    return normalize(tangent * cos(phi) * sinTheta +
                     bitangent * sin(phi) * sinTheta +
                     normal * cosTheta);
}

#endif // COMMON_HLSLI
