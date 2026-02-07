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

// ============================================================================
// Global Resources (space0)
// ============================================================================

RaytracingAccelerationStructure g_scene     : register(t0, space0);
RWTexture2D<float4>             g_output    : register(u0);
RWTexture2D<float4>             g_giAccum   : register(u1);
ConstantBuffer<SceneConstants>  g_constants : register(b0);

// Bindless texture array
Texture2D    g_textures[] : register(t4, space0);
SamplerState g_sampler    : register(s0);

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
        0,              // Hit group stride
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
    // On camera movement, frameCount resets to 0 on the C++ side (GISystem).
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
    float3 normal   = normalize(v0.normal * bary.x + v1.normal * bary.y + v2.normal * bary.z);
    float2 uv       = v0.uv * bary.x + v1.uv * bary.y + v2.uv * bary.z;
    float4 vtxColor = v0.color * bary.x + v1.color * bary.y + v2.color * bary.z;

    // --- Get material ---
    uint matID = g_materialIDs[primitiveIndex];
    Material mat = g_materials[matID];

    // --- Apply water UV scrolling ---
    bool isWaterSurface = (mat.isWater != 0);
    if (isWaterSurface) {
        uv.y += g_constants.time * 0.01;
    }

    // --- Sample texture ---
    float4 texColor = g_textures[NonUniformResourceIndex(mat.textureIndex)]
                        .SampleLevel(g_sampler, uv, 0);

    // --- Apply simplified N64 color combiner ---
    float3 albedo;
    switch (mat.combinerMode) {
        case COMBINER_MODULATE_RGB:
        case COMBINER_MODULATE_RGBA:
            albedo = texColor.rgb * vtxColor.rgb;
            break;
        case COMBINER_DECAL:
            albedo = texColor.rgb;
            break;
        case COMBINER_SHADE:
            albedo = vtxColor.rgb;
            break;
        case COMBINER_TEX_ENV_BLEND:
            albedo = lerp(texColor.rgb, float3(0.5, 0.5, 0.5), texColor.a);
            break;
        default:
            albedo = texColor.rgb * vtxColor.rgb;
            break;
    }

    // --- Direct lighting ---
    float NdotL1 = max(dot(normal, -g_constants.sunDirection1), 0.0);
    float NdotL2 = max(dot(normal, -g_constants.sunDirection2), 0.0);
    float3 directLight = g_constants.sunColor1 * NdotL1 + g_constants.sunColor2 * NdotL2;
    float3 ambient = g_constants.ambientColor;

    // --- Shadow ray for primary directional light ---
    float shadow = 1.0;
    if (payload.recursionDepth == 0) {
        RayDesc shadowRay;
        shadowRay.Origin = worldPos + normal * 0.1;  // Bias to avoid self-intersection
        shadowRay.Direction = -g_constants.sunDirection1;
        shadowRay.TMin = 0.1;
        shadowRay.TMax = 100000.0;

        RayPayload shadowPayload;
        shadowPayload.color = float3(0, 0, 0);
        shadowPayload.hit = false;
        shadowPayload.recursionDepth = payload.recursionDepth + 1;
        shadowPayload.distance = 0;

        TraceRay(
            g_scene,
            RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER,
            0xFF, 0, 0, 0,
            shadowRay,
            shadowPayload
        );

        // Soft shadow: don't go fully black, keep some ambient
        shadow = shadowPayload.hit ? 0.3 : 1.0;
    }

    // --- Global illumination bounce (only from primary rays) ---
    float3 giContribution = float3(0, 0, 0);
    if (payload.recursionDepth == 0 && g_constants.giIntensity > 0.0) {
        uint2 pixel = DispatchRaysIndex().xy;
        float2 rand = float2(
            Random01(pixel.x + pixel.y * 8192u + g_constants.frameCount * 65536u),
            Random01(pixel.x + pixel.y * 8192u + g_constants.frameCount * 65536u + 1u)
        );
        float3 giDir = SampleCosineHemisphere(normal, rand);

        RayDesc giRay;
        giRay.Origin = worldPos + normal * 0.1;
        giRay.Direction = giDir;
        giRay.TMin = 0.1;
        giRay.TMax = g_constants.aoRadius > 0.0 ? g_constants.aoRadius * 200.0 : 10000.0;

        RayPayload giPayload;
        giPayload.color = float3(0, 0, 0);
        giPayload.recursionDepth = 1;  // This is the bounce ray
        giPayload.distance = 0;
        giPayload.hit = false;

        TraceRay(g_scene, RAY_FLAG_NONE, 0xFF, 0, 0, 0, giRay, giPayload);

        if (giPayload.hit) {
            // Modulate bounce light by primary surface albedo and GI intensity
            giContribution = giPayload.color * albedo * 0.5 * g_constants.giIntensity;
        }
    }

    // --- Apply simple ambient occlusion estimate ---
    float aoFactor = 1.0;
    if (g_constants.aoIntensity > 0.0) {
        aoFactor = 1.0 - g_constants.aoIntensity * 0.3;
    }

    // --- Water surface reflectivity ---
    float3 baseColor = albedo * (directLight * shadow + ambient * aoFactor) + giContribution;
    if (isWaterSurface && g_constants.waterReflectivity > 0.0) {
        float3 viewDir = normalize(g_constants.cameraPos - worldPos);
        float fresnel = g_constants.waterReflectivity +
                        (1.0 - g_constants.waterReflectivity) * pow(1.0 - saturate(dot(viewDir, normal)), 5.0);
        float3 reflectColor = g_constants.fogColor * 1.2;
        baseColor = lerp(baseColor, reflectColor, fresnel * (1.0 - g_constants.waterRoughness));
    }

    // --- Final color ---
    payload.color = baseColor;
    payload.distance = RayTCurrent();
    payload.hit = true;
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
    if (!mat.isAlphaTested) return;

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

    // Sample texture alpha
    float alpha = g_textures[NonUniformResourceIndex(mat.textureIndex)]
                    .SampleLevel(g_sampler, uv, 0).a;

    // Apply Deku Tree death alpha fade to foliage
    alpha *= g_constants.dekuTreeAlpha;

    // Alpha test threshold (N64 uses 0.5 equivalent)
    if (alpha < 0.5) {
        IgnoreHit();
    }
}

// ============================================================================
// Miss Shader
// Returns sky/fog color when a ray doesn't hit any geometry.
// Blends between fog color at the horizon and a brighter sky at zenith.
// ============================================================================

[shader("miss")]
void Miss(inout RayPayload payload) {
    // Simple sky gradient based on ray direction
    float3 rayDir = WorldRayDirection();

    // t=0 at horizon, t=1 straight up
    float t = saturate(rayDir.y * 0.5 + 0.5);

    // Blend between fog color (horizon) and a slightly brighter sky (zenith)
    float3 skyColor = lerp(g_constants.fogColor, g_constants.fogColor * 1.3, t);

    payload.color = skyColor;
    payload.distance = 100000.0;
    payload.hit = false;
}
