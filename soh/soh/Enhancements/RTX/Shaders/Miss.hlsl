// ============================================================================
// Miss.hlsl - Miss Shader
// Returns sky/fog color when a ray doesn't hit any geometry.
// ============================================================================

#include "Common.hlsli"

ConstantBuffer<SceneConstants> g_constants : register(b0);

[shader("miss")]
void Miss(inout RayPayload payload) {
    // Simple sky gradient based on ray direction
    float3 rayDir = WorldRayDirection();

    // t=0 at horizon, t=1 straight up
    float t = saturate(rayDir.y * 0.5 + 0.5);

    // Blend between fog color (horizon) and a slightly brighter sky (zenith)
    // This gives a subtle gradient that looks reasonable for OoT's outdoor scenes
    float3 skyColor = lerp(g_constants.fogColor, g_constants.fogColor * 1.3, t);

    payload.color = skyColor;
    payload.distance = 100000.0;
    payload.hit = false;
}
