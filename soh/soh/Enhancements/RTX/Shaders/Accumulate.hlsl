// ============================================================================
// Accumulate.hlsl - Temporal Accumulation Compute Shader
// Blends current frame with history for temporal smoothing.
// Uses exponential moving average: output = lerp(history, current, alpha)
// where alpha decreases over time for more accumulation.
// ============================================================================

RWTexture2D<float4> g_current : register(u0);  // Current frame output
RWTexture2D<float4> g_history : register(u1);  // Accumulated history

cbuffer AccumulateConstants : register(b0) {
    uint2 resolution;    // Output resolution
    uint  frameCount;    // Frame counter (0 = first frame, reset on scene change)
    float blendAlpha;    // Base blend factor (e.g. 0.1)
};

[numthreads(8, 8, 1)]
void Accumulate(uint3 DTid : SV_DispatchThreadID) {
    if (DTid.x >= resolution.x || DTid.y >= resolution.y)
        return;

    float4 current = g_current[DTid.xy];
    float4 history = g_history[DTid.xy];

    // NaN/Inf guard on current frame
    if (any(isnan(current.rgb)) || any(isinf(current.rgb))) {
        current = float4(0, 0, 0, 1);
    }

    // First frame or invalid history: use current frame directly
    if (frameCount == 0 || any(isnan(history.rgb)) || any(isinf(history.rgb))) {
        g_history[DTid.xy] = current;
        g_current[DTid.xy] = current;
        return;
    }

    // Adaptive blend factor: start with larger alpha (more current frame weight)
    // and reduce over time for more temporal smoothing.
    // Use a more aggressive convergence ramp and a higher minimum alpha
    // to prevent heavy temporal ghosting. With blendAlpha=0.08 (8%), the
    // steady-state blends 92% history + 8% current — far too much history
    // retention for a 1spp ray tracer, causing visible ghosting on edges.
    // A minimum of 0.15 (15% current) provides good noise reduction while
    // keeping the image responsive.
    float alpha = blendAlpha;
    if (frameCount < 16) {
        // First 16 frames: aggressive convergence ramp.
        // Frame 0 = 1.0, frame 1 = 0.5, frame 2 = 0.33, ..., frame 15 = 0.0625
        // max() with blendAlpha ensures we never go below the configured base.
        alpha = max(alpha, 1.0 / (float)(frameCount + 1));
    }
    // Enforce a higher minimum to prevent excessive ghosting.
    // 0.10 = 10% new frame weight is the absolute minimum for responsiveness.
    alpha = clamp(alpha, 0.10, 1.0);

    // Variance-based rejection: if current frame differs significantly from
    // history, increase alpha to reject stale history faster. This reduces
    // ghosting when geometry edges move or lighting changes.
    float3 diff = abs(current.rgb - history.rgb);
    float diffMag = dot(diff, float3(0.299, 0.587, 0.114)); // Luminance diff
    // If the difference is large (>0.1 in luminance), blend more current frame
    float rejectionBoost = smoothstep(0.05, 0.3, diffMag) * 0.4;
    alpha = min(alpha + rejectionBoost, 1.0);

    // Exponential moving average blend
    float3 blended = lerp(history.rgb, current.rgb, alpha);

    // Write to both buffers
    float4 result = float4(blended, 1.0);
    g_history[DTid.xy] = result;
    g_current[DTid.xy] = result;
}
