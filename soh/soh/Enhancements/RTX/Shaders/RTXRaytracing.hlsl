// ============================================================================
// RTXRaytracing.hlsl - Combined DXR Raytracing Library Shader
//
// This file combines all raytracing shader stages into a single DXIL library
// that can be compiled with target lib_6_3. It contains:
//   - RayGen:     Primary ray generation with camera, fog, temporal accumulation
//   - ClosestHit: Material evaluation, direct lighting, shadows, 1-bounce GI
//   - AnyHit:     Alpha testing for foliage and transparent surfaces
//   - Miss:       Sky/fog color for rays that don't hit geometry
//
// Resource bindings (must match DXRPipeline.cpp root signature):
//   Global root signature (space0):
//     b0         - SceneConstants (CBV)
//     t0         - RaytracingAccelerationStructure (TLAS)
//     u0         - RWTexture2D<float4> output buffer
//     u1         - RWTexture2D<float4> GI accumulation buffer
//     t4+        - Texture2D[] bindless texture array
//     s0         - SamplerState (bilinear wrap)
//
//   Local root signature (space1, per hit group):
//     t0, space1 - StructuredBuffer<RTXVertex> vertex buffer
//     t1, space1 - StructuredBuffer<uint> index buffer
//     t2, space1 - StructuredBuffer<uint> material ID buffer
//     t3, space1 - StructuredBuffer<Material> material table
//
// This combined file can be used as an alternative to the individual shader
// files (RayGen.hlsl, ClosestHit.hlsl, Miss.hlsl, AnyHit.hlsl) which are
// compiled separately as individual lib_6_3 libraries by DXRPipeline.
// Both approaches produce equivalent DXIL libraries; the per-file approach
// is preferred for incremental compilation during development.
// ============================================================================

#include "Common.hlsli"

// Maximum recursion depth for TraceRay calls. Primary rays are depth 0.
// Shadow, GI, and reflection rays are depth 1. No rays are traced at depth >= 1.
// This must match the D3D12_RAYTRACING_PIPELINE_CONFIG MaxTraceRecursionDepth on the C++ side.
#define MAX_TRACE_RECURSION_DEPTH 2

// Maximum HDR color value allowed in the output. Values above this are clamped.
#define MAX_HDR_VALUE 10.0

// Safe normalize: returns fallback if the input vector is too short or contains NaN.
float3 SafeNormalize(float3 v, float3 fallback) {
    float len = length(v);
    return (len > 0.001 && !isnan(len) && !isinf(len)) ? (v / len) : fallback;
}

// Clamp a float3 to [0, maxVal], replacing NaN/Inf with a safe default.
float3 SanitizeColor(float3 c, float maxVal) {
    c.r = (isnan(c.r) || isinf(c.r)) ? 0.0 : c.r;
    c.g = (isnan(c.g) || isinf(c.g)) ? 0.0 : c.g;
    c.b = (isnan(c.b) || isinf(c.b)) ? 0.0 : c.b;
    return clamp(c, 0.0, maxVal);
}

// ============================================================================
// Global Resources (space0)
// SceneConstants cbuffer is already declared in Common.hlsli at register(b0).
// All cbuffer fields (viewMatrix, projMatrix, sunDirection, fogColor, etc.)
// are accessed directly as global variables.
// ============================================================================

RaytracingAccelerationStructure g_scene     : register(t0, space0);
RWTexture2D<float4>             g_output    : register(u0);
RWTexture2D<float4>             g_giAccum   : register(u1);
RWTexture2D<float4>             g_normals   : register(u2);  // World normal (xyz) + hit flag (w)
RWTexture2D<float>              g_depth     : register(u3);  // Linear depth

// Bindless texture array
Texture2D    g_textures[]       : register(t4, space0);
SamplerState g_samplerBilinear  : register(s0);  // Bilinear wrap (for water, smooth surfaces)
SamplerState g_samplerPoint     : register(s1);  // Point/nearest wrap (faithful N64 pixel-art)

// ============================================================================
// Derived scene parameters
// These are computed from the cbuffer fields in Common.hlsli.
// Fields that don't exist in the 384-byte cbuffer are given sensible defaults
// or derived from existing data.
// ============================================================================

// Approximate time for water animation (derived from frame count)
float GetTime() { return (float)frameCount * 0.016; }

// Second directional light (fill light opposite to sun for softer shadows)
float3 GetSunDirection2() {
    float3 sd = normalize(sunDirection.xyz);
    return normalize(float3(-sd.x, 0.3, -sd.z));
}
float3 GetSunColor2() { return sunColor.rgb * 0.15; }

// Water material defaults (not in cbuffer, use physically-based constants)
static const float kWaterReflectivity = 0.02;  // Physical F0 for water (IOR 1.33)
static const float kWaterRoughness    = 0.15;  // Moderate water roughness

// AO defaults (not in cbuffer)
static const float kAoRadius    = 50.0;
static const float kAoIntensity = 0.3;

// Sky colors (derived from fog color when not available in cbuffer)
float3 GetSkyHorizonColor() {
    return max(fogColor.rgb, float3(0.25, 0.30, 0.28));
}
float3 GetSkyZenithColor() {
    float3 h = GetSkyHorizonColor();
    return float3(h.r * 0.45, h.g * 0.65, max(h.b * 1.5, 0.45));
}

// ============================================================================
// Per-Geometry Resources (space1, local root signature)
// ============================================================================

StructuredBuffer<RTXVertex>  g_vertices    : register(t0, space1);
StructuredBuffer<uint>       g_indices     : register(t1, space1);
StructuredBuffer<uint>       g_materialIDs : register(t2, space1);
StructuredBuffer<Material>   g_materials   : register(t3, space1);

// ============================================================================
// Ray Generation Shader
// Generates camera rays from inverse view/projection matrices,
// traces primary rays, applies distance fog, and performs temporal
// accumulation for GI noise reduction.
// ============================================================================

// Halton low-discrepancy sequence for temporal jitter
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

    float3 origin = GetCameraPosition();

    // Sub-pixel jitter for temporal anti-aliasing (Halton 2,3 sequence)
    float2 jitter = float2(0.0, 0.0);
    if (frameCount > 0) {
        uint jitterIndex = frameCount;
        jitter.x = HaltonSequence(jitterIndex, 2) - 0.5;
        jitter.y = HaltonSequence(jitterIndex, 3) - 0.5;
    }

    float2 pixelCenter = (float2)launchIndex + 0.5 + jitter;
    float2 ndc = pixelCenter / (float2)launchDim * 2.0 - 1.0;
    ndc.y = -ndc.y; // Flip Y (screen space -> NDC)

    // Generate camera ray using inverse projection/view matrices.
    // Use inverse projection to convert NDC to view-space direction,
    // then inverse view matrix to convert to world-space direction.
    float4 viewSpaceDir = mul(invProjMatrix, float4(ndc, 1.0, 1.0));
    viewSpaceDir.xyz /= viewSpaceDir.w;
    float3 worldDir = mul((float3x3)invViewMatrix, viewSpaceDir.xyz);
    float3 direction = SafeNormalize(worldDir, float3(0, 0, 1));

    // Set up ray
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0.1;       // Near clip (N64 units)
    ray.TMax = 100000.0;  // Far clip

    // Initialize payload
    RayPayload payload;
    payload.color = float3(0, 0, 0);
    payload.distance = 0;
    payload.worldNormal = float3(0, 0, 0);
    payload.hit = 0;
    payload.recursionDepth = 0;

    // Trace primary ray
    TraceRay(
        g_scene,
        RAY_FLAG_NONE,
        0xFF,           // Instance inclusion mask
        0,              // RayContributionToHitGroupIndex
        1,              // MultiplierForGeometryContributionToHitGroupIndex
        0,              // Miss shader index
        ray,
        payload
    );

    float3 finalColor = payload.color;

    // Apply distance fog (linear fog between fogStart and fogEnd)
    if (payload.hit != 0 && fogEnd > fogStart) {
        float fogFactor = saturate((payload.distance - fogStart) / (fogEnd - fogStart));
        finalColor = lerp(finalColor, fogColor.rgb, fogFactor);
    }

    // Sanitize final color: prevent NaN/Inf from reaching the UAV output.
    // This is the last line of defense before pixel data is written.
    finalColor = SanitizeColor(finalColor, MAX_HDR_VALUE);

    // Write color output
    g_output[launchIndex] = float4(finalColor, 1.0);
    g_giAccum[launchIndex] = float4(finalColor, 1.0);

    // Write G-buffer data for denoiser edge-stopping.
    // Sanitize normal: ensure no NaN from water ripple perturbation propagates.
    float3 safeNormal = payload.worldNormal;
    if (any(isnan(safeNormal)) || any(isinf(safeNormal))) {
        safeNormal = float3(0, 1, 0);
    }
    g_normals[launchIndex] = float4(safeNormal, (float)payload.hit);
    // Guard against NaN/Inf distance values from degenerate geometry
    float safeDistance = payload.distance;
    if (isnan(safeDistance) || isinf(safeDistance)) safeDistance = 0.0;
    g_depth[launchIndex] = (payload.hit != 0) ? safeDistance : 0.0;
}

// ============================================================================
// Closest Hit Shader
// Evaluates material at hit point: texture sampling with N64 combiner modes,
// direct lighting with shadow ray, and 1-bounce diffuse GI.
// Uses per-scene material parameters from RTXSceneConfig via SceneConstants.
// ============================================================================

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
    // Safe normalize: avoid NaN from zero-length normals or degenerate N64 geometry.
    // Uses SafeNormalize which also guards against NaN/Inf in the input vector.
    float3 normal = SafeNormalize(rawNormal, float3(0, 1, 0));
    float2 uv       = v0.uv * bary.x + v1.uv * bary.y + v2.uv * bary.z;
    float4 vtxColor = v0.color * bary.x + v1.color * bary.y + v2.color * bary.z;

    // Guard against NaN/Inf in interpolated world position (degenerate vertex data).
    if (any(isnan(worldPos)) || any(isinf(worldPos))) {
        worldPos = WorldRayOrigin() + WorldRayDirection() * RayTCurrent();
    }

    // Guard against NaN/Inf in vertex color (degenerate vertex data)
    vtxColor.r = (isnan(vtxColor.r) || isinf(vtxColor.r)) ? 1.0 : saturate(vtxColor.r);
    vtxColor.g = (isnan(vtxColor.g) || isinf(vtxColor.g)) ? 1.0 : saturate(vtxColor.g);
    vtxColor.b = (isnan(vtxColor.b) || isinf(vtxColor.b)) ? 1.0 : saturate(vtxColor.b);
    vtxColor.a = (isnan(vtxColor.a) || isinf(vtxColor.a)) ? 1.0 : saturate(vtxColor.a);

    // --- Get material ---
    uint matID = g_materialIDs[primitiveIndex];
    Material mat = g_materials[matID];

    // Ensure vertex color has valid alpha (avoid fully transparent surfaces from bad data)
    vtxColor.a = max(vtxColor.a, 0.004); // Minimum alpha to avoid invisible surfaces

    // --- Apply water UV scrolling and ripple normal ---
    bool isWaterSurface = (mat.isWater != 0);
    float sceneTime = GetTime();
    if (isWaterSurface) {
        uv.y += sceneTime * 0.01;
        normal = WaterRippleNormal(worldPos, normal, sceneTime);
    }

    // --- Ensure UVs are valid (no NaN/Inf from degenerate geometry or bad vertex data) ---
    uv.x = (isnan(uv.x) || isinf(uv.x)) ? 0.0 : uv.x;
    uv.y = (isnan(uv.y) || isinf(uv.y)) ? 0.0 : uv.y;

    // --- Apply N64 texture wrap/clamp/mirror modes ---
    // Use the material's stored texture dimensions for exact half-texel clamping.
    uv = ApplyWrapModes(uv, mat.wrapModeS, mat.wrapModeT,
                        (float)mat.texWidthPx, (float)mat.texHeightPx);

    // --- Sample texture ---
    // textureIndex 0 = default white, 1 = checkerboard fallback, 2 = flat normal
    // Index 3+ = actual game textures
    uint texIdx = mat.textureIndex;
    float4 texColor = float4(1, 1, 1, 1);
    bool hasRealTexture = (texIdx >= 3);

    if (texIdx > 0) {
        // Use bilinear (linear) sampling for all textures. N64 textures are low-resolution
        // (16x16, 32x32, etc.) but at RTX resolution bilinear filtering produces much
        // smoother results than nearest-neighbor, eliminating blocky pixelation.
        texColor = g_textures[NonUniformResourceIndex(texIdx)]
                        .SampleLevel(g_samplerBilinear, uv, 0);
        // Guard against all-zero texture samples from freed/corrupted SRVs.
        // Replace with a procedural magenta/black checkerboard pattern so the issue
        // is immediately visible instead of producing invisible/transparent surfaces.
        if (IsCorruptedTextureSample(texColor)) {
            texColor = MissingTextureFallback(worldPos);
            hasRealTexture = false;
        }
    }

    // --- Apply simplified N64 color combiner ---
    // When real textures are loaded, tex * vtxColor gives correct N64-style rendering.
    // When textures are not loaded (texIdx==0 or corrupted), derive a fallback color
    // from surface properties to avoid flat white surfaces everywhere.
    float3 albedo;
    // needsFallbackColor is true when no valid texture data is available.
    // This includes: texIdx==0 (no texture assigned), or texIdx>=3 but the
    // sampled data was all-zero (corrupted/freed SRV, hasRealTexture set to false).
    // texIdx 1-2 (checkerboard/normal) are special textures that should NOT trigger fallback.
    bool hasAnyValidTexture = hasRealTexture || (texIdx > 0 && texIdx < 3);
    bool needsFallbackColor = !hasAnyValidTexture;
    switch (mat.combinerMode) {
        case COMBINER_MODULATE_RGB:
        case COMBINER_MODULATE_RGBA:
            if (hasRealTexture) {
                albedo = texColor.rgb * vtxColor.rgb;
            } else if (needsFallbackColor) {
                // Use vertex color if it has meaningful color data.
                // Near-white vertex colors (from RSP lighting mode) get normal-based tint.
                float vtxLuminance = dot(vtxColor.rgb, float3(0.299, 0.587, 0.114));
                if (vtxLuminance > 0.85) {
                    float upFactor = saturate(abs(normal.y));
                    float3 groundColor = float3(0.25, 0.35, 0.15);
                    float3 wallColor   = float3(0.40, 0.30, 0.20);
                    float3 posHash = frac(worldPos * 0.007);
                    float variation = 0.1 * (posHash.x + posHash.y + posHash.z) / 3.0;
                    albedo = lerp(wallColor, groundColor, upFactor) + variation;
                } else {
                    albedo = vtxColor.rgb;
                }
            } else {
                albedo = texColor.rgb * vtxColor.rgb;
            }
            break;
        case COMBINER_DECAL:
            if (hasRealTexture) {
                albedo = texColor.rgb;
            } else if (needsFallbackColor) {
                float upFactor = saturate(abs(normal.y));
                float3 posHash = frac(worldPos * 0.007);
                float variation = 0.08 * (posHash.x + posHash.z) / 2.0;
                albedo = lerp(float3(0.38, 0.30, 0.22), float3(0.30, 0.38, 0.20), upFactor) + variation;
            } else {
                albedo = texColor.rgb;
            }
            break;
        case COMBINER_SHADE:
            albedo = vtxColor.rgb;
            break;
        case COMBINER_TEX_ENV_BLEND:
            if (hasRealTexture) {
                albedo = lerp(texColor.rgb, float3(0.5, 0.5, 0.5), texColor.a);
            } else {
                albedo = float3(0.35, 0.40, 0.30);
            }
            break;
        default:
            if (hasRealTexture) {
                albedo = texColor.rgb * vtxColor.rgb;
            } else if (needsFallbackColor) {
                float vtxLuminance = dot(vtxColor.rgb, float3(0.299, 0.587, 0.114));
                if (vtxLuminance > 0.85) {
                    float upFactor = saturate(abs(normal.y));
                    float3 groundColor = float3(0.25, 0.35, 0.15);
                    float3 wallColor   = float3(0.40, 0.30, 0.20);
                    float3 posHash = frac(worldPos * 0.007);
                    float variation = 0.1 * (posHash.x + posHash.y + posHash.z) / 3.0;
                    albedo = lerp(wallColor, groundColor, upFactor) + variation;
                } else {
                    albedo = vtxColor.rgb;
                }
            } else {
                albedo = texColor.rgb * vtxColor.rgb;
            }
            break;
    }

    // Clamp albedo to valid range (protect against NaN from bad texture data)
    albedo = saturate(albedo);

    // --- Direct lighting ---
    // Use abs(dot) for bi-directional lighting — N64 geometry normals can face
    // the wrong way since backface culling was handled by the RSP, not the geometry.
    float3 sunDir1 = normalize(sunDirection.xyz);
    float3 sunCol1 = sunColor.rgb * max(sunIntensity, 0.01);
    float3 sunDir2 = GetSunDirection2();
    float3 sunCol2 = GetSunColor2();
    // Safety: if sun direction is zero-length, use default
    if (length(sunDirection.xyz) < 0.01) {
        sunDir1 = normalize(float3(0.5, 0.8, 0.3));
    }
    // Safety: if sun color is zero, use warm white default
    if (dot(sunColor.rgb, float3(1, 1, 1)) < 0.01) {
        sunCol1 = float3(1.0, 0.95, 0.9) * 2.0;
    }
    float NdotL1 = max(abs(dot(normal, sunDir1)), 0.0);
    float NdotL2 = max(abs(dot(normal, sunDir2)), 0.0);
    float3 directLight = sunCol1 * NdotL1 + sunCol2 * NdotL2;

    // Ambient sky light: cool blue-ish contribution simulating the sky hemisphere.
    // Shadowed areas receive ambient + direct*shadowFloor + GI, creating
    // warm-sun/cool-shadow contrast while keeping forest scenes visible.
    float3 ambient = ambientColor.rgb;
    float ambInt = max(ambientIntensity, 0.01);
    ambient *= ambInt;
    // Minimum ambient ensures shadows are never pitch black.
    // For forest scenes this must be high enough to keep ground visible under canopy.
    float ambientFloor = max(ambientIntensity, 0.10);
    // Use a neutral (white) ambient floor so the minimum ambient doesn't introduce
    // a color cast. The scene's ambientColor already carries any intended color tone
    // (cool blue for shadows, etc.) set by RTXSceneConfig. Previously this used
    // asymmetric multipliers (0.85, 1.0, 1.3) which created a blue-green tint on
    // ALL surfaces — causing an overall color cast when combined with warm sunlight.
    ambient = max(ambient, float3(ambientFloor, ambientFloor, ambientFloor));
    // Hemisphere weighting: upward-facing surfaces get more sky light.
    // Narrower range [0.75, 1.35] avoids making horizontal surfaces too dark.
    float skyHemisphere = saturate(normal.y * 0.5 + 0.5);
    float hemisphereBoost = lerp(0.75, 1.35, skyHemisphere);
    ambient *= hemisphereBoost;
    // Subtle green ground bounce for downward-facing surfaces
    float groundBounce = saturate(-normal.y * 0.5 + 0.5);
    ambient += float3(0.03, 0.06, 0.02) * groundBounce;

    // --- Soft shadow ray for primary directional light (sun disk sampling) ---
    // Jitter the shadow ray direction within a small cone representing the
    // angular size of the sun disk. This produces soft shadow penumbra at
    // shadow boundaries, essential for realistic forest lighting.
    float shadow = 1.0;
    if (payload.recursionDepth < MAX_TRACE_RECURSION_DEPTH - 1) {
        // Compute primary light direction (from surface toward sun)
        // sunDir1 already points toward the sun
        float3 lightDir = sunDir1;

        // Build tangent frame for sun disk sampling.
        // SafeNormalize guards against degenerate cross products when lightDir
        // is nearly parallel to the reference up/right vector.
        float3 sunTangent = abs(lightDir.y) < 0.999 ?
            SafeNormalize(cross(float3(0, 1, 0), lightDir), float3(1, 0, 0)) :
            SafeNormalize(cross(float3(1, 0, 0), lightDir), float3(0, 0, 1));
        float3 sunBitangent = cross(lightDir, sunTangent);

        // Per-pixel, per-frame random offset within the sun disk
        uint2 pixel = DispatchRaysIndex().xy;
        uint shadowSeed = pixel.x * 1973u + pixel.y * 9277u + frameCount * 26699u;
        float r1 = Random01(shadowSeed);
        float r2 = Random01(shadowSeed + 7919u);

        // Uniform disk sampling: ~0.018 rad ≈ 1° half-angle
        float sunAngularRadius = 0.018;
        float diskR = sqrt(r1) * sunAngularRadius;
        float diskTheta = r2 * 6.28318530718;
        float2 diskOffset = float2(diskR * cos(diskTheta), diskR * sin(diskTheta));
        // SafeNormalize guards against the astronomically unlikely case where
        // the disk offset perfectly cancels the light direction.
        float3 jitteredLightDir = SafeNormalize(
            lightDir + sunTangent * diskOffset.x + sunBitangent * diskOffset.y,
            lightDir);

        // Robust two-part shadow bias:
        // 1. Normal bias (scaled by NdotL for grazing angles)
        // 2. Light direction bias
        float NdotShadowL = saturate(dot(normal, jitteredLightDir));
        float normalBiasScale = lerp(3.0, 1.0, NdotShadowL);
        float3 shadowOrigin = worldPos + normal * normalBiasScale + jitteredLightDir * 0.5;

        RayDesc shadowRay;
        shadowRay.Origin = shadowOrigin;
        shadowRay.Direction = jitteredLightDir;
        shadowRay.TMin = 0.01;
        shadowRay.TMax = 200000.0;

        RayPayload shadowPayload;
        shadowPayload.color = float3(0, 0, 0);
        shadowPayload.worldNormal = float3(0, 0, 0);
        shadowPayload.hit = 1;   // Assume occluded; Miss shader will set to 0
        shadowPayload.recursionDepth = payload.recursionDepth + 1;
        shadowPayload.distance = 0;

        TraceRay(
            g_scene,
            RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER,
            0xFF, 0, 1, 0,
            shadowRay,
            shadowPayload
        );

        // Shadow factor: shadowFloor in full shadow, 1.0 in full light.
        // The shadow floor of 0.20 ensures that even fully-occluded surfaces
        // receive 20% of the direct light. This simulates light scattering
        // through tree canopy and prevents forest scenes from becoming nearly
        // black under dense foliage. With temporal accumulation, the shadow
        // result still converges to smooth penumbra with a higher minimum.
        float shadowFloor = 0.20;
        shadow = (shadowPayload.hit != 0) ? shadowFloor : 1.0;
    }

    // --- Global illumination bounce (only from primary rays) ---
    float3 giContribution = float3(0, 0, 0);
    if (payload.recursionDepth < MAX_TRACE_RECURSION_DEPTH - 1 && giIntensity > 0.0) {
        uint2 pixel = DispatchRaysIndex().xy;
        float2 rand = float2(
            Random01(pixel.x + pixel.y * 8192u + frameCount * 65536u),
            Random01(pixel.x + pixel.y * 8192u + frameCount * 65536u + 1u)
        );
        float3 giDir = SampleCosineHemisphere(normal, rand);

        RayDesc giRay;
        giRay.Origin = worldPos + normal * 1.5;
        giRay.Direction = giDir;
        giRay.TMin = 0.5;
        // Guard against negative or zero aoRadius from misconfigured scene data.
        giRay.TMax = (kAoRadius > 0.001) ? max(kAoRadius * 200.0, 1.0) : 10000.0;

        RayPayload giPayload;
        giPayload.color = float3(0, 0, 0);
        giPayload.worldNormal = float3(0, 0, 0);
        giPayload.recursionDepth = payload.recursionDepth + 1;  // This is the bounce ray
        giPayload.distance = 0;
        giPayload.hit = 0;

        TraceRay(g_scene, RAY_FLAG_NONE, 0xFF, 0, 1, 0, giRay, giPayload);

        if (giPayload.hit != 0) {
            // Modulate bounce light by primary surface albedo and GI intensity.
            // Sanitize bounce color to prevent NaN/Inf from propagating into GI.
            float3 bounceColor = SanitizeColor(giPayload.color, MAX_HDR_VALUE);
            giContribution = bounceColor * albedo * 0.5 * giIntensity;
        }
    }

    // --- Apply simple ambient occlusion estimate ---
    float aoFactor = 1.0;
    if (kAoIntensity > 0.0) {
        aoFactor = 1.0 - kAoIntensity * 0.3;
    }

    // =========================================================================
    // Water surface reflections with traced reflection rays
    // =========================================================================
    // For water surfaces, cast a reflection ray and blend with Schlick Fresnel.
    // The reflection ray captures actual scene geometry reflections (trees, sky,
    // structures, etc.) producing convincing water in Kokiri Forest's stream/pond.
    //
    // Water rendering pipeline:
    //   1. Fresnel-weighted reflection (strong at grazing angles, weak at normal incidence)
    //   2. Roughness-perturbed reflection direction (soft, natural-looking reflections)
    //   3. Traced reflection ray with sky/ambient fallback on miss
    //   4. Water color absorption tint (red > green > blue)
    //   5. Depth-based absorption for reflected geometry
    //   6. Sun specular glints on the water surface
    //   7. Subsurface scattering approximation for shallow water glow
    float3 baseColor = albedo * (directLight * shadow + ambient * aoFactor) + giContribution;
    if (isWaterSurface) {
        // Safe view direction: guard against camera exactly on the water surface
        // (zero-length vector → NaN after normalize). Fall back to surface normal
        // which gives a reasonable straight-down view approximation.
        float3 rawViewDir = GetCameraPosition() - worldPos;
        float viewDirLen = length(rawViewDir);
        float3 viewDir = (viewDirLen > 0.001) ? (rawViewDir / viewDirLen) : normal;

        // Schlick Fresnel approximation using the Common.hlsli helper.
        // Physical F0 for water is ~0.02 (dielectric IOR ≈ 1.33).
        // We use a floor of 0.02 to ensure water always has at least physically
        // correct reflectivity, while allowing waterReflectivity to boost it
        // for artistic effect. If waterReflectivity is 0, we still use the
        // physical minimum so water surfaces look like water (not matte plastic).
        float F0 = clamp(max(kWaterReflectivity, 0.02), 0.02, 1.0);
        float cosTheta = saturate(dot(viewDir, normal));
        float fresnel = SchlickFresnel(F0, cosTheta);

        // Compute sky color approximation for fallback when reflection ray misses
        // or when we can't trace (non-primary rays). Uses the scene's configured
        // sky colors for consistent look with the Miss shader's sky dome.
        // Derive sky colors from fog color
        float3 skyHorizon = GetSkyHorizonColor();
        float3 skyZenith = GetSkyZenithColor();
        skyHorizon = max(skyHorizon, float3(0.15, 0.18, 0.20));
        skyZenith  = max(skyZenith,  float3(0.08, 0.12, 0.25));

        // Trace a reflection ray (only from primary rays to limit recursion).
        // MAX_TRACE_RECURSION_DEPTH is 2, so from depth 0 we can trace at depth 1.
        // This guard prevents infinite reflection bounces between water surfaces.
        float3 reflectColor = float3(0, 0, 0);
        if (payload.recursionDepth < MAX_TRACE_RECURSION_DEPTH - 1) {
            float3 reflectDir = reflect(-viewDir, normal);

            // Cosine-weighted hemisphere perturbation around the reflection direction.
            // This approximates a GGX-like specular lobe: most samples cluster near
            // the perfect reflection direction, with occasional wider scatters for
            // rougher surfaces.
            uint2 pixel = DispatchRaysIndex().xy;
            uint rngSeed = pixel.x + pixel.y * 8192u + frameCount * 131072u;
            float2 reflRand = float2(
                Random01(rngSeed + 7u),
                Random01(rngSeed + 13u)
            );
            // Clamp roughness to valid range to prevent extreme perturbations
            float roughnessFactor = saturate(kWaterRoughness) * 0.3;
            // Use cosine-weighted hemisphere sampling around the reflection direction.
            float3 perturbedDir = SampleCosineHemisphere(reflectDir, reflRand);
            // Blend between perfect reflection and perturbed direction based on roughness.
            // roughnessFactor=0 → perfect mirror; roughnessFactor=0.3 → maximum blur.
            float3 blendedDir = lerp(reflectDir, perturbedDir, roughnessFactor);
            // Use SafeNormalize to avoid NaN from zero-length lerp result.
            reflectDir = SafeNormalize(blendedDir, reflectDir);

            // Ensure the reflection ray doesn't go below the water surface
            // (which would cause self-intersection artifacts)
            if (dot(reflectDir, normal) < 0.01) {
                float3 nudgedDir = reflectDir + normal * 0.1;
                float nudgedLen = length(nudgedDir);
                // Final safety: if nudging also fails (nearly impossible), use normal as reflection
                reflectDir = (nudgedLen > 0.001) ? (nudgedDir / nudgedLen) : normal;
            }

            RayDesc reflectRay;
            reflectRay.Origin = worldPos + normal * 2.0; // Bias away from surface
            reflectRay.Direction = reflectDir;
            reflectRay.TMin = 0.5;
            reflectRay.TMax = 100000.0;

            RayPayload reflectPayload;
            reflectPayload.color = float3(0, 0, 0);
            reflectPayload.distance = 0;
            reflectPayload.worldNormal = float3(0, 0, 0);
            reflectPayload.hit = 0;
            reflectPayload.recursionDepth = payload.recursionDepth + 1; // Mark as bounce ray

            TraceRay(g_scene, RAY_FLAG_NONE, 0xFF, 0, 1, 0, reflectRay, reflectPayload);

            if (reflectPayload.hit != 0) {
                // Reflection hit geometry — use its color with distance-based
                // atmospheric fade. Very distant reflections fade toward the sky
                // color, simulating atmospheric scattering in the reflection path.
                // Sanitize the bounce color to prevent NaN/Inf propagation from
                // degenerate geometry or bad material data in the reflected scene.
                float3 safeReflColor = SanitizeColor(reflectPayload.color, MAX_HDR_VALUE);
                float reflDist = max(reflectPayload.distance, 0.0);
                float atmosFade = saturate(reflDist / 50000.0);
                float reflSkyT = saturate(reflectDir.y * 0.5 + 0.5);
                float3 distantSky = lerp(skyHorizon, skyZenith, smoothstep(0.0, 0.6, reflSkyT));
                reflectColor = lerp(safeReflColor, distantSky, atmosFade);
            } else {
                // Reflection ray missed all geometry — return sky/ambient color.
                // Match the Miss shader's sky gradient for visual consistency.
                float reflSkyT = saturate(reflectDir.y);
                float reflSkyBlend = smoothstep(0.0, 0.6, reflSkyT);
                reflectColor = lerp(skyHorizon, skyZenith, reflSkyBlend);

                // Add sun glow in the reflection (sun reflected on water surface).
                // Use saturate on sunDot to ensure pow() input is in [0,1].
                float sunDot = saturate(dot(reflectDir, sunDir1));
                float sunGlow = pow(sunDot, 128.0) * 0.8 + pow(sunDot, 32.0) * 0.15;
                reflectColor += sunCol1 * sunGlow;

                // Below-horizon reflections: dark ground color
                if (reflectDir.y < 0.0) {
                    float groundFade = saturate(-reflectDir.y * 2.0);
                    float3 groundColor = float3(0.06, 0.08, 0.05);
                    reflectColor = lerp(skyHorizon, groundColor, groundFade);
                }
            }
        } else {
            // For non-primary rays (e.g., GI bounce hitting water), use a sky
            // approximation instead of tracing further to stay within recursion limits.
            float3 approxReflectDir = reflect(-viewDir, normal);
            float reflSkyT = saturate(approxReflectDir.y);
            float reflSkyBlend = smoothstep(0.0, 0.6, reflSkyT);
            reflectColor = lerp(skyHorizon, skyZenith, reflSkyBlend);
        }

        // Sanitize reflectColor: replace NaN/Inf and clamp to HDR range.
        // Sun glow and sky gradient are additive and can exceed 1.0 in HDR scenarios;
        // we allow values up to MAX_HDR_VALUE for specular highlights.
        reflectColor = SanitizeColor(reflectColor, MAX_HDR_VALUE);

        // Apply water color absorption tint to the reflection.
        // Water absorbs red wavelengths most, then green, then blue.
        // This gives reflections a characteristic cool blue-green tint.
        float3 waterTint = float3(0.7, 0.85, 1.0);
        reflectColor *= waterTint;

        // Compute the final reflection blend strength.
        // At glancing angles (low cosTheta), fresnel → 1.0 → strong mirror-like reflection.
        // At normal incidence (cosTheta → 1), fresnel → F0 → mostly see-through water.
        // Roughness reduces the overall reflection intensity (rough water scatters more).
        // Saturate waterRoughness to guard against out-of-range config values.
        float reflectionStrength = fresnel * (1.0 - saturate(kWaterRoughness) * 0.5);
        // Ensure a minimum visible reflection even at normal incidence.
        // Saturate to [0.03, 1.0] to prevent color inversion from bad config data.
        reflectionStrength = saturate(max(reflectionStrength, 0.03));

        // Blend reflected color with the lit water surface color
        baseColor = lerp(baseColor, reflectColor, reflectionStrength);

        // Subtle subsurface scattering approximation for shallow water
        if (payload.recursionDepth < MAX_TRACE_RECURSION_DEPTH - 1) {
            float3 sssLightDir = -sunDir1;
            float scatter = saturate(dot(viewDir, -sssLightDir)) * (1.0 - cosTheta) * 0.08;
            float3 scatterColor = float3(0.05, 0.15, 0.12) * sunCol1;
            baseColor += scatterColor * scatter * shadow;
        }

        // Sun specular glints on water surface
        // Blinn-Phong: H = normalize(V + L), where L = sunDirection1 (toward sun)
        if (payload.recursionDepth < MAX_TRACE_RECURSION_DEPTH - 1) {
            float3 rawHalfVec = viewDir + sunDir1;
            float halfVecLen = length(rawHalfVec);
            // Guard against zero-length half vector (view and light directions cancel).
            // This can happen when the camera looks directly along the light direction.
            if (halfVecLen > 0.001) {
                float3 halfVec = rawHalfVec / halfVecLen;
                float specAngle = saturate(dot(normal, halfVec));
                // Clamp waterRoughness and specPower to safe ranges.
                float clampedRoughness = saturate(kWaterRoughness);
                float specPower = lerp(256.0, 32.0, clampedRoughness);
                specPower = clamp(specPower, 1.0, 512.0);
                float spec = pow(specAngle, specPower) * shadow;
                // Clamp specular contribution to prevent extreme brightness
                baseColor += sunCol1 * min(spec, 2.0) * 0.6;
            }
        }
    }

    // Final color sanitization: replace any NaN/Inf components and clamp to HDR range.
    // This is the last line of defense against bad data from any path above.
    // SanitizeColor replaces NaN/Inf per-component (preserving valid channels)
    // and clamps to [0, MAX_HDR_VALUE].
    baseColor = SanitizeColor(baseColor, MAX_HDR_VALUE);

    // --- Final color ---
    payload.color = baseColor;
    payload.distance = RayTCurrent();
    // Ensure the output normal is valid (could have been perturbed to NaN by water ripples)
    payload.worldNormal = SafeNormalize(normal, float3(0, 1, 0));
    payload.hit = 1;
}

// ============================================================================
// Any Hit Shader
// Alpha test for foliage and other alpha-tested materials.
// Called for non-opaque geometry (BLAS flag: NONE) before accepting a hit.
// Applies the Deku Tree death alpha fade to vegetation.
// ============================================================================

[shader("anyhit")]
void AnyHit(inout RayPayload payload, in BuiltInTriangleIntersectionAttributes attribs) {
    uint primitiveIndex = PrimitiveIndex();
    uint matID = g_materialIDs[primitiveIndex];
    Material mat = g_materials[matID];

    // Only perform alpha test for materials flagged as alpha-tested
    if (mat.isAlphaTested == 0) return;

    // Decal/overlay geometry (e.g., dirt paths over grass in OoT) should never be
    // discarded by alpha testing. These are rendered as a second pass on top of the
    // base terrain; discarding them hides the path overlay entirely (Bug 2).
    if (mat.isDecal != 0) return;

    // If the texture hasn't been loaded yet (index 0 = white, 1 = checkerboard),
    // skip alpha testing — the default white texture has alpha=1 so test always passes
    if (mat.textureIndex < 3) return;

    // Interpolate UVs from triangle vertices
    uint i0 = g_indices[primitiveIndex * 3 + 0];
    uint i1 = g_indices[primitiveIndex * 3 + 1];
    uint i2 = g_indices[primitiveIndex * 3 + 2];

    float3 bary = float3(
        1.0 - attribs.barycentrics.x - attribs.barycentrics.y,
        attribs.barycentrics.x,
        attribs.barycentrics.y
    );

    float2 uv = g_vertices[i0].uv * bary.x +
                g_vertices[i1].uv * bary.y +
                g_vertices[i2].uv * bary.z;

    // Sanitize UVs to avoid NaN/Inf from degenerate geometry or bad vertex data
    uv.x = (isnan(uv.x) || isinf(uv.x)) ? 0.0 : uv.x;
    uv.y = (isnan(uv.y) || isinf(uv.y)) ? 0.0 : uv.y;

    // Apply N64 texture wrap/clamp/mirror modes.
    // Use the material's stored texture dimensions for exact half-texel clamping.
    uv = ApplyWrapModes(uv, mat.wrapModeS, mat.wrapModeT,
                        (float)mat.texWidthPx, (float)mat.texHeightPx);

    // Sample texture alpha using bilinear sampler for smooth alpha test edges at RTX resolution
    float alpha = g_textures[NonUniformResourceIndex(mat.textureIndex)]
                    .SampleLevel(g_samplerBilinear, uv, 0).a;

    // Apply Deku Tree death alpha fade to foliage.
    // dekuTreeAlpha is normally 1.0 (fully visible). During the Deku Tree death
    // cutscene, it fades from 1.0 → 0.0 to make vegetation disappear.
    // Note: dekuTreeAlpha is not available in the current cbuffer layout.
    // Use 1.0 (fully visible) as default until cbuffer is extended.
    float dekuTreeAlpha = 1.0;
    alpha *= dekuTreeAlpha;

    // Alpha test threshold: use 0.4 to account for bilinear filtering and
    // dekuTreeAlpha multiplication reducing edge alpha values slightly.
    // See AnyHit.hlsl for detailed rationale.
    if (alpha < 0.4) {
        IgnoreHit();
    }
}

// ============================================================================
// Miss Shader
// Returns sky/fog color when a ray doesn't hit any geometry.
// Produces a natural-looking sky gradient with sun glow for outdoor scenes.
// ============================================================================

[shader("miss")]
void Miss(inout RayPayload payload) {
    float3 rayDir = WorldRayDirection();

    // Sky gradient: t=0 at horizon, t=1 straight up
    float t = saturate(rayDir.y);

    // Derive sky colors from fog color
    float3 horizonColor = GetSkyHorizonColor();
    float3 zenithColor = GetSkyZenithColor();

    // Ensure horizon is never too dark
    horizonColor = max(horizonColor, float3(0.15, 0.18, 0.20));

    float skyBlend = smoothstep(0.0, 0.6, t);
    float3 skyColor = lerp(horizonColor, zenithColor, skyBlend);

    // Sun glow around the sun direction: three-tier (core, halo, scatter)
    float3 missSunDir = normalize(sunDirection.xyz);
    if (length(sunDirection.xyz) < 0.01) {
        missSunDir = normalize(float3(0.5, 0.8, 0.3));
    }
    float sunDot = saturate(dot(rayDir, missSunDir));
    float sunGlow = pow(sunDot, 128.0) * 1.2
                  + pow(sunDot, 32.0) * 0.25
                  + pow(sunDot, 6.0) * 0.10;
    float3 missSunCol = sunColor.rgb;
    if (dot(missSunCol, float3(1,1,1)) < 0.01) {
        missSunCol = float3(1.0, 0.95, 0.9);
    }
    float3 sunGlowColor = missSunCol * sunGlow;

    // Below horizon: dark ground approximation (forest floor color)
    if (rayDir.y < 0.0) {
        float groundFade = saturate(-rayDir.y * 2.0);
        float3 groundColor = float3(0.08, 0.10, 0.06);
        skyColor = lerp(horizonColor, groundColor, groundFade);
        sunGlowColor *= (1.0 - groundFade);
    }

    payload.color = skyColor + sunGlowColor;
    payload.distance = 100000.0;
    payload.worldNormal = float3(0, 0, 0);
    payload.hit = 0;
}
