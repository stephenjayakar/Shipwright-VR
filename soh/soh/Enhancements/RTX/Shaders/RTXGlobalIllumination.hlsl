// ============================================================================
// RTXGlobalIllumination.hlsl - Global Illumination Compute Shader
// Performs screen-space GI accumulation and temporal filtering.
// This shader runs as a post-process after the main DXR ray trace pass,
// combining the raw noisy GI from ClosestHit bounce rays with temporal
// accumulation to produce a clean indirect lighting buffer.
//
// Compiled as lib_6_3 (DXR library) for integration with the RT pipeline,
// or can be compiled as cs_6_0 (compute shader) for standalone dispatch.
// ============================================================================

#include "Common.hlsli"

// Scene constant buffer
ConstantBuffer<SceneConstants> g_constants : register(b0);

// Input: raw RT output with 1-bounce GI noise
RWTexture2D<float4> g_rtOutput        : register(u0);

// Accumulation buffer for temporal blending
RWTexture2D<float4> g_accumulation    : register(u1);

// Output: filtered GI result
RWTexture2D<float4> g_giOutput        : register(u2);

// GI-specific constants (could be part of SceneConstants or a separate CB)
// For now we reuse giIntensity, aoRadius, aoIntensity from SceneConstants.

// ============================================================================
// Temporal accumulation blending factor.
// Lower values = more ghosting but cleaner; higher = more responsive but noisier.
// ============================================================================
static const float TEMPORAL_BLEND_MIN = 0.05;  // Blend factor when fully converged
static const float TEMPORAL_BLEND_MAX = 1.0;   // Blend factor on first frame

float ComputeTemporalBlendFactor(uint frameCount) {
    // Exponential decay: first frames contribute heavily, later frames less
    if (frameCount == 0) return TEMPORAL_BLEND_MAX;
    float t = 1.0 / (float)(frameCount + 1);
    return max(t, TEMPORAL_BLEND_MIN);
}

// ============================================================================
// Luminance helper for edge-stopping
// ============================================================================
float Luminance(float3 color) {
    return dot(color, float3(0.2126, 0.7152, 0.0722));
}

// ============================================================================
// Spatial filtering kernel (3x3 cross bilateral)
// Smooths the GI signal while preserving edges based on luminance difference.
// ============================================================================
float3 SpatialFilter3x3(uint2 pixel, uint2 dims) {
    float3 centerColor = g_rtOutput[pixel].rgb;
    float centerLum = Luminance(centerColor);

    float3 sum = centerColor;
    float weightSum = 1.0;

    // Color-edge sigma for bilateral weight
    float sigma = g_constants.giIntensity > 0.0 ? 0.15 / max(g_constants.giIntensity, 0.1) : 0.15;

    // 3x3 cross pattern (skip diagonals for performance)
    int2 offsets[4] = {
        int2(-1, 0), int2(1, 0),
        int2(0, -1), int2(0, 1)
    };

    for (int i = 0; i < 4; i++) {
        int2 samplePos = int2(pixel) + offsets[i];
        if (samplePos.x < 0 || samplePos.y < 0 ||
            samplePos.x >= (int)dims.x || samplePos.y >= (int)dims.y)
            continue;

        float3 sampleColor = g_rtOutput[uint2(samplePos)].rgb;
        float sampleLum = Luminance(sampleColor);

        // Bilateral weight based on luminance difference
        float lumDiff = abs(centerLum - sampleLum);
        float weight = exp(-lumDiff * lumDiff / (2.0 * sigma * sigma));

        sum += sampleColor * weight;
        weightSum += weight;
    }

    return sum / weightSum;
}

// ============================================================================
// Main GI compute shader entry point
// ============================================================================
[numthreads(8, 8, 1)]
void GlobalIllumination(uint3 DTid : SV_DispatchThreadID) {
    uint2 dims;
    g_rtOutput.GetDimensions(dims.x, dims.y);

    if (DTid.x >= dims.x || DTid.y >= dims.y)
        return;

    uint2 pixel = DTid.xy;

    // Step 1: Spatial pre-filter to reduce high-frequency noise
    float3 filteredColor = SpatialFilter3x3(pixel, dims);

    // Step 2: Temporal accumulation
    float blendFactor = ComputeTemporalBlendFactor(g_constants.frameCount);
    float4 accumulated = g_accumulation[pixel];
    float3 prevColor = accumulated.rgb;
    float prevWeight = accumulated.a;

    // If this is the first frame or camera moved (frameCount reset to 0),
    // reset accumulation.
    float3 blended;
    if (g_constants.frameCount == 0 || prevWeight < 0.001) {
        blended = filteredColor;
    } else {
        blended = lerp(prevColor, filteredColor, blendFactor);
    }

    // Step 3: Apply GI intensity scaling
    blended *= g_constants.giIntensity;

    // Step 4: Apply AO darkening
    // The AO estimate from the trace pass is baked into the raw RT output.
    // Here we apply additional scene-configured AO intensity.
    if (g_constants.aoIntensity > 0.0) {
        float aoFactor = 1.0 - g_constants.aoIntensity * 0.15;
        blended *= aoFactor;
    }

    // Store accumulated result
    g_accumulation[pixel] = float4(blended, 1.0);

    // Write final GI output
    g_giOutput[pixel] = float4(blended, 1.0);
}
