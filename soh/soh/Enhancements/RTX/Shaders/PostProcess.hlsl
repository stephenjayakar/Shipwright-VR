// ============================================================================
// PostProcess.hlsl - Final Post-Processing Compute Shader
// Exposure -> tone mapping -> gamma correction.
// ============================================================================

RWTexture2D<float4> g_input  : register(u0);  // Denoised HDR input
RWTexture2D<float4> g_output : register(u1);  // Final LDR output (sRGB)

cbuffer PostProcessConstants : register(b0) {
    uint2  resolution;       // Output resolution
    float  exposure;         // Exposure multiplier (default 1.0)
    uint   toneMapMode;      // 0 = ACES, 1 = Reinhard, 2 = Linear clamp
    float  vignetteStrength; // (unused for now)
    float  saturation;       // (unused for now)
    float  contrast;         // (unused for now)
    int    debugMode;        // 0 = normal, >0 = debug visualization
};

// ACES Filmic Tone Mapping (Narkowicz approximation)
// Neutral — does NOT shift hue. Maps [0,∞) → [0,1]
float3 ACESFilmic(float3 x) {
    x = max(x, float3(0, 0, 0));
    return (x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14);
}

// Simple Reinhard tone mapping
float3 ReinhardToneMap(float3 x) {
    return x / (x + float3(1, 1, 1));
}

[numthreads(8, 8, 1)]
void PostProcess(uint3 DTid : SV_DispatchThreadID) {
    if (DTid.x >= resolution.x || DTid.y >= resolution.y)
        return;

    // Read HDR color from denoised input
    float3 color = g_input[DTid.xy].rgb;

    // NaN/Inf guard
    if (any(isnan(color)) || any(isinf(color))) {
        color = float3(0, 0, 0);
    }
    color = max(color, float3(0, 0, 0));

    // Apply exposure
    float safeExposure = (exposure < 0.05) ? 1.0 : clamp(exposure, 0.1, 5.0);
    color *= safeExposure;

    // Tone mapping
    float3 mapped;
    if (toneMapMode == 1) {
        mapped = ReinhardToneMap(color);
    } else if (toneMapMode == 2) {
        mapped = saturate(color);
    } else {
        mapped = ACESFilmic(color);
    }

    // Gamma correction (linear -> sRGB)
    mapped = saturate(mapped);
    mapped = pow(mapped, 1.0 / 2.2);

    g_output[DTid.xy] = float4(mapped, 1.0);
}
