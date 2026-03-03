// ============================================================================
// RayGen.hlsl - Camera ray generation for DXR raytracing
// Generates primary camera rays using inverse view/projection matrices,
// traces them against the scene TLAS, and writes the resulting color
// (from ClosestHit/Miss shaders) to the output UAV.
// Also outputs G-buffer data (normals, depth) for the denoiser.
// ============================================================================

#include "Common.hlsli"

RaytracingAccelerationStructure g_scene    : register(t0, space0);
RWTexture2D<float4>             g_output   : register(u0);
RWTexture2D<float4>             g_giAccum  : register(u1);
RWTexture2D<float4>             g_normals  : register(u2);  // World normal (xyz) + hit flag (w)
RWTexture2D<float>              g_depth    : register(u3);  // Linear depth (hit distance)

// Halton Low-Discrepancy Sequence for Temporal Jitter
float HaltonSequence(uint index, uint base) {
    float result = 0.0;
    float fraction = 1.0 / (float)base;
    uint i = index;
    while (i > 0) {
        result += fraction * (float)(i % base);
        i /= base;
        fraction /= (float)base;
    }
    return result;
}

[shader("raygeneration")]
void RayGen() {
    uint2 launchIndex = DispatchRaysIndex().xy;
    uint2 launchDim = DispatchRaysDimensions().xy;

    // Extract camera position from inverse view matrix
    float3 origin = GetCameraPosition();

    // =========================================================================
    // Generate camera ray direction from pixel coordinates with temporal jitter
    // =========================================================================

    // Keep primary rays deterministic to avoid visible lighting shimmer/flashing.
    // Temporal stability here is more important than TAA in the current RTX path.
    float2 jitter = float2(0.0, 0.0);

    float2 pixelCenter = (float2)launchIndex + 0.5 + jitter;
    float2 ndc = pixelCenter / (float2)launchDim * 2.0 - 1.0;
    ndc.y = -ndc.y; // Flip Y: screen space top-down -> NDC bottom-up

    // Use inverse projection matrix to convert NDC to view-space direction
    // Then use inverse view matrix to convert to world-space direction
    float4 viewSpaceDir = mul(invProjMatrix, float4(ndc, 1.0, 1.0));
    viewSpaceDir.xyz /= viewSpaceDir.w;
    float3 worldDir = mul((float3x3)invViewMatrix, viewSpaceDir.xyz);
    float3 rayDir = normalize(worldDir);

    // =========================================================================
    // Trace primary ray
    // =========================================================================

    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = rayDir;
    ray.TMin = 0.1;
    ray.TMax = 100000.0;

    RayPayload payload;
    payload.color = float3(0, 0, 0);
    payload.distance = 0;
    payload.worldNormal = float3(0, 0, 0);
    payload.hit = 0;
    payload.recursionDepth = 0;

    TraceRay(g_scene, RAY_FLAG_NONE, 0xFF, 0, 1, 0, ray, payload);

    // =========================================================================
    // Write output
    // =========================================================================

    float3 finalColor = payload.color;

    // Apply distance fog for outdoor scenes
    if (payload.hit != 0 && fogEnd > fogStart && fogEnd > 0) {
        float fogRange = fogEnd - fogStart;
        if (fogRange > 0.001) {
            float fogFactor = saturate((payload.distance - fogStart) / fogRange);
            // Quadratic fog falloff for more natural look
            fogFactor = fogFactor * fogFactor;
            finalColor = lerp(finalColor, fogColor.rgb, fogFactor);
        }
    }

    // Write color output
    g_output[launchIndex] = float4(finalColor, 1.0);
    g_giAccum[launchIndex] = float4(finalColor, 1.0);

    // Write G-buffer data for the denoiser
    g_normals[launchIndex] = float4(payload.worldNormal, (float)payload.hit);
    g_depth[launchIndex] = (payload.hit != 0) ? payload.distance : 0.0;
}
