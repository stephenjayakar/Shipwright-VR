// ============================================================================
// Denoise.hlsl - Edge-Aware A-Trous Wavelet Denoiser
// Uses geometry-aware bilateral filtering to smooth noisy ray tracing output
// while preserving edges (guided by normal and depth G-buffers).
// ============================================================================

RWTexture2D<float4> g_input  : register(u0);  // Input color (ping-pong)
RWTexture2D<float4> g_output : register(u1);  // Output color (ping-pong)
Texture2D<float4>   g_normals : register(t0); // World-space normals (xyz) + hit flag (w)
Texture2D<float>    g_depth   : register(t1); // Linear depth

cbuffer DenoiseConstants : register(b0) {
    int   stepSize;        // A-trous step size (1, 2, 4, 8, 16)
    float colorSigma;      // Color weight sigma
    float normalSigma;     // Normal weight sigma
    float depthSigma;      // Depth weight sigma
};

// 5x5 A-trous wavelet kernel weights
static const float kernel[5] = { 1.0/16.0, 4.0/16.0, 6.0/16.0, 4.0/16.0, 1.0/16.0 };
static const int offsets[5] = { -2, -1, 0, 1, 2 };

[numthreads(8, 8, 1)]
void Denoise(uint3 DTid : SV_DispatchThreadID) {
    uint width, height;
    g_input.GetDimensions(width, height);

    if (DTid.x >= width || DTid.y >= height) return;

    float4 centerColor = g_input[DTid.xy];
    float4 centerNormal = g_normals[DTid.xy];
    float  centerDepth  = g_depth[DTid.xy];

    // NaN/Inf guard
    if (any(isnan(centerColor.rgb)) || any(isinf(centerColor.rgb))) {
        g_output[DTid.xy] = float4(0, 0, 0, 1);
        return;
    }

    // If this is a miss pixel (no geometry hit), just pass through
    if (centerNormal.w < 0.5) {
        g_output[DTid.xy] = centerColor;
        return;
    }

    float3 sum = float3(0, 0, 0);
    float weightSum = 0.0;

    float safeColorSigma = max(colorSigma, 0.001);
    float safeNormalSigma = max(normalSigma, 0.001);
    float safeDepthSigma = max(depthSigma, 0.0001);

    for (int j = 0; j < 5; j++) {
        for (int i = 0; i < 5; i++) {
            int2 samplePos = int2(DTid.xy) + int2(offsets[i], offsets[j]) * stepSize;

            // Bounds check
            if (samplePos.x < 0 || samplePos.x >= (int)width ||
                samplePos.y < 0 || samplePos.y >= (int)height)
                continue;

            float4 sampleColor = g_input[samplePos];
            float4 sampleNormal = g_normals[samplePos];
            float  sampleDepth  = g_depth[samplePos];

            // Skip NaN samples
            if (any(isnan(sampleColor.rgb)) || any(isinf(sampleColor.rgb)))
                continue;

            // Kernel weight
            float w = kernel[i] * kernel[j];

            // Color-based weight
            float3 colorDiff = centerColor.rgb - sampleColor.rgb;
            float colorDist2 = dot(colorDiff, colorDiff);
            float colorWeight = exp(-colorDist2 / (2.0 * safeColorSigma * safeColorSigma));

            // Normal-based weight
            float normalDot = max(dot(centerNormal.xyz, sampleNormal.xyz), 0.0);
            float normalWeight = pow(normalDot, safeNormalSigma);

            // Depth-based weight
            float depthDiff = abs(centerDepth - sampleDepth);
            float depthWeight = exp(-depthDiff / (safeDepthSigma * max(centerDepth, 1.0)));

            float totalWeight = w * colorWeight * normalWeight * depthWeight;

            sum += sampleColor.rgb * totalWeight;
            weightSum += totalWeight;
        }
    }

    float3 result = (weightSum > 0.0001) ? (sum / weightSum) : centerColor.rgb;
    result = max(result, float3(0, 0, 0));

    g_output[DTid.xy] = float4(result, 1.0);
}
