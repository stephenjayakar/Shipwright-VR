// ============================================================================
// Composite.hlsl - UI/HUD Compositing Compute Shader
//
// Blends a 2D UI overlay (hearts, rupees, minimap, text, menus) on top of
// the raytraced 3D scene. The UI texture is RGBA8 where alpha=0 means
// fully transparent (show RTX scene) and alpha=1 means fully opaque
// (show UI element).
//
// This shader runs as the final pass after tone mapping and before present.
// ============================================================================

RWTexture2D<float4> g_rtxScene  : register(u0);  // RTX post-processed scene (read/write)
Texture2D<float4>   g_uiOverlay : register(t0);  // UI overlay texture (RGBA8, sRGB)

cbuffer CompositeConstants : register(b0) {
    uint2  resolution;       // Output resolution (width, height)
    float  uiOpacity;        // Global UI opacity multiplier (0.0 - 1.0)
    uint   flags;            // Bit flags: bit 0 = UI enabled, bit 1 = debug outline
};

// sRGB to linear conversion for the UI texture (which is in sRGB space)
float SRGBToLinear(float srgb) {
    if (srgb <= 0.04045)
        return srgb / 12.92;
    else
        return pow((srgb + 0.055) / 1.055, 2.4);
}

float3 SRGBToLinear3(float3 srgb) {
    return float3(
        SRGBToLinear(srgb.r),
        SRGBToLinear(srgb.g),
        SRGBToLinear(srgb.b)
    );
}

[numthreads(8, 8, 1)]
void Composite(uint3 DTid : SV_DispatchThreadID) {
    if (DTid.x >= resolution.x || DTid.y >= resolution.y)
        return;

    // Check if UI compositing is enabled
    if ((flags & 1) == 0)
        return;

    // Read the RTX scene pixel (already tone-mapped and in sRGB space)
    float4 sceneColor = g_rtxScene[DTid.xy];

    // Read the UI overlay pixel using Load() (Texture2D doesn't support [] indexing)
    float4 uiColor = g_uiOverlay.Load(int3(DTid.xy, 0));

    // Apply global opacity to UI alpha
    float uiAlpha = uiColor.a * uiOpacity;

    // Skip blending if UI pixel is fully transparent
    if (uiAlpha < 0.004) // ~1/255 threshold
        return;

    // Alpha-blend UI over scene: result = ui * alpha + scene * (1 - alpha)
    float3 blended = uiColor.rgb * uiAlpha + sceneColor.rgb * (1.0 - uiAlpha);

    g_rtxScene[DTid.xy] = float4(blended, 1.0);
}
