// ============================================================================
// RayGen.hlsl - Primary Ray Generation Shader
// Generates camera rays, traces them, applies fog and temporal accumulation.
// ============================================================================

#include "Common.hlsli"

// Global resources
RaytracingAccelerationStructure g_scene    : register(t0, space0);
RWTexture2D<float4>             g_output   : register(u0);
RWTexture2D<float4>             g_giAccum  : register(u1);
ConstantBuffer<SceneConstants>  g_constants : register(b0);

[shader("raygeneration")]
void RayGen() {
    uint2 launchIndex = DispatchRaysIndex().xy;
    uint2 launchDim = DispatchRaysDimensions().xy;

    // Compute normalized device coordinates for this pixel
    float2 pixelCenter = (float2)launchIndex + 0.5;
    float2 ndc = pixelCenter / (float2)launchDim * 2.0 - 1.0;
    ndc.y = -ndc.y; // Flip Y (screen space -> NDC)

    // Generate camera ray using inverse view/projection matrices
    float4 target = mul(g_constants.projInverse, float4(ndc, 1.0, 1.0));
    target /= target.w;
    float4 direction = mul(g_constants.viewInverse, float4(target.xyz, 0.0));

    // Set up ray
    RayDesc ray;
    ray.Origin = g_constants.cameraPos;
    ray.Direction = normalize(direction.xyz);
    ray.TMin = 1.0;       // Near clip (N64 units)
    ray.TMax = 100000.0;  // Far clip

    // Initialize payload
    RayPayload payload;
    payload.color = float3(0, 0, 0);
    payload.distance = 0;
    payload.hit = false;
    payload.recursionDepth = 0;

    // Trace primary ray
    TraceRay(
        g_scene,
        RAY_FLAG_NONE,
        0xFF,           // Instance inclusion mask
        0,              // Hit group index
        0,              // Hit group stride (unused with single hit group)
        0,              // Miss shader index
        ray,
        payload
    );

    // Apply distance fog (linear fog between fogNear and fogFar)
    float fogRange = max(g_constants.fogFar - g_constants.fogNear, 1.0);
    float fogFactor = saturate((payload.distance - g_constants.fogNear) / fogRange);
    float3 finalColor = lerp(payload.color, g_constants.fogColor, fogFactor);

    // Temporal accumulation for GI noise reduction
    // Running average converges over ~64-128 frames when camera is static.
    // On camera movement, frameCount resets to 0 on the C++ side.
    if (g_constants.frameCount > 0) {
        float3 prevColor = g_giAccum[launchIndex].rgb;
        float weight = 1.0 / min((float)g_constants.frameCount + 1.0, 128.0);
        finalColor = lerp(prevColor, finalColor, weight);
    }

    // Write to accumulation buffer (persists across frames)
    g_giAccum[launchIndex] = float4(finalColor, 1.0);

    // Write to output buffer (consumed by denoise + present)
    g_output[launchIndex] = float4(finalColor, 1.0);
}
