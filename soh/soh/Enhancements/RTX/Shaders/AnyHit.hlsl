// ============================================================================
// AnyHit.hlsl - Any Hit Shader
// Alpha test for foliage and other alpha-tested materials.
// Called for non-opaque geometry before accepting a hit.
// ============================================================================

#include "Common.hlsli"

// Per-geometry resources (local root signature, space1)
StructuredBuffer<RTXVertex>  g_vertices    : register(t0, space1);
StructuredBuffer<uint>       g_indices     : register(t1, space1);
StructuredBuffer<uint>       g_materialIDs : register(t2, space1);
StructuredBuffer<Material>   g_materials   : register(t3, space1);

// Bindless texture array (global root signature, space0)
Texture2D    g_textures[]       : register(t4, space0);
SamplerState g_samplerBilinear  : register(s0);  // Bilinear wrap (water / smooth surfaces)
SamplerState g_samplerPoint     : register(s1);  // Point/nearest wrap — OoT faithful pixel-art

// SceneConstants cbuffer is already declared in Common.hlsli at register(b0)
// No separate ConstantBuffer declaration needed.

[shader("anyhit")]
void AnyHit(inout RayPayload payload, in BuiltInTriangleIntersectionAttributes attribs) {
    uint primitiveIndex = PrimitiveIndex();
    uint matID = g_materialIDs[primitiveIndex];
    Material mat = g_materials[matID];

    // Only perform alpha test for materials flagged as alpha-tested
    if (mat.isAlphaTested == 0) return;

    // If the texture hasn't been loaded yet (index 0 = default white, index 1 = checkerboard),
    // skip alpha testing — the default white texture has alpha=1 so the test would always pass
    // anyway, but accessing it avoids potential issues with uninitialized descriptors.
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

    // Apply N64 texture wrap/clamp/mirror modes (same as ClosestHit).
    // Use the material's stored texture dimensions for exact half-texel clamping.
    uv = ApplyWrapModes(uv, mat.wrapModeS, mat.wrapModeT,
                        (float)mat.texWidthPx, (float)mat.texHeightPx);

    // Match ClosestHit sampling policy: bilinear filtering for all textures.
    float alpha = g_textures[NonUniformResourceIndex(mat.textureIndex)].SampleLevel(g_samplerBilinear, uv, 0).a;

    // Alpha test threshold: N64 RDP uses 0x80/255 ≈ 0.5 as its reference.
    // We use 0.4 to give a small margin for bilinear filtering, which slightly
    // softens alpha edges on foliage textures and could clip valid opaque regions.
    float alphaThreshold = (mat.isDecal != 0) ? 0.65 : 0.4;
    if (alpha < alphaThreshold) {
        IgnoreHit();
    }
}
