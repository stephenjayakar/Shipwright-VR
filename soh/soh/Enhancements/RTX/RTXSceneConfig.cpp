#ifdef ENABLE_DX12_RTX

#include "RTXSceneConfig.h"
#include <cmath>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <fstream>
#include <chrono>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace RTX {

// ============================================================================
// Debug logging: prints constant buffer values to OutputDebugString
// Called periodically (every 100 frames) and on config load.
// ============================================================================
static uint32_t s_logFrameCounter = 0;
static uint32_t s_cbUploadLogCount = 0;  // Tracks first-3-frames CB upload logging

static void LogConstantBufferValues(const SceneConfig& config, const char* context) {
    // Pre-multiplied values that go into the constant buffer:
    // RTXRenderer.cpp sets sunColor1[R] = sunColor[R] * sunIntensity,
    // sunColor1[G] = sunColor[G] * sunIntensity, sunColor1[B] = sunColor[B] * sunIntensity.
    // When sunColor is (1,1,1), this gives (sunIntensity, sunIntensity, sunIntensity) = equal RGB.
    // ambientColor in CB = ambientColor * ambientIntensity.
    float sunGPU[3] = {
        config.sunColor[0] * config.sunIntensity,
        config.sunColor[1] * config.sunIntensity,
        config.sunColor[2] * config.sunIntensity
    };
    float ambFinal[3] = {
        config.ambientColor[0] * config.ambientIntensity,
        config.ambientColor[1] * config.ambientIntensity,
        config.ambientColor[2] * config.ambientIntensity
    };

    char buf[512];
    snprintf(buf, sizeof(buf),
        "RTX_SCENE_CONFIG: sunDir=(%.6f,%.6f,%.6f) sunColor=(%.3f,%.3f,%.3f) sunInt=%.2f GPU_sunColor1=(%.3f,%.3f,%.3f) ambient=(%.3f,%.3f,%.3f)\n",
        config.sunDirection[0], config.sunDirection[1], config.sunDirection[2],
        config.sunColor[0], config.sunColor[1], config.sunColor[2],
        config.sunIntensity,
        sunGPU[0], sunGPU[1], sunGPU[2],
        ambFinal[0], ambFinal[1], ambFinal[2]);
    OutputDebugStringA(buf);

    // Also log equality check for color cast debugging
    bool rEqG = (fabsf(sunGPU[0] - sunGPU[1]) < 0.001f);
    bool rEqB = (fabsf(sunGPU[0] - sunGPU[2]) < 0.001f);
    snprintf(buf, sizeof(buf),
        "RTX_SCENE_CONFIG: GPU_sunColor1 R==G:%s R==B:%s denoiser=%s (context=%s)\n",
        rEqG ? "YES" : "NO",
        rEqB ? "YES" : "NO",
        config.denoiserEnabled ? "ON" : "OFF",
        context);
    OutputDebugStringA(buf);
}

// OutputDebugStringA logger for constant buffer upload: prints EXACT values for first 3 frames only.
// Called from GetCurrentConfig() which is invoked each frame before CB upload.
static void LogCBUploadFirstFrames(const SceneConfig& config) {
    if (s_cbUploadLogCount >= 3) return;  // Only log first 3 frames
    s_cbUploadLogCount++;

    char buf[768];
    // Print the EXACT sun color, direction, and intensity values being sent to the GPU.
    // RTXRenderer sets sunColor1 = sunColor * sunIntensity (per-channel multiply).
    // For pure white sunColor (1,1,1), this gives (sunIntensity, sunIntensity, sunIntensity).
    float sunGPU[3] = {
        config.sunColor[0] * config.sunIntensity,
        config.sunColor[1] * config.sunIntensity,
        config.sunColor[2] * config.sunIntensity
    };
    float ambFinal[3] = {
        config.ambientColor[0] * config.ambientIntensity,
        config.ambientColor[1] * config.ambientIntensity,
        config.ambientColor[2] * config.ambientIntensity
    };

    snprintf(buf, sizeof(buf),
        "[RTX CB UPLOAD frame %u/3] "
        "sunDirection=(%.6f, %.6f, %.6f) "
        "sunColor=(%.3f, %.3f, %.3f) sunIntensity=%.3f "
        "GPU sunColor1=(%.3f, %.3f, %.3f) "
        "ambientColor=(%.3f, %.3f, %.3f) ambientIntensity=%.3f "
        "GPU ambient=(%.3f, %.3f, %.3f) "
        "denoiserEnabled=%d temporalWeight=%.3f blurRadius=%d\n",
        s_cbUploadLogCount,
        config.sunDirection[0], config.sunDirection[1], config.sunDirection[2],
        config.sunColor[0], config.sunColor[1], config.sunColor[2], config.sunIntensity,
        sunGPU[0], sunGPU[1], sunGPU[2],
        config.ambientColor[0], config.ambientColor[1], config.ambientColor[2], config.ambientIntensity,
        ambFinal[0], ambFinal[1], ambFinal[2],
        config.denoiserEnabled ? 1 : 0, config.temporalWeight, config.blurRadius);
    OutputDebugStringA(buf);
}

// Internal state for current/override scene configs
static SceneConfig s_currentConfig = {};
static SceneConfig s_overrideConfig = {};
static bool s_hasOverride = false;
static bool s_configLoaded = false;

// Material override registry: maps texture name/path pattern -> material override.
// Populated with hardcoded OoT defaults and augmented by LoadOverrides().
static std::unordered_map<std::string, RTXMaterialOverride> s_materialOverrides;
static bool s_materialOverridesInitialized = false;

// Initialize the default material overrides for common OoT textures.
// These provide reasonable PBR defaults for well-known N64 materials.
static void InitDefaultMaterialOverrides() {
    if (s_materialOverridesInitialized) return;
    s_materialOverridesInitialized = true;

    // Water surfaces: smooth, reflective, flagged as water for UV scroll
    //                                       rough  metal emiss  subsrf water glass lava
    s_materialOverrides["water"]           = {0.05f, 0.0f, 0.0f, 0.0f, true,  false, false};
    s_materialOverrides["river"]           = {0.08f, 0.0f, 0.0f, 0.0f, true,  false, false};
    s_materialOverrides["stream"]          = {0.10f, 0.0f, 0.0f, 0.0f, true,  false, false};
    s_materialOverrides["pond"]            = {0.03f, 0.0f, 0.0f, 0.0f, true,  false, false};
    s_materialOverrides["lake"]            = {0.02f, 0.0f, 0.0f, 0.0f, true,  false, false};
    s_materialOverrides["waterfall"]       = {0.20f, 0.0f, 0.0f, 0.0f, true,  false, false};

    // Lava surfaces: very rough, strongly emissive, flagged as lava
    s_materialOverrides["lava"]            = {0.80f, 0.0f, 5.0f, 0.0f, false, false, true};
    s_materialOverrides["magma"]           = {0.85f, 0.0f, 4.5f, 0.0f, false, false, true};
    s_materialOverrides["fire"]            = {0.90f, 0.0f, 6.0f, 0.0f, false, false, false};

    // Torches and fire: strong emissive
    s_materialOverrides["torch"]           = {0.70f, 0.0f, 4.0f, 0.0f, false, false, false};
    s_materialOverrides["flame"]           = {0.85f, 0.0f, 5.0f, 0.0f, false, false, false};
    s_materialOverrides["candle"]          = {0.60f, 0.0f, 3.0f, 0.0f, false, false, false};
    s_materialOverrides["lantern"]         = {0.50f, 0.0f, 2.5f, 0.0f, false, false, false};

    // Metal surfaces: low roughness, high metallic
    s_materialOverrides["metal"]           = {0.20f, 0.9f, 0.0f, 0.0f, false, false, false};
    s_materialOverrides["iron"]            = {0.30f, 0.85f, 0.0f, 0.0f, false, false, false};
    s_materialOverrides["steel"]           = {0.15f, 0.95f, 0.0f, 0.0f, false, false, false};
    s_materialOverrides["gold"]            = {0.10f, 1.0f, 0.0f, 0.0f, false, false, false};
    s_materialOverrides["chain"]           = {0.35f, 0.8f, 0.0f, 0.0f, false, false, false};
    s_materialOverrides["sword"]           = {0.12f, 0.9f, 0.0f, 0.0f, false, false, false};
    s_materialOverrides["shield"]          = {0.15f, 0.85f, 0.0f, 0.0f, false, false, false};

    // Stone/rock: rough, non-metallic
    s_materialOverrides["stone"]           = {0.75f, 0.0f, 0.0f, 0.0f, false, false, false};
    s_materialOverrides["rock"]            = {0.80f, 0.0f, 0.0f, 0.0f, false, false, false};
    s_materialOverrides["brick"]           = {0.85f, 0.0f, 0.0f, 0.0f, false, false, false};
    s_materialOverrides["wall"]            = {0.70f, 0.0f, 0.0f, 0.0f, false, false, false};

    // Wood: moderate roughness, slight subsurface
    s_materialOverrides["wood"]            = {0.65f, 0.0f, 0.0f, 0.1f, false, false, false};
    s_materialOverrides["plank"]           = {0.60f, 0.0f, 0.0f, 0.08f, false, false, false};
    s_materialOverrides["bark"]            = {0.80f, 0.0f, 0.0f, 0.15f, false, false, false};
    s_materialOverrides["tree"]            = {0.70f, 0.0f, 0.0f, 0.12f, false, false, false};

    // Foliage/vegetation: rough, subsurface for translucency
    s_materialOverrides["leaf"]            = {0.75f, 0.0f, 0.0f, 0.3f, false, false, false};
    s_materialOverrides["grass"]           = {0.80f, 0.0f, 0.0f, 0.25f, false, false, false};
    s_materialOverrides["vine"]            = {0.70f, 0.0f, 0.0f, 0.2f, false, false, false};
    s_materialOverrides["bush"]            = {0.78f, 0.0f, 0.0f, 0.28f, false, false, false};

    // Glass/crystal: very smooth, flagged as glass
    s_materialOverrides["glass"]           = {0.02f, 0.0f, 0.0f, 0.0f, false, true, false};
    s_materialOverrides["crystal"]         = {0.05f, 0.0f, 0.3f, 0.0f, false, true, false};
    s_materialOverrides["gem"]             = {0.03f, 0.0f, 0.2f, 0.0f, false, true, false};
    s_materialOverrides["window"]          = {0.02f, 0.0f, 0.0f, 0.0f, false, true, false};

    // Cloth/fabric: very rough, non-metallic
    s_materialOverrides["cloth"]           = {0.90f, 0.0f, 0.0f, 0.05f, false, false, false};
    s_materialOverrides["fabric"]          = {0.88f, 0.0f, 0.0f, 0.05f, false, false, false};
    s_materialOverrides["carpet"]          = {0.92f, 0.0f, 0.0f, 0.03f, false, false, false};

    // Dirt/ground: very rough
    s_materialOverrides["dirt"]            = {0.90f, 0.0f, 0.0f, 0.0f, false, false, false};
    s_materialOverrides["ground"]          = {0.85f, 0.0f, 0.0f, 0.0f, false, false, false};
    s_materialOverrides["sand"]            = {0.88f, 0.0f, 0.0f, 0.0f, false, false, false};

    // Kokiri Forest-specific texture patterns
    // These match common OTR path substrings for spot04 textures
    s_materialOverrides["spot04_room_0Tex_00B0A8"] = {0.05f, 0.0f, 0.0f, 0.0f, true, false, false}; // Water texture
    s_materialOverrides["spot04_room_0Tex_00B8A8"] = {0.05f, 0.0f, 0.0f, 0.0f, true, false, false}; // Water texture 2
}

// ============================================================================
// Default Config
// ============================================================================

SceneConfig GetDefaultConfig() {
    SceneConfig config = {};
    config.enabled = false;
    config.giIntensity = 0.3f;
    config.maxBounces = 2;

    // Sky: neutral blue sky default with light gray-blue horizon
    config.skyColor[0] = 0.4f;
    config.skyColor[1] = 0.6f;
    config.skyColor[2] = 1.0f;
    config.skyHorizonColor[0] = 0.7f;
    config.skyHorizonColor[1] = 0.75f;
    config.skyHorizonColor[2] = 0.8f;

    // Sun: default direction from upper-left for interesting shadow patterns.
    // normalize(0.5, 0.8, 0.3)
    config.sunDirection[0] = 0.5145f;
    config.sunDirection[1] = 0.8232f;
    config.sunDirection[2] = 0.3087f;
    config.sunIntensity = 1.8f;
    // PURE WHITE sunlight — absolutely NO warm/yellow/green cast.
    // N64 textures are naturally very warm/green (especially Kokiri Forest).
    // Any non-white sun color will amplify the green/yellow tint.
    config.sunColor[0] = 1.0f;
    config.sunColor[1] = 1.0f;
    config.sunColor[2] = 1.0f;

    // Ambient: COOL BLUE — counteract the naturally warm/green N64 textures.
    // Higher blue pushes the overall scene toward neutral.
    config.ambientColor[0] = 0.25f;
    config.ambientColor[1] = 0.30f;
    config.ambientColor[2] = 0.50f;
    config.ambientIntensity = 0.35f;

    // Fog: BLUE-gray defaults — override game values which are often yellow-green.
    // Game fog for Kokiri Forest is (0.784, 0.784, 0.588) which is very yellow.
    config.fogColorDefault[0] = 0.45f;
    config.fogColorDefault[1] = 0.55f;
    config.fogColorDefault[2] = 0.80f;
    config.fogNearDefault = 3000.0f;
    config.fogFarDefault = 12000.0f;
    config.fogDensity = 1.0f;

    // GI probe parameters
    config.probeDensity = 200.0f;       // 200 world units between probes
    config.probeRadius = 300.0f;        // 300 world unit influence radius

    // Material properties: reasonable defaults for OoT's art style
    config.baseReflectivity = 0.04f;    // Dielectric Fresnel F0 (typical for non-metals)
    config.roughnessScale = 1.0f;       // No roughness scaling
    config.emissiveScale = 1.0f;        // No emissive scaling

    // Reflection settings
    config.reflectionQuality = 1.0f;    // Full reflection quality
    config.waterReflectivity = 0.6f;    // Moderate water reflections
    config.waterRoughness = 0.1f;       // Slightly rough water for natural look

    // Ambient occlusion
    config.aoRadius = 50.0f;            // 50 OoT world units sample radius
    config.aoIntensity = 0.5f;          // Moderate AO darkening

    // Ambient minimum (indirect bounce light floor)
    config.ambientMinIntensity = 0.08f; // Low floor to preserve shadow contrast

    // Tone mapping — use ACES filmic for natural-looking HDR to LDR mapping.
    // ACES provides a pleasing S-curve that handles bright highlights gracefully.
    config.exposure = 1.2f;             // Slight overexposure for brighter outdoor scenes
    config.toneMapMode = 0;             // 0=ACES filmic tone mapping

    // Denoiser: ENABLED — temporal accumulation reduces stochastic noise from ray tracing.
    // Without this, 1-sample-per-pixel shadow and GI rays produce heavy grain.
    config.denoiserEnabled = true;      // Master denoiser switch: ON
    config.temporalWeight = 0.15f;      // Temporal accumulation alpha (0.15 = keep 85% history, add 15% new). Matches DEFAULT_BLEND_ALPHA.
    config.blurRadius = 2;              // Moderate spatial blur for remaining noise

    return config;
}

// ============================================================================
// Per-Scene Configs
// ============================================================================

SceneConfig GetSceneConfig(uint16_t sceneId) {
    SceneConfig config = GetDefaultConfig();

    switch (sceneId) {
        case SCENE_KOKIRI_FOREST:
            // ================================================================
            // Kokiri Forest (0x55): Lush forest scene.
            // The N64 textures are NATURALLY very green/olive (CI palette textures).
            // To avoid the scene looking entirely yellow-green, we use:
            //   - PURE WHITE sun (no warm bias that would amplify green)
            //   - COOL BLUE ambient (counteracts the warm/green textures)
            //   - Blue-gray fog (instead of game's yellow fog)
            //   - Lower sun intensity to avoid over-saturation through ACES
            // ================================================================
            config.enabled = true;
            config.giIntensity = 0.3f;
            config.maxBounces = 2;

            // Sky: clear blue zenith, lighter gray-blue horizon
            config.skyColor[0] = 0.4f;
            config.skyColor[1] = 0.6f;
            config.skyColor[2] = 1.0f;
            config.skyHorizonColor[0] = 0.7f;
            config.skyHorizonColor[1] = 0.75f;
            config.skyHorizonColor[2] = 0.8f;

            // Sun: upper-left direction for interesting shadow patterns
            config.sunDirection[0] = 0.5145f;
            config.sunDirection[1] = 0.8232f;
            config.sunDirection[2] = 0.3087f;
            // Reduced intensity: 1.6 instead of 2.0.
            // N64 textures have very saturated yellow/green. High intensity
            // pushes these through ACES tone mapping curve which shifts hue.
            config.sunIntensity = 1.6f;
            // PURE WHITE sun — absolutely critical.
            // Any warm tint (e.g., 1.0,0.95,0.85) amplifies the green N64 textures.
            config.sunColor[0] = 1.0f;
            config.sunColor[1] = 1.0f;
            config.sunColor[2] = 1.0f;

            // Ambient: COOL BLUE — this is the KEY to counteracting green textures.
            // The N64 textures have pixel values like (82,90,41), (57,57,33), (132,148,0).
            // These have very low blue channels. Cool blue ambient adds blue to the scene.
            config.ambientColor[0] = 0.20f;
            config.ambientColor[1] = 0.25f;
            config.ambientColor[2] = 0.50f;
            config.ambientIntensity = 0.40f;

            // Fog: DISTINCTLY BLUE — the game's fog is (200,200,150)/255 = (0.784,0.784,0.588)
            // which is very yellow. Override with blue fog to push scene toward blue.
            config.fogColorDefault[0] = 0.40f;
            config.fogColorDefault[1] = 0.50f;
            config.fogColorDefault[2] = 0.80f;
            config.fogNearDefault = 4000.0f;
            config.fogFarDefault = 15000.0f;
            config.fogDensity = 0.5f;

            // Dense probe grid for detailed forest GI
            config.probeDensity = 150.0f;
            config.probeRadius = 250.0f;

            // Kokiri Forest: lush outdoor scene
            config.baseReflectivity = 0.04f;
            config.roughnessScale = 1.1f;
            config.emissiveScale = 0.5f;

            // Forest water streams
            config.reflectionQuality = 0.8f;
            config.waterReflectivity = 0.5f;
            config.waterRoughness = 0.12f;

            // Moderate AO — adds depth and contact shadows to the forest
            config.aoRadius = 50.0f;
            config.aoIntensity = 0.40f;

            // Ambient minimum: low floor to preserve shadow contrast.
            config.ambientMinIntensity = 0.10f;

            config.exposure = 1.0f;
            config.toneMapMode = 0;  // 0=ACES filmic tone mapping
            break;

        case SCENE_HYRULE_FIELD:
            // ================================================================
            // Hyrule Field (0x51): Bright open field.
            // CRITICAL: NO green tint. Bright warm sun, cool blue ambient.
            // ================================================================
            config.enabled = true;
            config.giIntensity = 0.3f;
            config.maxBounces = 2;

            // Sky: clear vibrant blue
            config.skyColor[0] = 0.4f;
            config.skyColor[1] = 0.6f;
            config.skyColor[2] = 1.0f;
            // Horizon: light gray-blue
            config.skyHorizonColor[0] = 0.7f;
            config.skyHorizonColor[1] = 0.75f;
            config.skyHorizonColor[2] = 0.8f;

            // Sun: bright NEUTRAL daylight from upper-right
            config.sunDirection[0] = 0.3986f;
            config.sunDirection[1] = 0.8471f;
            config.sunDirection[2] = 0.3487f;
            config.sunIntensity = 1.8f;
            // PURE WHITE — no warm bias on naturally green/yellow N64 textures
            config.sunColor[0] = 1.0f;
            config.sunColor[1] = 1.0f;
            config.sunColor[2] = 1.0f;

            // Ambient: COOL BLUE — counteract warm N64 textures
            config.ambientColor[0] = 0.25f;
            config.ambientColor[1] = 0.30f;
            config.ambientColor[2] = 0.50f;
            config.ambientIntensity = 0.40f;

            // Fog: BLUE-gray — counteract warm N64 textures
            config.fogColorDefault[0] = 0.50f;
            config.fogColorDefault[1] = 0.55f;
            config.fogColorDefault[2] = 0.85f;
            config.fogNearDefault = 10000.0f;
            config.fogFarDefault = 30000.0f;
            config.fogDensity = 0.3f;

            // Sparse probes for large area
            config.probeDensity = 400.0f;
            config.probeRadius = 600.0f;

            // Grass/terrain — natural roughness
            config.baseReflectivity = 0.04f;
            config.roughnessScale = 1.0f;
            config.emissiveScale = 0.3f;

            // Small streams — moderate water
            config.reflectionQuality = 0.5f;
            config.waterReflectivity = 0.5f;
            config.waterRoughness = 0.15f;

            // Moderate AO — adds grounding to objects on the field
            config.aoRadius = 40.0f;
            config.aoIntensity = 0.35f;

            config.ambientMinIntensity = 0.12f;

            config.exposure = 1.0f;
            config.toneMapMode = 0;  // 0=ACES filmic tone mapping
            break;

        case SCENE_ZORAS_DOMAIN:
            // ================================================================
            // Zora's Domain (0x58): Cool cave with water.
            // CRITICAL: Blue-toned cave, NO green tint.
            // ================================================================
            config.enabled = true;
            config.giIntensity = 0.3f;
            config.maxBounces = 2;

            // Sky: dim cool blue cavern ceiling
            config.skyColor[0] = 0.3f;
            config.skyColor[1] = 0.4f;
            config.skyColor[2] = 0.7f;
            // Horizon: dim blue-gray
            config.skyHorizonColor[0] = 0.4f;
            config.skyHorizonColor[1] = 0.5f;
            config.skyHorizonColor[2] = 0.65f;

            // Sun: cool blue-white for cave — per task spec
            config.sunDirection[0] = 0.0f;
            config.sunDirection[1] = 0.95f;
            config.sunDirection[2] = -0.3122f;
            config.sunIntensity = 1.5f;
            config.sunColor[0] = 0.7f;
            config.sunColor[1] = 0.8f;
            config.sunColor[2] = 1.0f;

            // Ambient: deep blue cave — per task spec
            config.ambientColor[0] = 0.2f;
            config.ambientColor[1] = 0.25f;
            config.ambientColor[2] = 0.4f;
            config.ambientIntensity = 0.4f;

            // Fog: blue-gray cave fog — per task spec
            config.fogColorDefault[0] = 0.3f;
            config.fogColorDefault[1] = 0.4f;
            config.fogColorDefault[2] = 0.6f;
            config.fogNearDefault = 1200.0f;
            config.fogFarDefault = 5000.0f;
            config.fogDensity = 1.0f;

            // Dense probes for detailed cave GI
            config.probeDensity = 120.0f;
            config.probeRadius = 200.0f;

            // Wet polished rock surfaces
            config.baseReflectivity = 0.08f;
            config.roughnessScale = 0.8f;      // Wet surfaces are smoother
            config.emissiveScale = 0.5f;       // Crystal glow

            // Water: SHOWCASE — highly reflective pool
            config.reflectionQuality = 1.0f;
            config.waterReflectivity = 0.95f;  // Near-perfect reflections
            config.waterRoughness = 0.02f;     // Mirror-like still pool

            // Strong AO in cave crevices and under rock overhangs
            config.aoRadius = 70.0f;
            config.aoIntensity = 0.60f;

            // Ambient minimum: moderate
            config.ambientMinIntensity = 0.15f;

            config.exposure = 1.0f;
            config.toneMapMode = 0;  // 0=ACES filmic tone mapping
            break;

        case SCENE_ZORAS_RIVER:
            // ================================================================
            // Zora's River (0x54): Outdoor river canyon.
            // CRITICAL: NO green tint. Warm sun, cool blue ambient.
            // ================================================================
            config.enabled = true;
            config.giIntensity = 0.3f;
            config.maxBounces = 2;

            // Sky: blue sky
            config.skyColor[0] = 0.4f;
            config.skyColor[1] = 0.6f;
            config.skyColor[2] = 1.0f;
            // Horizon: light gray-blue
            config.skyHorizonColor[0] = 0.7f;
            config.skyHorizonColor[1] = 0.75f;
            config.skyHorizonColor[2] = 0.8f;

            // Sun: good daylight from above — PURE WHITE
            config.sunDirection[0] = -0.2985f;
            config.sunDirection[1] =  0.8955f;
            config.sunDirection[2] = -0.3282f;
            config.sunIntensity = 1.7f;
            // PURE WHITE — no warm bias on naturally green/yellow N64 textures
            config.sunColor[0] = 1.0f;
            config.sunColor[1] = 1.0f;
            config.sunColor[2] = 1.0f;

            // Ambient: COOL BLUE — counteract warm N64 textures
            config.ambientColor[0] = 0.20f;
            config.ambientColor[1] = 0.25f;
            config.ambientColor[2] = 0.45f;
            config.ambientIntensity = 0.35f;

            // Fog: BLUE-gray — counteract warm N64 textures
            config.fogColorDefault[0] = 0.45f;
            config.fogColorDefault[1] = 0.55f;
            config.fogColorDefault[2] = 0.80f;
            config.fogNearDefault = 3000.0f;
            config.fogFarDefault = 12000.0f;
            config.fogDensity = 0.6f;

            // Medium-density probes for winding canyon geometry
            config.probeDensity = 180.0f;
            config.probeRadius = 280.0f;

            // Natural outdoor canyon materials
            config.baseReflectivity = 0.05f;
            config.roughnessScale = 1.1f;
            config.emissiveScale = 0.3f;

            // Water: running river — good reflections but slightly rough
            config.reflectionQuality = 0.85f;
            config.waterReflectivity = 0.75f;
            config.waterRoughness = 0.08f;

            // Moderate AO for canyon walls and rock overhangs
            config.aoRadius = 55.0f;
            config.aoIntensity = 0.45f;

            // Ambient minimum: outdoor canyon has decent fill
            config.ambientMinIntensity = 0.10f;

            config.exposure = 1.0f;
            config.toneMapMode = 0;  // 0=ACES filmic tone mapping
            break;

        case SCENE_LOST_WOODS:
            // Lost Woods: dense forest, atmospheric, mysterious
            config.enabled = true;
            config.giIntensity = 1.2f;
            config.maxBounces = 2;

            // Sky: barely visible through canopy, misty
            config.skyColor[0] = 0.3f;
            config.skyColor[1] = 0.5f;
            config.skyColor[2] = 0.4f;
            config.skyHorizonColor[0] = 0.4f;
            config.skyHorizonColor[1] = 0.55f;
            config.skyHorizonColor[2] = 0.45f;

            // Sun: default direction (disabled scene — placeholder)
            config.sunDirection[0] = -0.5145f;
            config.sunDirection[1] =  0.8232f;
            config.sunDirection[2] = -0.3087f;
            config.sunIntensity = 2.0f;
            config.sunColor[0] = 1.0f;
            config.sunColor[1] = 1.0f;
            config.sunColor[2] = 1.0f;

            config.ambientColor[0] = 0.15f;
            config.ambientColor[1] = 0.18f;
            config.ambientColor[2] = 0.25f;
            config.ambientIntensity = 1.0f;

            config.fogDensity = 1.5f;

            config.probeDensity = 120.0f;
            config.probeRadius = 200.0f;

            config.baseReflectivity = 0.04f;
            config.roughnessScale = 1.3f;
            config.emissiveScale = 0.6f;

            config.reflectionQuality = 0.6f;
            config.waterReflectivity = 0.4f;
            config.waterRoughness = 0.2f;

            config.aoRadius = 70.0f;
            config.aoIntensity = 0.8f;
            break;

        case SCENE_SACRED_FOREST_MEADOW:
            // Sacred Forest Meadow: semi-open area with hedges/walls
            config.enabled = true;
            config.giIntensity = 0.9f;
            config.maxBounces = 2;

            config.skyColor[0] = 0.4f;
            config.skyColor[1] = 0.6f;
            config.skyColor[2] = 0.85f;
            config.skyHorizonColor[0] = 0.6f;
            config.skyHorizonColor[1] = 0.7f;
            config.skyHorizonColor[2] = 0.8f;

            config.sunDirection[0] = -0.5145f;
            config.sunDirection[1] =  0.8232f;
            config.sunDirection[2] = -0.3087f;
            config.sunIntensity = 2.0f;
            config.sunColor[0] = 1.0f;
            config.sunColor[1] = 1.0f;
            config.sunColor[2] = 1.0f;

            config.ambientColor[0] = 0.15f;
            config.ambientColor[1] = 0.18f;
            config.ambientColor[2] = 0.25f;
            config.ambientIntensity = 1.0f;

            config.probeDensity = 180.0f;
            config.probeRadius = 280.0f;

            config.baseReflectivity = 0.04f;
            config.roughnessScale = 1.1f;
            config.emissiveScale = 0.4f;

            config.reflectionQuality = 0.7f;
            config.waterReflectivity = 0.5f;
            config.waterRoughness = 0.15f;

            config.aoRadius = 55.0f;
            config.aoIntensity = 0.6f;
            break;

        case SCENE_LINKS_HOUSE:
            // Link's House: small indoor scene, warm lighting
            config.enabled = true;
            config.giIntensity = 1.3f;
            config.maxBounces = 3;

            // Interior: no real sky, use warm ceiling bounce
            config.skyColor[0] = 0.3f;
            config.skyColor[1] = 0.25f;
            config.skyColor[2] = 0.2f;
            config.skyHorizonColor[0] = 0.3f;
            config.skyHorizonColor[1] = 0.25f;
            config.skyHorizonColor[2] = 0.2f;

            config.sunDirection[0] = -0.5145f;
            config.sunDirection[1] =  0.8232f;
            config.sunDirection[2] = -0.3087f;
            config.sunIntensity = 2.0f;
            config.sunColor[0] = 1.0f;
            config.sunColor[1] = 1.0f;
            config.sunColor[2] = 1.0f;

            config.ambientColor[0] = 0.15f;
            config.ambientColor[1] = 0.18f;
            config.ambientColor[2] = 0.25f;
            config.ambientIntensity = 1.0f;

            // Tight probes for small room
            config.probeDensity = 80.0f;
            config.probeRadius = 120.0f;

            config.baseReflectivity = 0.04f;
            config.roughnessScale = 0.9f;
            config.emissiveScale = 1.5f;

            config.reflectionQuality = 0.8f;
            config.waterReflectivity = 0.3f;
            config.waterRoughness = 0.3f;

            config.aoRadius = 30.0f;
            config.aoIntensity = 0.9f;
            break;

        case SCENE_KOKIRI_SHOP:
            // Kokiri Shop: small interior with shelves and items
            config.enabled = true;
            config.giIntensity = 1.2f;
            config.maxBounces = 3;

            config.skyColor[0] = 0.25f;
            config.skyColor[1] = 0.22f;
            config.skyColor[2] = 0.2f;
            config.skyHorizonColor[0] = 0.25f;
            config.skyHorizonColor[1] = 0.22f;
            config.skyHorizonColor[2] = 0.2f;

            config.sunDirection[0] = -0.5145f;
            config.sunDirection[1] =  0.8232f;
            config.sunDirection[2] = -0.3087f;
            config.sunIntensity = 2.0f;
            config.sunColor[0] = 1.0f;
            config.sunColor[1] = 1.0f;
            config.sunColor[2] = 1.0f;

            config.ambientColor[0] = 0.15f;
            config.ambientColor[1] = 0.18f;
            config.ambientColor[2] = 0.25f;
            config.ambientIntensity = 1.0f;

            config.probeDensity = 60.0f;
            config.probeRadius = 100.0f;

            config.baseReflectivity = 0.05f;
            config.roughnessScale = 0.8f;
            config.emissiveScale = 1.2f;

            config.reflectionQuality = 0.9f;
            config.waterReflectivity = 0.3f;
            config.waterRoughness = 0.3f;

            config.aoRadius = 25.0f;
            config.aoIntensity = 0.8f;
            break;

        case SCENE_DEKU_TREE:
            // Inside the Deku Tree: first dungeon, organic interior
            config.enabled = true;
            config.giIntensity = 1.1f;
            config.maxBounces = 2;

            // Dim, earthy interior
            config.skyColor[0] = 0.15f;
            config.skyColor[1] = 0.12f;
            config.skyColor[2] = 0.08f;
            config.skyHorizonColor[0] = 0.15f;
            config.skyHorizonColor[1] = 0.12f;
            config.skyHorizonColor[2] = 0.08f;

            config.sunDirection[0] = -0.5145f;
            config.sunDirection[1] =  0.8232f;
            config.sunDirection[2] = -0.3087f;
            config.sunIntensity = 2.0f;
            config.sunColor[0] = 1.0f;
            config.sunColor[1] = 1.0f;
            config.sunColor[2] = 1.0f;

            config.ambientColor[0] = 0.15f;
            config.ambientColor[1] = 0.18f;
            config.ambientColor[2] = 0.25f;
            config.ambientIntensity = 1.0f;

            config.probeDensity = 100.0f;
            config.probeRadius = 150.0f;

            config.baseReflectivity = 0.03f;
            config.roughnessScale = 1.4f;
            config.emissiveScale = 0.8f;

            config.reflectionQuality = 0.5f;
            config.waterReflectivity = 0.6f;
            config.waterRoughness = 0.05f;

            config.aoRadius = 45.0f;
            config.aoIntensity = 1.0f;
            break;

        case SCENE_TEMPLE_OF_TIME:
            // Temple of Time: grand stone interior, dramatic lighting
            config.enabled = true;
            config.giIntensity = 1.0f;
            config.maxBounces = 3;

            // Stained-glass light creates warm/cool contrast
            config.skyColor[0] = 0.2f;
            config.skyColor[1] = 0.22f;
            config.skyColor[2] = 0.35f;
            config.skyHorizonColor[0] = 0.2f;
            config.skyHorizonColor[1] = 0.22f;
            config.skyHorizonColor[2] = 0.35f;

            config.sunDirection[0] = -0.5145f;
            config.sunDirection[1] =  0.8232f;
            config.sunDirection[2] = -0.3087f;
            config.sunIntensity = 2.0f;
            config.sunColor[0] = 1.0f;
            config.sunColor[1] = 1.0f;
            config.sunColor[2] = 1.0f;

            config.ambientColor[0] = 0.15f;
            config.ambientColor[1] = 0.18f;
            config.ambientColor[2] = 0.25f;
            config.ambientIntensity = 1.0f;

            // Wide probes for large interior
            config.probeDensity = 250.0f;
            config.probeRadius = 400.0f;

            config.baseReflectivity = 0.06f;
            config.roughnessScale = 0.7f;
            config.emissiveScale = 1.0f;

            config.reflectionQuality = 1.0f;
            config.waterReflectivity = 0.3f;
            config.waterRoughness = 0.3f;

            config.aoRadius = 80.0f;
            config.aoIntensity = 0.7f;
            break;

        default:
            // RTX not enabled for this scene
            break;
    }

    return config;
}

// ============================================================================
// Config Validation
// ============================================================================

// Validate and clamp a SceneConfig's values to reasonable ranges.
// This prevents corrupt config files or programming errors from causing
// rendering artifacts or crashes (e.g., negative exposure, NaN sun direction).
static void ValidateAndClampConfig(SceneConfig& config) {
    // Helper: clamp a float to a range
    auto clampf = [](float& v, float lo, float hi) {
        if (v < lo || std::isnan(v) || std::isinf(v)) v = lo;
        if (v > hi) v = hi;
    };
    // Helper: clamp a float3 component-wise
    auto clampf3 = [&clampf](float v[3], float lo, float hi) {
        clampf(v[0], lo, hi); clampf(v[1], lo, hi); clampf(v[2], lo, hi);
    };

    // Core settings
    clampf(config.giIntensity, 0.0f, 5.0f);
    // Ensure at least 1 bounce for shadow rays + GI. maxBounces=0 would mean
    // primary rays only with no shadow or GI contribution.
    if (config.maxBounces < 1) config.maxBounces = 1;
    if (config.maxBounces > 8) config.maxBounces = 8;

    // Sky colors [0, 2] (allow slight overbright for HDR sky)
    clampf3(config.skyColor, 0.0f, 2.0f);
    clampf3(config.skyHorizonColor, 0.0f, 2.0f);

    // Sun direction: must be normalizable (not zero-length).
    // Also normalize to ensure unit length — non-unit sun directions cause
    // incorrect NdotL calculations and shadow ray behavior in the shader.
    {
        float len = sqrtf(config.sunDirection[0]*config.sunDirection[0] +
                          config.sunDirection[1]*config.sunDirection[1] +
                          config.sunDirection[2]*config.sunDirection[2]);
        if (len < 0.001f || std::isnan(len) || std::isinf(len)) {
            // Reset to default sun direction: normalize(-0.5, 0.8, -0.3)
            config.sunDirection[0] = -0.5145f;
            config.sunDirection[1] =  0.8232f;
            config.sunDirection[2] = -0.3087f;
        } else if (fabsf(len - 1.0f) > 0.001f) {
            // Normalize to unit length
            float invLen = 1.0f / len;
            config.sunDirection[0] *= invLen;
            config.sunDirection[1] *= invLen;
            config.sunDirection[2] *= invLen;
        }
    }
    clampf(config.sunIntensity, 0.0f, 10.0f);
    clampf3(config.sunColor, 0.0f, 2.0f);

    // Ambient
    clampf3(config.ambientColor, 0.0f, 2.0f);
    clampf(config.ambientIntensity, 0.0f, 5.0f);

    // Fog (0 = use game values, so negatives are invalid)
    clampf3(config.fogColorDefault, 0.0f, 2.0f);
    if (config.fogNearDefault < 0.0f) config.fogNearDefault = 0.0f;
    if (config.fogFarDefault < 0.0f) config.fogFarDefault = 0.0f;
    clampf(config.fogDensity, 0.0f, 5.0f);

    // Probe parameters
    clampf(config.probeDensity, 1.0f, 10000.0f);
    clampf(config.probeRadius, 1.0f, 10000.0f);

    // Material properties
    clampf(config.baseReflectivity, 0.0f, 1.0f);
    clampf(config.roughnessScale, 0.0f, 5.0f);
    clampf(config.emissiveScale, 0.0f, 20.0f);

    // Reflection settings
    clampf(config.reflectionQuality, 0.0f, 1.0f);
    clampf(config.waterReflectivity, 0.0f, 1.0f);
    clampf(config.waterRoughness, 0.0f, 1.0f);

    // AO
    clampf(config.aoRadius, 0.0f, 1000.0f);
    clampf(config.aoIntensity, 0.0f, 3.0f);

    // Ambient minimum
    clampf(config.ambientMinIntensity, 0.0f, 1.0f);

    // Tone mapping
    clampf(config.exposure, 0.01f, 20.0f);
    if (config.toneMapMode > 2) config.toneMapMode = 0; // Default to ACES if invalid

    // Denoiser: validate settings. Temporal accumulation is essential for
    // noise reduction with stochastic ray tracing (1 sample per pixel).
    clampf(config.temporalWeight, 0.0f, 0.98f);
    if (config.blurRadius < 0) config.blurRadius = 0;
    if (config.blurRadius > 8) config.blurRadius = 8;
}

// ============================================================================
// Load / Save
// ============================================================================

SceneConfig LoadSceneConfig(int sceneId) {
    // First try to load from file (user overrides)
    SceneConfig fileConfig = LoadSceneConfigFromFile(static_cast<uint16_t>(sceneId));
    if (fileConfig.enabled) {
        s_currentConfig = fileConfig;
    } else {
        // Fall back to built-in config
        s_currentConfig = GetSceneConfig(static_cast<uint16_t>(sceneId));
    }

    // Validate and clamp all config values to prevent rendering issues from
    // corrupt config files or programming errors
    ValidateAndClampConfig(s_currentConfig);

    // Per-scene sun direction, color, intensity, and ambient are all KEPT
    // from GetSceneConfig(). Each scene has individually tuned values.

    // Ensure denoiser has reasonable defaults if not explicitly configured.
    // Temporal accumulation is critical for noise reduction.
    if (s_currentConfig.temporalWeight <= 0.0f) {
        s_currentConfig.temporalWeight = 0.85f;
    }
    if (!s_currentConfig.denoiserEnabled) {
        s_currentConfig.denoiserEnabled = true;
    }

    s_configLoaded = true;

    // Log the exact constant buffer values being uploaded
    LogConstantBufferValues(s_currentConfig, "LoadSceneConfig");

    // DIAGNOSTIC: Write scene config to file for reliable inspection
    {
        FILE* configDump = fopen("rtx_cb_dump.txt", "a");
        if (configDump) {
            fprintf(configDump, "\n====== SceneConfig Loaded (sceneId=0x%04X) ======\n", (uint16_t)sceneId);
            fprintf(configDump, "  enabled=%d  giIntensity=%.3f  maxBounces=%d\n",
                    s_currentConfig.enabled, s_currentConfig.giIntensity, s_currentConfig.maxBounces);
            fprintf(configDump, "  sunDirection=(%.4f, %.4f, %.4f)\n",
                    s_currentConfig.sunDirection[0], s_currentConfig.sunDirection[1], s_currentConfig.sunDirection[2]);
            fprintf(configDump, "  sunColor=(%.4f, %.4f, %.4f)  sunIntensity=%.4f\n",
                    s_currentConfig.sunColor[0], s_currentConfig.sunColor[1], s_currentConfig.sunColor[2],
                    s_currentConfig.sunIntensity);
            fprintf(configDump, "  ambientColor=(%.4f, %.4f, %.4f)  ambientIntensity=%.4f\n",
                    s_currentConfig.ambientColor[0], s_currentConfig.ambientColor[1], s_currentConfig.ambientColor[2],
                    s_currentConfig.ambientIntensity);
            fprintf(configDump, "  fogColor=(%.4f, %.4f, %.4f)  fogNear=%.1f  fogFar=%.1f\n",
                    s_currentConfig.fogColorDefault[0], s_currentConfig.fogColorDefault[1], s_currentConfig.fogColorDefault[2],
                    s_currentConfig.fogNearDefault, s_currentConfig.fogFarDefault);
            fprintf(configDump, "  skyColor=(%.4f, %.4f, %.4f)\n",
                    s_currentConfig.skyColor[0], s_currentConfig.skyColor[1], s_currentConfig.skyColor[2]);
            fprintf(configDump, "  exposure=%.4f  toneMapMode=%u\n", s_currentConfig.exposure, s_currentConfig.toneMapMode);
            fprintf(configDump, "  denoiserEnabled=%d  temporalWeight=%.3f  blurRadius=%d\n",
                    s_currentConfig.denoiserEnabled, s_currentConfig.temporalWeight, s_currentConfig.blurRadius);
            // Compute what the shader will receive after multiplication:
            float gpuSun[3] = {
                s_currentConfig.sunColor[0] * s_currentConfig.sunIntensity,
                s_currentConfig.sunColor[1] * s_currentConfig.sunIntensity,
                s_currentConfig.sunColor[2] * s_currentConfig.sunIntensity
            };
            float gpuAmb[3] = {
                s_currentConfig.ambientColor[0] * s_currentConfig.ambientIntensity,
                s_currentConfig.ambientColor[1] * s_currentConfig.ambientIntensity,
                s_currentConfig.ambientColor[2] * s_currentConfig.ambientIntensity
            };
            fprintf(configDump, "  GPU effective sunColor (sun * intensity): (%.4f, %.4f, %.4f)\n",
                    gpuSun[0], gpuSun[1], gpuSun[2]);
            fprintf(configDump, "  GPU effective ambient (amb * intensity): (%.4f, %.4f, %.4f)\n",
                    gpuAmb[0], gpuAmb[1], gpuAmb[2]);
            fprintf(configDump, "  NOTE: sunColor and ambientColor are sent SEPARATELY to the shader.\n");
            fprintf(configDump, "  The shader multiplies: directLight = sunColor * sunIntensity * NdotL\n");
            fprintf(configDump, "  and: ambient = ambientColor * ambientIntensity\n");
            fprintf(configDump, "====== End SceneConfig ======\n");
            fclose(configDump);
        }
    }

    return s_currentConfig;
}

bool SaveSceneConfig(const SceneConfig& config, uint16_t sceneId, const std::string& configPath) {
    std::string path = configPath;
    if (path.empty()) {
        // Generate default path based on scene ID
        char buf[256];
        snprintf(buf, sizeof(buf), "rtx_scene_%04X.cfg", sceneId);
        path = buf;
    }

    FILE* f = fopen(path.c_str(), "w");
    if (!f) {
        return false;
    }

    // Write config as simple key=value pairs for easy editing
    fprintf(f, "# RTX Scene Config for scene 0x%04X\n", sceneId);
    fprintf(f, "enabled=%d\n", config.enabled ? 1 : 0);
    fprintf(f, "giIntensity=%.3f\n", config.giIntensity);
    fprintf(f, "maxBounces=%d\n", config.maxBounces);

    fprintf(f, "skyColor=%.3f,%.3f,%.3f\n", config.skyColor[0], config.skyColor[1], config.skyColor[2]);
    fprintf(f, "skyHorizonColor=%.3f,%.3f,%.3f\n", config.skyHorizonColor[0], config.skyHorizonColor[1], config.skyHorizonColor[2]);

    fprintf(f, "sunDirection=%.3f,%.3f,%.3f\n", config.sunDirection[0], config.sunDirection[1], config.sunDirection[2]);
    fprintf(f, "sunIntensity=%.3f\n", config.sunIntensity);
    fprintf(f, "sunColor=%.3f,%.3f,%.3f\n", config.sunColor[0], config.sunColor[1], config.sunColor[2]);

    fprintf(f, "ambientColor=%.3f,%.3f,%.3f\n", config.ambientColor[0], config.ambientColor[1], config.ambientColor[2]);
    fprintf(f, "ambientIntensity=%.3f\n", config.ambientIntensity);

    fprintf(f, "fogColorDefault=%.3f,%.3f,%.3f\n", config.fogColorDefault[0], config.fogColorDefault[1], config.fogColorDefault[2]);
    fprintf(f, "fogNearDefault=%.3f\n", config.fogNearDefault);
    fprintf(f, "fogFarDefault=%.3f\n", config.fogFarDefault);
    fprintf(f, "fogDensity=%.3f\n", config.fogDensity);

    fprintf(f, "probeDensity=%.3f\n", config.probeDensity);
    fprintf(f, "probeRadius=%.3f\n", config.probeRadius);

    fprintf(f, "baseReflectivity=%.3f\n", config.baseReflectivity);
    fprintf(f, "roughnessScale=%.3f\n", config.roughnessScale);
    fprintf(f, "emissiveScale=%.3f\n", config.emissiveScale);

    fprintf(f, "reflectionQuality=%.3f\n", config.reflectionQuality);
    fprintf(f, "waterReflectivity=%.3f\n", config.waterReflectivity);
    fprintf(f, "waterRoughness=%.3f\n", config.waterRoughness);

    fprintf(f, "aoRadius=%.3f\n", config.aoRadius);
    fprintf(f, "aoIntensity=%.3f\n", config.aoIntensity);

    fprintf(f, "ambientMinIntensity=%.3f\n", config.ambientMinIntensity);
    fprintf(f, "exposure=%.3f\n", config.exposure);
    fprintf(f, "toneMapMode=%u\n", config.toneMapMode);

    fprintf(f, "denoiserEnabled=%d\n", config.denoiserEnabled ? 1 : 0);
    fprintf(f, "temporalWeight=%.3f\n", config.temporalWeight);
    fprintf(f, "blurRadius=%d\n", config.blurRadius);

    fclose(f);
    return true;
}

// Helper: parse a float value from "key=value" line
static bool ParseFloat(const char* line, const char* key, float& outValue) {
    size_t keyLen = strlen(key);
    if (strncmp(line, key, keyLen) == 0 && line[keyLen] == '=') {
        outValue = static_cast<float>(atof(line + keyLen + 1));
        return true;
    }
    return false;
}

// Helper: parse an int value from "key=value" line
static bool ParseInt(const char* line, const char* key, int& outValue) {
    size_t keyLen = strlen(key);
    if (strncmp(line, key, keyLen) == 0 && line[keyLen] == '=') {
        outValue = atoi(line + keyLen + 1);
        return true;
    }
    return false;
}

// Helper: parse a float3 from "key=x,y,z" line
static bool ParseFloat3(const char* line, const char* key, float out[3]) {
    size_t keyLen = strlen(key);
    if (strncmp(line, key, keyLen) == 0 && line[keyLen] == '=') {
        const char* values = line + keyLen + 1;
        float a = 0, b = 0, c = 0;
        if (sscanf(values, "%f,%f,%f", &a, &b, &c) == 3) {
            out[0] = a; out[1] = b; out[2] = c;
            return true;
        }
    }
    return false;
}

SceneConfig LoadSceneConfigFromFile(uint16_t sceneId, const std::string& configPath) {
    SceneConfig config = GetSceneConfig(sceneId);

    std::string path = configPath;
    if (path.empty()) {
        char buf[256];
        snprintf(buf, sizeof(buf), "rtx_scene_%04X.cfg", sceneId);
        path = buf;
    }

    FILE* f = fopen(path.c_str(), "r");
    if (!f) {
        // File doesn't exist; return default config (with enabled=false for non-RTX scenes)
        return config;
    }

    char line[512];
    int intVal = 0;
    float floatVal = 0.0f;

    while (fgets(line, sizeof(line), f)) {
        // Strip newline
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }

        // Skip comments and empty lines
        if (line[0] == '#' || line[0] == '\0') continue;

        // Parse each field
        if (ParseInt(line, "enabled", intVal)) { config.enabled = (intVal != 0); continue; }
        if (ParseFloat(line, "giIntensity", floatVal)) { config.giIntensity = floatVal; continue; }
        if (ParseInt(line, "maxBounces", intVal)) { config.maxBounces = intVal; continue; }

        if (ParseFloat3(line, "skyColor", config.skyColor)) continue;
        if (ParseFloat3(line, "skyHorizonColor", config.skyHorizonColor)) continue;
        if (ParseFloat3(line, "sunDirection", config.sunDirection)) continue;
        if (ParseFloat(line, "sunIntensity", floatVal)) { config.sunIntensity = floatVal; continue; }
        if (ParseFloat3(line, "sunColor", config.sunColor)) continue;
        if (ParseFloat3(line, "ambientColor", config.ambientColor)) continue;
        if (ParseFloat(line, "ambientIntensity", floatVal)) { config.ambientIntensity = floatVal; continue; }
        if (ParseFloat3(line, "fogColorDefault", config.fogColorDefault)) continue;
        if (ParseFloat(line, "fogNearDefault", floatVal)) { config.fogNearDefault = floatVal; continue; }
        if (ParseFloat(line, "fogFarDefault", floatVal)) { config.fogFarDefault = floatVal; continue; }
        if (ParseFloat(line, "fogDensity", floatVal)) { config.fogDensity = floatVal; continue; }
        if (ParseFloat(line, "probeDensity", floatVal)) { config.probeDensity = floatVal; continue; }
        if (ParseFloat(line, "probeRadius", floatVal)) { config.probeRadius = floatVal; continue; }
        if (ParseFloat(line, "baseReflectivity", floatVal)) { config.baseReflectivity = floatVal; continue; }
        if (ParseFloat(line, "roughnessScale", floatVal)) { config.roughnessScale = floatVal; continue; }
        if (ParseFloat(line, "emissiveScale", floatVal)) { config.emissiveScale = floatVal; continue; }
        if (ParseFloat(line, "reflectionQuality", floatVal)) { config.reflectionQuality = floatVal; continue; }
        if (ParseFloat(line, "waterReflectivity", floatVal)) { config.waterReflectivity = floatVal; continue; }
        if (ParseFloat(line, "waterRoughness", floatVal)) { config.waterRoughness = floatVal; continue; }
        if (ParseFloat(line, "aoRadius", floatVal)) { config.aoRadius = floatVal; continue; }
        if (ParseFloat(line, "aoIntensity", floatVal)) { config.aoIntensity = floatVal; continue; }
        if (ParseFloat(line, "ambientMinIntensity", floatVal)) { config.ambientMinIntensity = floatVal; continue; }
        if (ParseFloat(line, "exposure", floatVal)) { config.exposure = floatVal; continue; }
        if (ParseInt(line, "toneMapMode", intVal)) { config.toneMapMode = static_cast<uint32_t>(intVal); continue; }
        if (ParseInt(line, "denoiserEnabled", intVal)) { config.denoiserEnabled = (intVal != 0); continue; }
        if (ParseFloat(line, "temporalWeight", floatVal)) { config.temporalWeight = floatVal; continue; }
        if (ParseInt(line, "blurRadius", intVal)) { config.blurRadius = intVal; continue; }
    }

    fclose(f);

    // Validate loaded config to prevent corrupt file values from causing issues
    ValidateAndClampConfig(config);

    return config;
}

// ============================================================================
// Current Config State
// ============================================================================

const SceneConfig& GetCurrentConfig() {
    // ========================================================================
    // FILE-BASED PROOF OF EXECUTION: Write marker file for first N frames
    // to prove this code path executes and show actual config values.
    // ========================================================================
    static int s_frameWriteCount = 0;
    if (s_frameWriteCount < 5) {
        const SceneConfig& proofCfg = s_hasOverride ? s_overrideConfig : (s_configLoaded ? s_currentConfig : s_currentConfig);
        auto now = std::chrono::system_clock::now();
        auto epoch_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        std::ofstream marker("rtx_execution_proof.txt", std::ios::app);
        if (marker.is_open()) {
            marker << "SceneConfig frame " << s_frameWriteCount
                   << " timestamp_ms=" << epoch_ms
                   << " enabled=" << (proofCfg.enabled ? "true" : "false")
                   << " sunDir=(" << proofCfg.sunDirection[0] << "," << proofCfg.sunDirection[1] << "," << proofCfg.sunDirection[2] << ")"
                   << " sunColor=(" << proofCfg.sunColor[0] << "," << proofCfg.sunColor[1] << "," << proofCfg.sunColor[2] << ")"
                   << " sunIntensity=" << proofCfg.sunIntensity
                   << " ambientColor=(" << proofCfg.ambientColor[0] << "," << proofCfg.ambientColor[1] << "," << proofCfg.ambientColor[2] << ")"
                   << " ambientIntensity=" << proofCfg.ambientIntensity
                   << " denoiserEnabled=" << (proofCfg.denoiserEnabled ? "true" : "false")
                   << " toneMapMode=" << proofCfg.toneMapMode
                   << " exposure=" << proofCfg.exposure
                   << " hasOverride=" << (s_hasOverride ? "true" : "false")
                   << " configLoaded=" << (s_configLoaded ? "true" : "false")
                   << std::endl;
            marker.close();
        }
        s_frameWriteCount++;
    }

    if (s_hasOverride) {
        // Log first 3 frames for constant buffer upload debugging
        LogCBUploadFirstFrames(s_overrideConfig);
        // Log override values every 100 frames
        s_logFrameCounter++;
        if ((s_logFrameCounter % 100) == 0) {
            LogConstantBufferValues(s_overrideConfig, "GetCurrentConfig(OVERRIDE)");
        }
        return s_overrideConfig;
    }
    if (!s_configLoaded) {
        // Return a static default if no scene has been loaded yet
        static SceneConfig defaultConfig = GetDefaultConfig();
        // Log first 3 frames for constant buffer upload debugging
        LogCBUploadFirstFrames(defaultConfig);
        return defaultConfig;
    }
    // Log first 3 frames for constant buffer upload debugging
    LogCBUploadFirstFrames(s_currentConfig);
    // Log current config values every 100 frames
    s_logFrameCounter++;
    if ((s_logFrameCounter % 100) == 0) {
        LogConstantBufferValues(s_currentConfig, "GetCurrentConfig");
    }
    return s_currentConfig;
}

void SetSceneOverride(const SceneConfig& overrideConfig) {
    s_overrideConfig = overrideConfig;
    s_hasOverride = true;
}

void ClearSceneOverride() {
    s_hasOverride = false;
    s_overrideConfig = {};
}

// ============================================================================
// Time of Day
// ============================================================================

void ApplyTimeOfDay(SceneConfig& config, float timeNormalized) {
    // DISABLED: Time-of-day modulation is removed. We use ONE fixed sun direction
    // and fixed lighting for all scenes and all times. This ensures consistent,
    // compelling shadows without any time-dependent variation that could wash them out.
    //
    // The sun direction, intensity, color, and all lighting parameters are set in
    // GetDefaultConfig() / GetSceneConfig() and remain constant.
    (void)config;
    (void)timeNormalized;
}

// ============================================================================
// Convenience Accessors
// ============================================================================

void GetSkyColor(float outColor[3]) {
    const SceneConfig& cfg = GetCurrentConfig();
    outColor[0] = cfg.skyColor[0];
    outColor[1] = cfg.skyColor[1];
    outColor[2] = cfg.skyColor[2];
}

void GetSunDirection(float outDir[3]) {
    const SceneConfig& cfg = GetCurrentConfig();
    outDir[0] = cfg.sunDirection[0];
    outDir[1] = cfg.sunDirection[1];
    outDir[2] = cfg.sunDirection[2];
}

float GetSunIntensity() {
    return GetCurrentConfig().sunIntensity;
}

void GetAmbientColor(float outColor[3]) {
    const SceneConfig& cfg = GetCurrentConfig();
    outColor[0] = cfg.ambientColor[0] * cfg.ambientIntensity;
    outColor[1] = cfg.ambientColor[1] * cfg.ambientIntensity;
    outColor[2] = cfg.ambientColor[2] * cfg.ambientIntensity;
}

void GetFogSettings(float outColor[3], float& outNear, float& outFar) {
    const SceneConfig& cfg = GetCurrentConfig();
    outColor[0] = cfg.fogColorDefault[0];
    outColor[1] = cfg.fogColorDefault[1];
    outColor[2] = cfg.fogColorDefault[2];
    outNear = cfg.fogNearDefault;
    outFar = cfg.fogFarDefault;
}

void GetGIParams(float& outIntensity, int& outMaxBounces, float& outProbeDensity) {
    const SceneConfig& cfg = GetCurrentConfig();
    outIntensity = cfg.giIntensity;
    outMaxBounces = cfg.maxBounces;
    outProbeDensity = cfg.probeDensity;
}

// ============================================================================
// Material Override System
// ============================================================================

RTXMaterialOverride GetMaterialForTexture(const std::string& textureName) {
    InitDefaultMaterialOverrides();

    if (textureName.empty()) {
        return RTXMaterialOverride();
    }

    // First, try an exact match on the full texture name/path.
    auto it = s_materialOverrides.find(textureName);
    if (it != s_materialOverrides.end()) {
        return it->second;
    }

    // Convert the texture name to lowercase for case-insensitive substring matching.
    std::string lowerName = textureName;
    std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    // Try substring matching against all registered patterns.
    // This allows patterns like "water" to match "spot04_room_0Tex_water_01".
    // We prefer the longest matching pattern (most specific match).
    const RTXMaterialOverride* bestMatch = nullptr;
    size_t bestMatchLen = 0;

    for (const auto& [pattern, override_] : s_materialOverrides) {
        std::string lowerPattern = pattern;
        std::transform(lowerPattern.begin(), lowerPattern.end(), lowerPattern.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        if (lowerName.find(lowerPattern) != std::string::npos) {
            if (lowerPattern.size() > bestMatchLen) {
                bestMatchLen = lowerPattern.size();
                bestMatch = &override_;
            }
        }
    }

    if (bestMatch) {
        return *bestMatch;
    }

    // No match found; return default material properties
    return RTXMaterialOverride();
}

SceneLightConfig GetSceneLightConfig(uint16_t sceneId) {
    SceneConfig cfg = GetSceneConfig(sceneId);
    SceneLightConfig lightCfg;

    lightCfg.sunDirection[0] = cfg.sunDirection[0];
    lightCfg.sunDirection[1] = cfg.sunDirection[1];
    lightCfg.sunDirection[2] = cfg.sunDirection[2];
    lightCfg.sunColor[0] = cfg.sunColor[0];
    lightCfg.sunColor[1] = cfg.sunColor[1];
    lightCfg.sunColor[2] = cfg.sunColor[2];
    lightCfg.sunIntensity = cfg.sunIntensity;
    lightCfg.ambientColor[0] = cfg.ambientColor[0];
    lightCfg.ambientColor[1] = cfg.ambientColor[1];
    lightCfg.ambientColor[2] = cfg.ambientColor[2];
    lightCfg.ambientIntensity = cfg.ambientIntensity;
    lightCfg.skyColor[0] = cfg.skyColor[0];
    lightCfg.skyColor[1] = cfg.skyColor[1];
    lightCfg.skyColor[2] = cfg.skyColor[2];
    lightCfg.fogColor[0] = cfg.fogColorDefault[0];
    lightCfg.fogColor[1] = cfg.fogColorDefault[1];
    lightCfg.fogColor[2] = cfg.fogColorDefault[2];
    lightCfg.fogNear = cfg.fogNearDefault;
    lightCfg.fogFar = cfg.fogFarDefault;
    lightCfg.fogDensity = cfg.fogDensity;

    return lightCfg;
}

uint32_t LoadOverrides(const std::string& configPath) {
    InitDefaultMaterialOverrides();

    std::string path = configPath;
    if (path.empty()) {
        path = "rtx_materials.cfg";
    }

    FILE* f = fopen(path.c_str(), "r");
    if (!f) {
        // File doesn't exist; hardcoded defaults are already loaded
        return 0;
    }

    uint32_t count = 0;
    char line[1024];

    while (fgets(line, sizeof(line), f)) {
        // Strip newline
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }

        // Skip comments and empty lines
        if (line[0] == '#' || line[0] == '\0') continue;

        // Expected format: textureName=roughness,metallic,emissive,subsurface,isWater,isGlass,isLava
        // Example: water=0.05,0.0,0.0,0.0,1,0,0
        char* eq = strchr(line, '=');
        if (!eq) continue;

        *eq = '\0';
        const char* key = line;
        const char* values = eq + 1;

        float roughness = 0.5f, metallic = 0.0f, emissive = 0.0f, subsurface = 0.0f;
        int isWater = 0, isGlass = 0, isLava = 0;

        int parsed = sscanf(values, "%f,%f,%f,%f,%d,%d,%d",
            &roughness, &metallic, &emissive, &subsurface, &isWater, &isGlass, &isLava);

        if (parsed >= 4) {
            RTXMaterialOverride override_;
            override_.roughness = roughness;
            override_.metallic = metallic;
            override_.emissiveStrength = emissive;
            override_.subsurface = subsurface;
            override_.isWater = (isWater != 0);
            override_.isGlass = (isGlass != 0);
            override_.isLava = (isLava != 0);

            s_materialOverrides[std::string(key)] = override_;
            count++;
        }
    }

    fclose(f);
    return count;
}

void RegisterMaterialOverride(const std::string& texturePattern, const RTXMaterialOverride& override_) {
    InitDefaultMaterialOverrides();
    s_materialOverrides[texturePattern] = override_;
}

const std::unordered_map<std::string, RTXMaterialOverride>& GetMaterialOverrides() {
    InitDefaultMaterialOverrides();
    return s_materialOverrides;
}

} // namespace RTX

#endif // ENABLE_DX12_RTX
