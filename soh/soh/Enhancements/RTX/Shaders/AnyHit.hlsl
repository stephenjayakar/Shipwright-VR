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
SamplerState g_samplerBilinear  : register(s0);  // Bilinear wrap
SamplerState g_samplerPoint     : register(s1);  // Point/nearest wrap (N64 pixel-art)

// SceneConstants cbuffer is already declared in Common.hlsli at register(b0)
// No separate ConstantBuffer declaration needed.

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
    // The isDecal flag is set by SceneGeometryExtractor when ZMODE_DEC is detected.
    if (mat.isDecal != 0) return;

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

    // Sample texture alpha using bilinear sampler for smooth alpha test edges at RTX resolution.
    // Bilinear filtering softens the alpha boundary slightly, producing smoother foliage edges
    // that look natural at high resolution rather than jagged pixel-art cutoffs.
    float alpha = g_textures[NonUniformResourceIndex(mat.textureIndex)]
                    .SampleLevel(g_samplerBilinear, uv, 0).a;

    // Note: dekuTreeAlpha fade is temporarily disabled in the nuclear simplified
    // cbuffer layout. It can be re-added via a padding field once baseline is stable.

    // Alpha test threshold: N64 uses 0x80/255 ≈ 0.5 as the reference value.
    // We use 0.4 instead of 0.5 to account for:
    // 1. Bilinear filtering reducing edge alpha slightly below the original 1.0
    // 2. The dekuTreeAlpha multiplication — even small reductions compound
    // 3. Some OoT textures use alpha values like 0xFE (254/255 = 0.996) for
    //    "opaque" areas that should pass the test
    // The 0.4 threshold preserves more foliage detail while still cleanly
    // cutting out the transparent background (typically alpha=0).
    if (alpha < 0.4) {
        IgnoreHit();
    }
}
