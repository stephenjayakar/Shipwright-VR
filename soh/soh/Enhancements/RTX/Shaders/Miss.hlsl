// ============================================================================
// Miss.hlsl - Miss Shader
// Returns a sky gradient when a ray doesn't hit geometry.
// Blue at zenith, warm white at horizon, dark below horizon.
// ============================================================================

#include "Common.hlsli"

[shader("miss")]
void Miss(inout RayPayload payload) {
    float3 rayDir = WorldRayDirection();
    float3 dir = normalize(rayDir);

    // Sky gradient based on ray direction Y component
    float t = dir.y; // -1 (straight down) to +1 (straight up)

    float3 zenithColor  = max(float3(0.14, 0.30, 0.72), fogColor.rgb * float3(0.55, 0.65, 0.95));
    float3 horizonColor = max(float3(0.45, 0.55, 0.72), fogColor.rgb * float3(0.85, 0.90, 1.00));
    float3 groundColor  = float3(0.10, 0.11, 0.10);

    float3 skyColor;
    if (t > 0.0) {
        // Above horizon: blend from horizon to zenith
        float skyT = saturate(t); // 0 at horizon, 1 at zenith
        skyT = sqrt(skyT); // Non-linear: more horizon color near horizon
        skyColor = lerp(horizonColor, zenithColor, skyT);
    } else {
        // Below horizon: blend to dark ground
        float groundT = saturate(-t * 4.0); // Quick falloff below horizon
        skyColor = lerp(horizonColor, groundColor, groundT);
    }

    // Add subtle sun glow near the sun direction
    float3 sunDir = normalize(sunDirection.xyz);
    if (length(sunDirection.xyz) > 0.01) {
        float sunDot = max(dot(dir, sunDir), 0.0);
        float sunGlow = pow(sunDot, 48.0) * 0.35;
        float sunHalo = pow(sunDot, 6.0) * 0.08;
        float3 sunTint = sunColor.rgb;
        if (dot(sunTint, float3(1, 1, 1)) < 0.01) {
            sunTint = float3(1.0, 1.0, 1.0);  // Pure white fallback
        }
        skyColor += sunTint * (sunGlow + sunHalo);
    }

    // Apply sky intensity
    float safeIntensity = max(skyIntensity, 0.5);
    skyColor *= safeIntensity;

    payload.color = skyColor;
    payload.distance = 100000.0;
    payload.worldNormal = float3(0, 0, 0);
    payload.hit = 0;
}
