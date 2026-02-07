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

// Bindless texture array
Texture2D    g_textures[] : register(t4, space1);
SamplerState g_sampler    : register(s0);

ConstantBuffer<SceneConstants> g_constants : register(b0);

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
