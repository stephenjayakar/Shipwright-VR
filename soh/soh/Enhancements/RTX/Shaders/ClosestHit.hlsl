// ============================================================================
// ClosestHit.hlsl - Closest Hit Shader
// Evaluates material at hit point: texture sampling, N64 combiner,
// and direct lighting with shadow rays.
// ============================================================================

#define MAX_TRACE_RECURSION_DEPTH 4
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
    bool isPrimaryRay = (payload.recursionDepth == 0);
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

    bool isWaterSurface = (mat.isWater != 0);
    if (isWaterSurface) {
        float t = (float)frameCount * 0.028;
        uv.x += sin(worldPos.x * 0.022 + t) * 0.030;
        uv.y += cos(worldPos.z * 0.021 - t * 0.9) * 0.028;
    }

    // --- Apply N64 texture wrap/clamp/mirror modes ---
    uv = ApplyWrapModes(uv, mat.wrapModeS, mat.wrapModeT,
                        (float)mat.texWidthPx, (float)mat.texHeightPx);

    // --- Sample texture ---
    uint texIdx = mat.textureIndex;
    float4 texColor = float4(1, 1, 1, 1);
    bool hasRealTexture = (texIdx >= 3);

    if (texIdx > 0) {
        texColor = g_textures[NonUniformResourceIndex(texIdx)].SampleLevel(g_samplerBilinear, uv, 0);
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
                float3 shaded = texColor.rgb * vtxColor.rgb;
                albedo = shaded;
            } else if (needsFallbackColor) {
                albedo = float3(0.5, 0.5, 0.5);
            } else {
                albedo = texColor.rgb * vtxColor.rgb;
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
                if (mat.isAlphaTested != 0) {
                    // Path/foliage mask materials in Kokiri use TEX_ENV_BLEND with alpha-tested coverage.
                    // Blending toward vertex shade here can wash into a white/chalk strip.
                    albedo = texColor.rgb * vtxColor.rgb;
                } else {
                    albedo = lerp(texColor.rgb * vtxColor.rgb, vtxColor.rgb, texColor.a);
                }
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

    bool pathStyleMaterial = ((mat.isDecal != 0) || (mat.combinerMode == COMBINER_DECAL));
    bool pathLikeForClamp = pathStyleMaterial;

    if (!isWaterSurface) {
        float3 prePath = saturate(albedo);
        float maxCh = max(prePath.r, max(prePath.g, prePath.b));
        float minCh = min(prePath.r, min(prePath.g, prePath.b));
        float sat = maxCh - minCh;
        float greenBias = prePath.g - max(prePath.r, prePath.b);
        bool looksVegetation = (greenBias > 0.04) || (prePath.g > prePath.r * 1.10 && prePath.g > prePath.b * 1.06);
        bool lowSatTerrain = (sat < 0.22);
        bool isPathLike = pathStyleMaterial && lowSatTerrain && !looksVegetation;
        pathLikeForClamp = isPathLike;

        if (hasRealTexture) {
            // Keep alpha-tested masks texture-driven; avoid vertex-color whitening.
            float coverage = saturate(texColor.a);
            if (mat.isAlphaTested != 0) {
                albedo *= lerp(0.68, 1.0, coverage);
            }
        }

        // Targeted dirt-path remap only for decal-like materials.
        if (isPathLike) {
            float mask = hasRealTexture
                ? saturate(dot(texColor.rgb, float3(0.299, 0.587, 0.114)))
                : 0.55;
            float3 dirtDark = float3(0.24, 0.19, 0.13);
            float3 dirtLight = float3(0.56, 0.47, 0.34);
            float3 dirtTone = lerp(dirtDark, dirtLight, mask);
            // Stronger blend for alpha-tested path overlays so the bright/chalk strip
            // reads as a grounded dirt path close to the native look.
            float blend = (mat.isAlphaTested != 0) ? 0.52 : 0.24;
            albedo = lerp(albedo, dirtTone, blend);
        }

        // Mild fallback for truly untextured surfaces.
        if (!hasRealTexture) {
            float primLuma = ((mat._materialPad >> 16) & 0xFF) / 255.0;
            float envLuma = ((mat._materialPad >> 24) & 0xFF) / 255.0;
            float tint = max(primLuma, envLuma);
            float3 fallbackTerrain = lerp(float3(0.26, 0.21, 0.15), float3(0.48, 0.42, 0.34), saturate(tint));
            albedo = lerp(albedo, fallbackTerrain, 0.45);
        }
    }

    albedo = saturate(albedo);
    float albedoLuma = dot(albedo, float3(0.299, 0.587, 0.114));
    if (!isWaterSurface) {
        float maxLuma = pathLikeForClamp ? 0.62 : 0.82;
        if (albedoLuma > maxLuma) {
            albedo *= (maxLuma / max(albedoLuma, 0.001));
        }
    }

    // =========================================================================
    // Debug visualization modes
    // =========================================================================
    if (debugMode == 1) {
        // Albedo only: no lighting, GI, or specular.
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
    if (isWaterSurface) {
        N = WaterRippleNormal(worldPos, N, (float)frameCount);
    }
    float NdotL = max(dot(N, sunDir), 0.0);

    // =========================================================================
    // Shadow ray: trace toward the sun to check for occlusion
    // =========================================================================
    float shadowFactor = 1.0; // 1 = fully lit, 0 = fully shadowed
    if (NdotL > 0.001 && isPrimaryRay) {
        RayDesc shadowRay;
        float3 biasScale = max(abs(worldPos.x), max(abs(worldPos.y), abs(worldPos.z))) * 0.001;
        shadowRay.Origin = worldPos + N * (0.01 + length(biasScale));
        shadowRay.Direction = sunDir;
        shadowRay.TMin = 0.01;
        shadowRay.TMax = 12000.0;

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
        ambient = float3(0.10, 0.14, 0.28) * ambInt;
    }

    // Simple hemisphere ambient: slightly brighter above than below
    float hemiBlend = N.y * 0.5 + 0.5; // 0 = downward facing, 1 = upward facing
    float3 groundAmbient = ambient * 0.6;
    float3 skyAmbient = ambient * 1.4;
    float3 hemiAmbient = lerp(groundAmbient, skyAmbient, hemiBlend);

    // Surface response: vary roughness by material class to avoid flat "all same" look.
    float roughness = 0.88;
    if (mat.isDecal != 0) roughness = 0.95;
    else if (isWaterSurface) roughness = 0.06;
    else if (mat.combinerMode == COMBINER_DECAL) roughness = 0.9;
    else if (mat.combinerMode == COMBINER_MODULATE_RGB || mat.combinerMode == COMBINER_MODULATE_RGBA) roughness = 0.6;
    roughness = saturate(roughness);

    float3 viewDir = SafeNormalize(-WorldRayDirection(), float3(0, 0, 1));
    float3 halfVec = SafeNormalize(viewDir + sunDir, sunDir);
    float specPower = lerp(96.0, 8.0, roughness);
    float spec = pow(saturate(dot(N, halfVec)), specPower);
    float nonWaterSpec = 0.0;
    float3 specular = isPrimaryRay
        ? (sunCol * sunInt * spec * (isWaterSurface ? 0.7 : nonWaterSpec) * shadowFactor)
        : float3(0.0, 0.0, 0.0);

    // GI bounce: recursive diffuse bounces up to scene-configured budget.
    float3 giContribution = float3(0.0, 0.0, 0.0);
    if (giIntensity > 0.001 && payload.recursionDepth < giMaxBounces) {
        // Deterministic GI direction avoids temporal flashing from stochastic sampling.
        float3 giDir = SafeNormalize(N + sunDir * 0.35 + float3(0.21, 0.31, -0.13), N);

        RayDesc giRay;
        giRay.Origin = worldPos + N * 0.6;
        giRay.Direction = giDir;
        giRay.TMin = 0.1;
        giRay.TMax = 12000.0;

        RayPayload giPayload;
        giPayload.color = float3(0, 0, 0);
        giPayload.distance = 0;
        giPayload.worldNormal = float3(0, 0, 0);
        giPayload.hit = 0;
        giPayload.recursionDepth = payload.recursionDepth + 1;

        TraceRay(g_scene, RAY_FLAG_NONE, 0xFF, 0, 1, 0, giRay, giPayload);

        // Decay energy per bounce to keep multi-bounce GI stable.
        float bounceDecay = 1.0 / (1.0 + (float)payload.recursionDepth * 0.85);
        float bounceEnergy = (giPayload.hit != 0) ? (0.22 * bounceDecay) : (0.05 * bounceDecay);
        giContribution = SanitizeColor(giPayload.color, MAX_HDR_VALUE) * albedo * (giIntensity * bounceEnergy);
    }

    float3 finalColor = albedo * (directLight + hemiAmbient) + giContribution + specular;
    // Keep non-water surfaces matte and prevent washed-out white terrain/path output.
    if (!isWaterSurface) {
        float3 energyCap = albedo * 1.35 + float3(0.12, 0.12, 0.12);
        finalColor = min(finalColor, energyCap);
        // Decal/path materials: strict white guard only where needed.
        if (pathLikeForClamp) {
            float luma = dot(finalColor, float3(0.299, 0.587, 0.114));
            if (luma > 0.72) {
                finalColor *= (0.72 / max(luma, 0.001));
            }
        }
    }

    // Water reflections + refractions
    if (isWaterSurface && isPrimaryRay) {
        float cosTheta = saturate(dot(N, viewDir));
        float fresnel = SchlickFresnel(0.02, cosTheta);

        float3 skyFallback = lerp(float3(0.45, 0.55, 0.72), float3(0.14, 0.30, 0.72), saturate(N.y * 0.5 + 0.5));

        // Trace a deterministic reflection ray for real scene reflections.
        float3 reflDir = reflect(-viewDir, N);
        RayPayload reflPayload;
        reflPayload.color = float3(0, 0, 0);
        reflPayload.distance = 0;
        reflPayload.worldNormal = float3(0, 0, 0);
        reflPayload.hit = 0;
        reflPayload.recursionDepth = payload.recursionDepth + 1;

        RayDesc reflRay;
        reflRay.Origin = worldPos + N * 0.15;
        reflRay.Direction = SafeNormalize(reflDir, N);
        reflRay.TMin = 0.05;
        reflRay.TMax = 12000.0;

        TraceRay(g_scene, RAY_FLAG_NONE, 0xFF, 0, 1, 0, reflRay, reflPayload);

        float3 reflColor = (reflPayload.hit != 0) ? reflPayload.color : skyFallback;
        float sunGlint = pow(saturate(dot(reflect(-sunDir, N), viewDir)), 48.0) * 0.8;
        reflColor += sunColor.rgb * sunGlint;

        // Refraction ray: trace through water surface to avoid solid look.
        float eta = 1.0 / 1.33;
        float3 refrDir = refract(-viewDir, N, eta);
        if (length(refrDir) < 0.001) {
            refrDir = -viewDir;
        }
        refrDir = SafeNormalize(refrDir, -viewDir);

        RayPayload refrPayload;
        refrPayload.color = float3(0, 0, 0);
        refrPayload.distance = 0;
        refrPayload.worldNormal = float3(0, 0, 0);
        refrPayload.hit = 0;
        refrPayload.recursionDepth = payload.recursionDepth + 1;

        RayDesc refrRay;
        refrRay.Origin = worldPos - N * 0.10;
        refrRay.Direction = refrDir;
        refrRay.TMin = 0.05;
        refrRay.TMax = 12000.0;

        TraceRay(g_scene, RAY_FLAG_NONE, 0xFF, 0, 1, 0, refrRay, refrPayload);

        float3 refrBase = (albedo * (hemiAmbient + ambient * 0.7 + float3(0.06, 0.12, 0.16)));
        float3 refrColor = (refrPayload.hit != 0) ? refrPayload.color : refrBase;
        refrColor *= float3(0.72, 0.88, 1.0);

        // More transparent/transmissive water while keeping reflective highlights.
        float reflAmount = saturate(fresnel * (0.85 + 0.55 * reflectionIntensity));
        float refrAmount = saturate(1.0 - reflAmount);
        float transmission = saturate(0.72 + 0.18 * refrAmount);
        float3 waterLit = finalColor * float3(0.74, 0.90, 1.04);
        finalColor = waterLit * 0.10 + reflColor * (reflAmount * 1.06) + refrColor * (refrAmount * transmission);

        // Add explicit animated wave luminance so movement is visible even on calm camera shots.
        float waveA = sin(worldPos.x * 0.05 + (float)frameCount * 0.070);
        float waveB = cos(worldPos.z * 0.045 - (float)frameCount * 0.062);
        float waveMix = saturate(0.5 + 0.25 * (waveA + waveB));
        finalColor *= lerp(0.78, 1.18, waveMix);
        finalColor += float3(0.02, 0.06, 0.08) * waveMix;
    }

    // Clamp to valid HDR range
    finalColor = SanitizeColor(finalColor, MAX_HDR_VALUE);

    payload.color = finalColor;
    payload.distance = RayTCurrent();
    payload.worldNormal = SafeNormalize(N, float3(0, 1, 0));
    payload.hit = 1;
}
