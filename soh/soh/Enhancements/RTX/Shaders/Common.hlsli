#ifndef COMMON_HLSLI
#define COMMON_HLSLI

// ============================================================================
// RTX Common Shader Include
// Shared structures, constants, and utility functions for DXR shaders.
// Must match the C++ structures in RTXTypes.h EXACTLY byte-for-byte.
// ============================================================================

// Mathematical constants
#define RTX_PI 3.14159265358979323846
#define RTX_INV_PI 0.31830988618379067

// ============================================================================
// Scene Constants - CANONICAL LAYOUT (384 bytes total)
// This MUST match RTXTypes.h SceneConstants byte-for-byte.
//
// BYTE OFFSET MAP:
//   offset   0: viewMatrix    (float4x4, 64 bytes)
//   offset  64: projMatrix    (float4x4, 64 bytes)
//   offset 128: invViewMatrix (float4x4, 64 bytes)
//   offset 192: invProjMatrix (float4x4, 64 bytes)
//   offset 256: sunDirection  (float4, 16 bytes)
//   offset 272: sunColor      (float4, 16 bytes)
//   offset 288: ambientColor  (float4, 16 bytes)
//   offset 304: fogColor      (float4, 16 bytes)
//   offset 320: fogStart, fogEnd, sunIntensity, ambientIntensity (16 bytes)
//   offset 336: giIntensity, reflectionIntensity, skyIntensity, exposure (16 bytes)
//   offset 352: frameCount, toneMapMode, skyBlendFactor, debugMode (16 bytes)
//   offset 368: giMaxBounces, pad1, pad2, pad3 (16 bytes)
//   Total: 384 bytes
// ============================================================================
cbuffer SceneConstants : register(b0) {
    row_major float4x4 viewMatrix;        // offset 0
    row_major float4x4 projMatrix;        // offset 64
    row_major float4x4 invViewMatrix;     // offset 128
    row_major float4x4 invProjMatrix;     // offset 192
    float4 sunDirection;        // offset 256
    float4 sunColor;            // offset 272
    float4 ambientColor;        // offset 288
    float4 fogColor;            // offset 304
    float fogStart;             // offset 320
    float fogEnd;               // offset 324
    float sunIntensity;         // offset 328
    float ambientIntensity;     // offset 332
    float giIntensity;          // offset 336
    float reflectionIntensity;  // offset 340
    float skyIntensity;         // offset 344
    float exposure;             // offset 348
    uint frameCount;            // offset 352
    uint toneMapMode;           // offset 356
    float skyBlendFactor;       // offset 360
    int debugMode;              // offset 364
    uint giMaxBounces;          // offset 368
    float pad1;                 // offset 372
    float pad2;                 // offset 376
    float pad3;                 // offset 380
};

// --- Ray Payload ---
// Note: DXR payload structs should use fixed-size types (float, uint, int)
// to avoid alignment issues between shader stages. Using 'bool' can cause
// payload mismatch on some drivers. Use uint instead (0 = false, 1 = true).
struct RayPayload {
    float3 color;
    float  distance;
    float3 worldNormal;    // World-space surface normal at hit point (for denoiser)
    uint   hit;            // 0 = miss, 1 = hit (avoid bool for DXR payload alignment)
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
    uint wrapModeS;        // S (horizontal) wrap mode
    uint wrapModeT;        // T (vertical) wrap mode
    uint texWidthPx;       // Texture width in texels (e.g., 32)
    uint texHeightPx;      // Texture height in texels (e.g., 32)
    uint isDecal;          // 1 if decal/overlay geometry (ZMODE_DEC)
    uint _materialPad;     // Padding to maintain alignment with C++ struct
};

// Wrap mode constants
#define WRAP_MODE_WRAP   0
#define WRAP_MODE_MIRROR 1
#define WRAP_MODE_CLAMP  2

// --- Combiner Mode IDs (must match RTX::CombinerMode enum) ---
#define COMBINER_MODULATE_RGB     0   // tex * vtxColor
#define COMBINER_MODULATE_RGBA    1   // tex * vtxColor (with alpha)
#define COMBINER_DECAL            2   // tex only
#define COMBINER_SHADE            3   // vtxColor only
#define COMBINER_TEX_ENV_BLEND    4   // lerp(tex, envColor, texAlpha)

// ============================================================================
// UV Wrapping Utilities
// ============================================================================

float ApplyWrapMode(float uv, uint mode, float texDimension) {
    if (mode == WRAP_MODE_MIRROR) {
        float s = frac(uv * 0.5) * 2.0;
        return (s > 1.0) ? (2.0 - s) : s;
    } else if (mode == WRAP_MODE_CLAMP) {
        float halfTexel = (texDimension > 0.0) ? (0.5 / texDimension) : 0.015;
        return clamp(uv, halfTexel, 1.0 - halfTexel);
    }
    return uv;
}

float ApplyWrapMode(float uv, uint mode) {
    return ApplyWrapMode(uv, mode, 32.0);
}

float2 ApplyWrapModes(float2 uv, uint wrapModeS, uint wrapModeT, float texWidth, float texHeight) {
    uv.x = ApplyWrapMode(uv.x, wrapModeS, texWidth);
    uv.y = ApplyWrapMode(uv.y, wrapModeT, texHeight);
    return uv;
}

float2 ApplyWrapModes(float2 uv, uint wrapModeS, uint wrapModeT) {
    return ApplyWrapModes(uv, wrapModeS, wrapModeT, 32.0, 32.0);
}

// ============================================================================
// Missing Texture Fallback
// ============================================================================

float4 MissingTextureFallback(float3 worldPos, float cellSize) {
    float checker = frac(floor(worldPos.x / cellSize) * 0.5 + floor(worldPos.z / cellSize) * 0.5);
    bool isMagenta = (checker < 0.25);
    return isMagenta ? float4(1.0, 0.0, 1.0, 1.0) : float4(0.1, 0.1, 0.1, 1.0);
}

float4 MissingTextureFallback(float3 worldPos) {
    return MissingTextureFallback(worldPos, 4.0);
}

bool IsCorruptedTextureSample(float4 texColor) {
    return (texColor.r + texColor.g + texColor.b + texColor.a) == 0.0;
}

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
    float theta = 2.0 * RTX_PI * Random01(seed + 1u);
    return float2(r * cos(theta), r * sin(theta));
}

// ============================================================================
// Helper: Extract camera position from inverse view matrix
// ============================================================================
float3 GetCameraPosition() {
    // Camera position is the translation column of the inverse view matrix.
    // For a row-major inverse view matrix (world-from-view transform):
    //   row 0 = [Rx Ux Lx eye.x]
    //   row 1 = [Ry Uy Ly eye.y]
    //   row 2 = [Rz Uz Lz eye.z]
    //   row 3 = [0  0  0  1    ]
    // The eye position is in column 3 of rows 0-2, i.e., [row][3].
    return float3(invViewMatrix[0][3], invViewMatrix[1][3], invViewMatrix[2][3]);
}

// ============================================================================
// Helper: Extract camera basis vectors from inverse view matrix
// ============================================================================
float3 GetCameraRight() {
    return normalize(float3(invViewMatrix[0][0], invViewMatrix[0][1], invViewMatrix[0][2]));
}

float3 GetCameraUp() {
    return normalize(float3(invViewMatrix[1][0], invViewMatrix[1][1], invViewMatrix[1][2]));
}

float3 GetCameraForward() {
    return normalize(float3(invViewMatrix[2][0], invViewMatrix[2][1], invViewMatrix[2][2]));
}

// ============================================================================
// Water Surface Utilities
// ============================================================================

float SchlickFresnel(float F0, float cosTheta) {
    float safeF0 = saturate(F0);
    float safeCos = saturate(cosTheta);
    float oneMinusCos = 1.0 - safeCos;
    float oneMinusCos2 = oneMinusCos * oneMinusCos;
    return safeF0 + (1.0 - safeF0) * oneMinusCos2 * oneMinusCos2 * oneMinusCos;
}

float3 WaterRippleNormal(float3 worldPos, float3 normal, float time) {
    float2 wave1 = float2(
        sin(worldPos.x * 0.05 + time * 0.02) * 0.03,
        sin(worldPos.z * 0.07 + time * 0.015) * 0.03
    );
    float2 wave2 = float2(
        sin(worldPos.x * 0.12 - time * 0.03 + worldPos.z * 0.08) * 0.015,
        sin(worldPos.z * 0.15 + time * 0.025 - worldPos.x * 0.06) * 0.015
    );
    float2 wave3 = float2(
        sin(worldPos.x * 0.25 + time * 0.045 + worldPos.z * 0.18) * 0.008,
        sin(worldPos.z * 0.28 - time * 0.04 + worldPos.x * 0.14) * 0.008
    );
    float2 ripple = wave1 + wave2 + wave3;

    float3 tangent = abs(normal.y) < 0.999 ?
        normalize(cross(float3(0, 1, 0), normal)) :
        normalize(cross(float3(1, 0, 0), normal));
    float3 bitangent = cross(normal, tangent);

    float3 rawPerturbed = normal + tangent * ripple.x + bitangent * ripple.y;
    float perturbedLen = length(rawPerturbed);
    float3 perturbedNormal = (perturbedLen > 0.001) ? (rawPerturbed / perturbedLen) : normal;
    return perturbedNormal;
}

// ============================================================================
// Hemisphere Sampling
// ============================================================================

float3 SampleCosineHemisphere(float3 normal, float2 rand) {
    float phi = 2.0 * RTX_PI * rand.x;
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

#endif // COMMON_HLSLI
