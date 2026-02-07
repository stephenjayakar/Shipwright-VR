// ============================================================================
// ClosestHit.hlsl - Closest Hit Shader
// Evaluates material at hit point: texture sampling, N64 combiner,
// direct lighting with shadow ray, and 1-bounce GI.
// ============================================================================

#include "Common.hlsli"

// Global resources
RaytracingAccelerationStructure g_scene     : register(t0, space0);
ConstantBuffer<SceneConstants>  g_constants : register(b0);

// Per-geometry resources (local root signature, space1)
StructuredBuffer<RTXVertex>  g_vertices    : register(t0, space1);
StructuredBuffer<uint>       g_indices     : register(t1, space1);
StructuredBuffer<uint>       g_materialIDs : register(t2, space1);
StructuredBuffer<Material>   g_materials   : register(t3, space1);

// Bindless texture array (global root signature, space0)
Texture2D    g_textures[] : register(t4, space0);
SamplerState g_sampler    : register(s0);

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
        } else {
            // GI ray missed (open sky) — no AO darkening for this sample
        }
    }

    // --- Apply simple ambient occlusion estimate ---
    // AO is naturally captured by the GI bounce: if the bounce ray hits
    // nearby geometry, less indirect light reaches this point. We add an
    // additional AO darkening factor controlled by aoIntensity.
    float aoFactor = 1.0;
    // (AO is implicitly handled by the GI bounce; aoIntensity scales the
    //  ambient term to fake additional occlusion darkening in dense areas.)
    if (g_constants.aoIntensity > 0.0) {
        // Reduce ambient in proportion to AO intensity setting.
        // This is a rough approximation: in a full implementation, a
        // separate short-range AO ray would be cast.
        aoFactor = 1.0 - g_constants.aoIntensity * 0.3;
    }

    // --- Water surface reflectivity ---
    // For water surfaces, blend in a simple Fresnel reflection approximation
    // using waterReflectivity from the scene config.
    float3 baseColor = albedo * (directLight * shadow + ambient * aoFactor) + giContribution;
    if (isWaterSurface && g_constants.waterReflectivity > 0.0) {
        float3 viewDir = normalize(g_constants.cameraPos - worldPos);
        float fresnel = g_constants.waterReflectivity +
                        (1.0 - g_constants.waterReflectivity) * pow(1.0 - saturate(dot(viewDir, normal)), 5.0);
        // Blend between water surface color and reflected sky/fog color
        float3 reflectColor = g_constants.fogColor * 1.2; // Approximation: reflect sky/fog
        baseColor = lerp(baseColor, reflectColor, fresnel * (1.0 - g_constants.waterRoughness));
    }

    // --- Final color ---
    payload.color = baseColor;
    payload.distance = RayTCurrent();
    payload.hit = true;
}
