// ============================================================================
// ClosestHit.hlsl - Closest Hit Shader
// Evaluates material at hit point: texture sampling, N64 combiner,
// and direct lighting with shadow rays.
// ============================================================================

#define MAX_TRACE_RECURSION_DEPTH 2
#define MAX_HDR_VALUE 10.0

#include "Common.hlsli"

// Safe normalize helper
float3 SafeNormalize(float3 v, float3 fallback) {
    float len = length(v);
    return (len > 0.001 && !isnan(len) && !isinf(len)) ? (v / len) : fallback;
}

// NaN/Inf clamp helper
float3 SanitizeColor(float3 c, float maxVal) {
    c.r = (isnan(c.r) || isinf(c.r)) ? 0.0 : c.r;
    c.g = (isnan(c.g) || isinf(c.g)) ? 0.0 : c.g;
    c.b = (isnan(c.b) || isinf(c.b)) ? 0.0 : c.b;
    return clamp(c, 0.0, maxVal);
}

// Global resources
RaytracingAccelerationStructure g_scene     : register(t0, space0);

// Per-geometry resources (local root signature, space1)
StructuredBuffer<RTXVertex>  g_vertices    : register(t0, space1);
StructuredBuffer<uint>       g_indices     : register(t1, space1);
StructuredBuffer<uint>       g_materialIDs : register(t2, space1);
StructuredBuffer<Material>   g_materials   : register(t3, space1);

// Bindless texture array (global root signature, space0)
Texture2D    g_textures[]       : register(t4, space0);
SamplerState g_samplerBilinear  : register(s0);  // Bilinear (reserved for water / smooth surfaces)
SamplerState g_samplerPoint     : register(s1);  // Point/nearest — OoT faithful pixel-art look

[shader("closesthit")]
void ClosestHit(inout RayPayload payload, in BuiltInTriangleIntersectionAttributes attribs) {
    // --- Fetch triangle vertices ---
    uint primitiveIndex = PrimitiveIndex();
    uint i0 = g_indices[primitiveIndex * 3 + 0];
    uint i1 = g_indices[primitiveIndex * 3 + 1];
    uint i2 = g_indices[primitiveIndex * 3 + 2];

    RTXVertex v0 = g_vertices[i0];
    RTXVertex v1 = g_vertices[i1];
    RTXVertex v2 = g_vertices[i2];

    // --- Interpolate attributes using barycentrics ---
    float3 bary = float3(
        1.0 - attribs.barycentrics.x - attribs.barycentrics.y,
        attribs.barycentrics.x,
        attribs.barycentrics.y
    );

    float3 worldPos = v0.position * bary.x + v1.position * bary.y + v2.position * bary.z;
    float3 rawNormal = v0.normal * bary.x + v1.normal * bary.y + v2.normal * bary.z;
    float3 normal = SafeNormalize(rawNormal, float3(0, 1, 0));
    float2 uv       = v0.uv * bary.x + v1.uv * bary.y + v2.uv * bary.z;
    float4 vtxColor = v0.color * bary.x + v1.color * bary.y + v2.color * bary.z;

    // Guard against NaN/Inf in interpolated world position
    if (any(isnan(worldPos)) || any(isinf(worldPos))) {
        worldPos = WorldRayOrigin() + WorldRayDirection() * RayTCurrent();
    }

    // Guard against NaN/Inf in vertex color
    vtxColor.r = (isnan(vtxColor.r) || isinf(vtxColor.r)) ? 1.0 : saturate(vtxColor.r);
    vtxColor.g = (isnan(vtxColor.g) || isinf(vtxColor.g)) ? 1.0 : saturate(vtxColor.g);
    vtxColor.b = (isnan(vtxColor.b) || isinf(vtxColor.b)) ? 1.0 : saturate(vtxColor.b);
    vtxColor.a = (isnan(vtxColor.a) || isinf(vtxColor.a)) ? 1.0 : saturate(vtxColor.a);
    vtxColor.a = max(vtxColor.a, 0.004);

    // --- Get material ---
    uint matID = g_materialIDs[primitiveIndex];
    Material mat = g_materials[matID];

    // --- Ensure UVs are valid ---
    uv.x = (isnan(uv.x) || isinf(uv.x)) ? 0.0 : uv.x;
    uv.y = (isnan(uv.y) || isinf(uv.y)) ? 0.0 : uv.y;

    // --- Apply N64 texture wrap/clamp/mirror modes ---
    uv = ApplyWrapModes(uv, mat.wrapModeS, mat.wrapModeT,
                        (float)mat.texWidthPx, (float)mat.texHeightPx);

    // --- Sample texture ---
    uint texIdx = mat.textureIndex;
    float4 texColor = float4(1, 1, 1, 1);
    bool hasRealTexture = (texIdx >= 3);

    if (texIdx > 0) {
        // Bilinear filtering on native N64-resolution textures = correct OoT look at RTX resolution.
        // (The previous blurriness was caused by bilinear being applied on top of a 4x
        // nearest-neighbor upscale — that upscale is now disabled, so bilinear is correct.)
        texColor = g_textures[NonUniformResourceIndex(texIdx)]
                        .SampleLevel(g_samplerBilinear, uv, 0);
        if (IsCorruptedTextureSample(texColor)) {
            texColor = MissingTextureFallback(worldPos);
            hasRealTexture = false;
        }
    }

    // --- Apply simplified N64 color combiner ---
    float3 albedo;
    bool hasAnyValidTexture = hasRealTexture || (texIdx > 0 && texIdx < 3);
    bool needsFallbackColor = !hasAnyValidTexture;

    switch (mat.combinerMode) {
        case COMBINER_MODULATE_RGB:
        case COMBINER_MODULATE_RGBA:
            if (hasRealTexture) {
                albedo = texColor.rgb * vtxColor.rgb;
            } else if (needsFallbackColor) {
                albedo = float3(0.5, 0.5, 0.5);
            } else {
                albedo = texColor.rgb * vtxColor.rgb;
            }
            break;
        case COMBINER_DECAL:
            if (hasRealTexture) {
                albedo = texColor.rgb;
            } else if (needsFallbackColor) {
                albedo = float3(0.5, 0.5, 0.5);
            } else {
                albedo = texColor.rgb;
            }
            break;
        case COMBINER_SHADE:
            {
                float vtxLum = dot(vtxColor.rgb, float3(0.299, 0.587, 0.114));
                albedo = lerp(float3(vtxLum, vtxLum, vtxLum), vtxColor.rgb, 0.5);
            }
            break;
        case COMBINER_TEX_ENV_BLEND:
            if (hasRealTexture) {
                albedo = lerp(texColor.rgb, float3(0.5, 0.5, 0.5), texColor.a);
            } else {
                albedo = float3(0.5, 0.5, 0.5);
            }
            break;
        default:
            if (hasRealTexture) {
                albedo = texColor.rgb * vtxColor.rgb;
            } else if (needsFallbackColor) {
                albedo = float3(0.5, 0.5, 0.5);
            } else {
                albedo = texColor.rgb * vtxColor.rgb;
            }
            break;
    }

    // Clamp albedo to valid range
    albedo = saturate(albedo);

    // =========================================================================
    // Debug visualization modes
    // =========================================================================
    if (debugMode == 1) {
        // Albedo only — no lighting
        payload.color = albedo;
        payload.distance = RayTCurrent();
        payload.worldNormal = SafeNormalize(normal, float3(0, 1, 0));
        payload.hit = 1;
        return;
    }
    if (debugMode == 2) {
        // Normal visualization
        payload.color = normal * 0.5 + 0.5;
        payload.distance = RayTCurrent();
        payload.worldNormal = SafeNormalize(normal, float3(0, 1, 0));
        payload.hit = 1;
        return;
    }

    // =========================================================================
    // DIRECT LIGHTING with shadow rays
    // =========================================================================
    float3 sunDir = normalize(sunDirection.xyz);
    float3 sunCol = sunColor.rgb;
    float sunInt = sunIntensity;
    float ambInt = ambientIntensity;

    // Fallback if constants are zero (shouldn't happen, but safety)
    if (length(sunDirection.xyz) < 0.01) {
        sunDir = normalize(float3(0.5, 0.8, 0.3));
    }
    if (dot(sunCol, float3(1, 1, 1)) < 0.01) {
        sunCol = float3(1.0, 1.0, 1.0);  // Pure white fallback — no warm bias
    }
    if (sunInt < 0.01) sunInt = 2.0;
    if (ambInt < 0.01) ambInt = 0.35;

    float3 N = normalize(normal);
    float NdotL = max(dot(N, sunDir), 0.0);

    // =========================================================================
    // Shadow ray: trace toward the sun to check for occlusion
    // =========================================================================
    float shadowFactor = 1.0; // 1 = fully lit, 0 = fully shadowed
    if (NdotL > 0.001 && payload.recursionDepth < MAX_TRACE_RECURSION_DEPTH) {
        RayDesc shadowRay;
        float3 biasScale = max(abs(worldPos.x), max(abs(worldPos.y), abs(worldPos.z))) * 0.001;
        shadowRay.Origin = worldPos + N * (0.01 + length(biasScale));
        shadowRay.Direction = sunDir;
        shadowRay.TMin = 0.01;
        shadowRay.TMax = 200000.0;

        // Shadow ray payload: initialize hit=1 (assume occluded).
        // With RAY_FLAG_SKIP_CLOSEST_HIT_SHADER, if geometry is hit, ClosestHit
        // doesn't run and hit stays at 1 (shadow). If the ray misses, the Miss
        // shader runs and sets hit=0 (no shadow).
        RayPayload shadowPayload;
        shadowPayload.color = float3(0, 0, 0);
        shadowPayload.distance = 0;
        shadowPayload.worldNormal = float3(0, 0, 0);
        shadowPayload.hit = 1;  // Assume occluded; Miss shader will clear this
        shadowPayload.recursionDepth = payload.recursionDepth + 1;

        TraceRay(g_scene,
                 RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER,
                 0xFF, 0, 1, 0, shadowRay, shadowPayload);

        if (shadowPayload.hit != 0) {
            // In shadow — use a soft shadow factor (not pure black)
            shadowFactor = 0.1;
        }
    }

    // Direct sunlight contribution (with shadow)
    float3 directLight = sunCol * sunInt * NdotL * shadowFactor;

    // Ambient lighting (always present, gives depth to shadows)
    float3 ambient = ambientColor.rgb * ambInt;
    if (dot(ambientColor.rgb, float3(1, 1, 1)) < 0.01) {
        // Cool blue ambient fallback — counteract warm/green N64 textures
        ambient = float3(0.12, 0.15, 0.30) * ambInt;
    }

    // Simple hemisphere ambient: slightly brighter above than below
    float hemiBlend = N.y * 0.5 + 0.5; // 0 = downward facing, 1 = upward facing
    float3 groundAmbient = ambient * 0.6;
    float3 skyAmbient = ambient * 1.4;
    float3 hemiAmbient = lerp(groundAmbient, skyAmbient, hemiBlend);

    // Combine: albedo * (direct + ambient)
    float3 finalColor = albedo * (directLight + hemiAmbient);

    // Clamp to valid HDR range
    finalColor = SanitizeColor(finalColor, MAX_HDR_VALUE);

    payload.color = finalColor;
    payload.distance = RayTCurrent();
    payload.worldNormal = SafeNormalize(normal, float3(0, 1, 0));
    payload.hit = 1;
}
