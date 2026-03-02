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

    // Sky colors (in linear space, before gamma correction)
    float3 zenithColor  = float3(0.15, 0.35, 0.85);   // Deep blue at top
    float3 horizonColor = float3(0.6, 0.7, 0.85);      // Pale blue-white at horizon
    float3 groundColor  = float3(0.15, 0.12, 0.1);     // Dark brown below horizon

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
        float sunGlow = pow(sunDot, 32.0) * 0.5; // Concentrated glow
        float sunHalo = pow(sunDot, 4.0) * 0.15;  // Wide halo
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
