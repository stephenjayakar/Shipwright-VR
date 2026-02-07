// ============================================================================
// Accumulate.hlsl - Temporal Accumulation Compute Shader
// Blends the current frame's ray-traced output with the history buffer
// using an exponential moving average. Resets on camera movement.
// ============================================================================

RWTexture2D<float4> g_current : register(u0);  // Current frame output
RWTexture2D<float4> g_history : register(u1);  // Accumulated history

cbuffer AccumulateConstants : register(b0) {
    uint2 resolution;    // Output resolution
    uint  frameCount;    // Accumulation frame count (0 = reset)
    uint  _pad;
};

[numthreads(8, 8, 1)]
void Accumulate(uint3 DTid : SV_DispatchThreadID) {
    // Bounds check
    if (DTid.x >= resolution.x || DTid.y >= resolution.y)
        return;

    float4 currentColor = g_current[DTid.xy];
    float4 historyColor = g_history[DTid.xy];

    float4 accumulated;
    if (frameCount == 0) {
        // Reset accumulation on camera movement or scene load
        accumulated = currentColor;
    } else {
        // Exponential moving average blending
        // Weight decreases as more frames accumulate, converging to a clean image.
        // Capped at 128 frames to maintain responsiveness.
        float alpha = 1.0 / min((float)frameCount + 1.0, 128.0);
        accumulated = lerp(historyColor, currentColor, alpha);
    }

    // Write back to both buffers
    g_history[DTid.xy] = accumulated;
    g_current[DTid.xy] = accumulated;
}
