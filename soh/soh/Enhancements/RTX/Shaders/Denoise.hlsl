// ============================================================================
// Denoise.hlsl - Edge-Aware A-Trous Wavelet Denoiser (Compute Shader)
// Run 3 passes with increasing step size (1, 2, 4) to smooth GI noise
// while preserving edges.
// ============================================================================

RWTexture2D<float4> g_input  : register(u0);
RWTexture2D<float4> g_output : register(u1);

cbuffer DenoiseConstants : register(b0) {
    int   stepSize;      // 1, 2, 4 for 3 A-trous passes
    float colorSigma;    // Color weight threshold
    float normalSigma;   // Normal weight threshold (reserved for G-buffer)
    float _pad;
};

// 5x5 A-trous kernel weights (1D, separable)
static const float kernel[5] = {
    1.0 / 16.0,
    4.0 / 16.0,
    6.0 / 16.0,
    4.0 / 16.0,
    1.0 / 16.0
};

[numthreads(8, 8, 1)]
void Denoise(uint3 DTid : SV_DispatchThreadID) {
    // Get output dimensions
    uint width, height;
    g_input.GetDimensions(width, height);

    // Bounds check
    if (DTid.x >= width || DTid.y >= height) return;

    float4 centerColor = g_input[DTid.xy];
    float4 sum = float4(0, 0, 0, 0);
    float weightSum = 0.0;

    // 5x5 filter with A-trous step size
    for (int y = -2; y <= 2; y++) {
        for (int x = -2; x <= 2; x++) {
            int2 offset = int2(x, y) * stepSize;
            int2 samplePos = (int2)DTid.xy + offset;

            // Clamp to image bounds
            samplePos = clamp(samplePos, int2(0, 0), int2(width - 1, height - 1));

            float4 sampleColor = g_input[samplePos];

            // Edge-aware weight: reduce weight for large color differences
            // This preserves sharp edges while smoothing noisy flat regions
            float3 colorDiff = centerColor.rgb - sampleColor.rgb;
            float colorDist = dot(colorDiff, colorDiff);
            float colorWeight = exp(-colorDist / (colorSigma * colorSigma + 0.0001));

            // Spatial weight from kernel
            float spatialWeight = kernel[x + 2] * kernel[y + 2];

            float weight = spatialWeight * colorWeight;
            sum += sampleColor * weight;
            weightSum += weight;
        }
    }

    g_output[DTid.xy] = sum / max(weightSum, 0.0001);
}
