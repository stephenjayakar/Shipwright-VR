#ifdef ENABLE_DX12_RTX

#include "RTXSceneConfig.h"
#include <cmath>
#include <cstring>
#include <cstdio>
#include <algorithm>

namespace RTX {

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
    config.giIntensity = 1.0f;
    config.maxBounces = 2;

    // Sky: blue sky default
    config.skyColor[0] = 0.4f;
    config.skyColor[1] = 0.6f;
    config.skyColor[2] = 1.0f;
    config.skyHorizonColor[0] = 0.7f;
    config.skyHorizonColor[1] = 0.8f;
    config.skyHorizonColor[2] = 1.0f;

    // Sun: warm afternoon default
    // Direction pointing toward sun (upper-left-forward)
    config.sunDirection[0] = -0.5f;
    config.sunDirection[1] = 0.8f;
    config.sunDirection[2] = -0.3f;
    config.sunIntensity = 1.0f;
    config.sunColor[0] = 1.0f;
    config.sunColor[1] = 0.95f;
    config.sunColor[2] = 0.85f;

    // Ambient: soft blue-grey
    config.ambientColor[0] = 0.15f;
    config.ambientColor[1] = 0.18f;
    config.ambientColor[2] = 0.22f;
    config.ambientIntensity = 1.0f;

    // Fog: zeros mean "use game-provided values from LightContext"
    config.fogColorDefault[0] = 0.0f;
    config.fogColorDefault[1] = 0.0f;
    config.fogColorDefault[2] = 0.0f;
    config.fogNearDefault = 0.0f;
    config.fogFarDefault = 0.0f;
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

    return config;
}

// ============================================================================
// Per-Scene Configs
// ============================================================================

SceneConfig GetSceneConfig(uint16_t sceneId) {
    SceneConfig config = GetDefaultConfig();

    switch (sceneId) {
        case SCENE_KOKIRI_FOREST:
            config.enabled = true;
            config.giIntensity = 1.0f;
            config.maxBounces = 2; // primary + 1 GI bounce

            // Sky: bright green-blue, typical of Kokiri Forest's enchanted atmosphere
            config.skyColor[0] = 0.35f;
            config.skyColor[1] = 0.65f;
            config.skyColor[2] = 0.95f;
            config.skyHorizonColor[0] = 0.55f;
            config.skyHorizonColor[1] = 0.75f;
            config.skyHorizonColor[2] = 0.85f;

            // Sun: warm golden light filtering through canopy
            config.sunDirection[0] = -0.4f;
            config.sunDirection[1] = 0.85f;
            config.sunDirection[2] = -0.35f;
            config.sunIntensity = 1.2f;
            config.sunColor[0] = 1.0f;
            config.sunColor[1] = 0.92f;
            config.sunColor[2] = 0.75f;

            // Ambient: greenish from foliage bouncing
            config.ambientColor[0] = 0.12f;
            config.ambientColor[1] = 0.2f;
            config.ambientColor[2] = 0.15f;
            config.ambientIntensity = 1.0f;

            // Use game-provided fog values by default (zeros mean "use game values")
            config.fogColorDefault[0] = 0.0f;
            config.fogColorDefault[1] = 0.0f;
            config.fogColorDefault[2] = 0.0f;
            config.fogNearDefault = 0.0f;
            config.fogFarDefault = 0.0f;
            config.fogDensity = 1.0f;

            // Dense probe grid for detailed forest GI
            config.probeDensity = 150.0f;       // Tighter grid for detailed forest
            config.probeRadius = 250.0f;

            // Kokiri Forest: lush outdoor scene
            config.baseReflectivity = 0.05f;
            config.roughnessScale = 1.2f;        // Slightly rough organic surfaces
            config.emissiveScale = 0.5f;          // Minimal emissive (no lava/torches in forest)

            // Moderate reflection quality for forest water streams
            config.reflectionQuality = 0.8f;
            config.waterReflectivity = 0.5f;      // Forest streams: less mirror-like
            config.waterRoughness = 0.15f;         // Slightly rough flowing water

            // Dense vegetation benefits from stronger AO
            config.aoRadius = 60.0f;
            config.aoIntensity = 0.7f;            // Stronger AO under tree canopy
            break;

        case SCENE_HYRULE_FIELD:
            // Hyrule Field: large open outdoor area with dramatic lighting
            config.enabled = false;
            config.giIntensity = 0.8f;
            config.maxBounces = 1;

            // Sky: wide open sky
            config.skyColor[0] = 0.4f;
            config.skyColor[1] = 0.6f;
            config.skyColor[2] = 1.0f;
            config.skyHorizonColor[0] = 0.75f;
            config.skyHorizonColor[1] = 0.85f;
            config.skyHorizonColor[2] = 1.0f;

            // Sun: bright overhead
            config.sunDirection[0] = -0.3f;
            config.sunDirection[1] = 0.9f;
            config.sunDirection[2] = -0.3f;
            config.sunIntensity = 1.5f;
            config.sunColor[0] = 1.0f;
            config.sunColor[1] = 0.97f;
            config.sunColor[2] = 0.9f;

            // Ambient: neutral warm
            config.ambientColor[0] = 0.2f;
            config.ambientColor[1] = 0.2f;
            config.ambientColor[2] = 0.22f;
            config.ambientIntensity = 1.0f;

            // Sparse probes for large area
            config.probeDensity = 400.0f;
            config.probeRadius = 500.0f;

            config.baseReflectivity = 0.04f;
            config.roughnessScale = 1.0f;
            config.emissiveScale = 0.3f;

            config.reflectionQuality = 0.5f;
            config.waterReflectivity = 0.7f;
            config.waterRoughness = 0.1f;

            config.aoRadius = 40.0f;
            config.aoIntensity = 0.4f;
            break;

        case SCENE_LOST_WOODS:
            // Lost Woods: dense forest, atmospheric, mysterious
            config.enabled = false;
            config.giIntensity = 1.2f;
            config.maxBounces = 2;

            // Sky: barely visible through canopy, misty
            config.skyColor[0] = 0.3f;
            config.skyColor[1] = 0.5f;
            config.skyColor[2] = 0.4f;
            config.skyHorizonColor[0] = 0.4f;
            config.skyHorizonColor[1] = 0.55f;
            config.skyHorizonColor[2] = 0.45f;

            // Sun: diffuse, filtered through dense canopy
            config.sunDirection[0] = -0.2f;
            config.sunDirection[1] = 0.9f;
            config.sunDirection[2] = -0.4f;
            config.sunIntensity = 0.7f;
            config.sunColor[0] = 0.9f;
            config.sunColor[1] = 0.95f;
            config.sunColor[2] = 0.8f;

            // Ambient: heavy green
            config.ambientColor[0] = 0.1f;
            config.ambientColor[1] = 0.22f;
            config.ambientColor[2] = 0.12f;
            config.ambientIntensity = 1.3f;

            config.fogDensity = 1.5f;

            config.probeDensity = 120.0f;       // Dense probes for forest detail
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
            config.enabled = false;
            config.giIntensity = 0.9f;
            config.maxBounces = 2;

            config.skyColor[0] = 0.4f;
            config.skyColor[1] = 0.6f;
            config.skyColor[2] = 0.85f;
            config.skyHorizonColor[0] = 0.6f;
            config.skyHorizonColor[1] = 0.7f;
            config.skyHorizonColor[2] = 0.8f;

            config.sunDirection[0] = -0.5f;
            config.sunDirection[1] = 0.8f;
            config.sunDirection[2] = -0.3f;
            config.sunIntensity = 1.0f;
            config.sunColor[0] = 1.0f;
            config.sunColor[1] = 0.95f;
            config.sunColor[2] = 0.85f;

            config.ambientColor[0] = 0.15f;
            config.ambientColor[1] = 0.2f;
            config.ambientColor[2] = 0.15f;
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
            config.enabled = false;
            config.giIntensity = 1.3f;
            config.maxBounces = 3;

            // Interior: no real sky, use warm ceiling bounce
            config.skyColor[0] = 0.3f;
            config.skyColor[1] = 0.25f;
            config.skyColor[2] = 0.2f;
            config.skyHorizonColor[0] = 0.3f;
            config.skyHorizonColor[1] = 0.25f;
            config.skyHorizonColor[2] = 0.2f;

            // Soft warm window light
            config.sunDirection[0] = -0.6f;
            config.sunDirection[1] = 0.5f;
            config.sunDirection[2] = -0.6f;
            config.sunIntensity = 0.6f;
            config.sunColor[0] = 1.0f;
            config.sunColor[1] = 0.9f;
            config.sunColor[2] = 0.7f;

            // Warm ambient for interior
            config.ambientColor[0] = 0.25f;
            config.ambientColor[1] = 0.2f;
            config.ambientColor[2] = 0.15f;
            config.ambientIntensity = 1.2f;

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
            config.enabled = false;
            config.giIntensity = 1.2f;
            config.maxBounces = 3;

            config.skyColor[0] = 0.25f;
            config.skyColor[1] = 0.22f;
            config.skyColor[2] = 0.2f;
            config.skyHorizonColor[0] = 0.25f;
            config.skyHorizonColor[1] = 0.22f;
            config.skyHorizonColor[2] = 0.2f;

            config.sunDirection[0] = -0.3f;
            config.sunDirection[1] = 0.6f;
            config.sunDirection[2] = -0.7f;
            config.sunIntensity = 0.5f;
            config.sunColor[0] = 1.0f;
            config.sunColor[1] = 0.9f;
            config.sunColor[2] = 0.75f;

            config.ambientColor[0] = 0.22f;
            config.ambientColor[1] = 0.2f;
            config.ambientColor[2] = 0.18f;
            config.ambientIntensity = 1.1f;

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
            config.enabled = false;
            config.giIntensity = 1.1f;
            config.maxBounces = 2;

            // Dim, earthy interior
            config.skyColor[0] = 0.15f;
            config.skyColor[1] = 0.12f;
            config.skyColor[2] = 0.08f;
            config.skyHorizonColor[0] = 0.15f;
            config.skyHorizonColor[1] = 0.12f;
            config.skyHorizonColor[2] = 0.08f;

            // Subtle light from openings above
            config.sunDirection[0] = 0.0f;
            config.sunDirection[1] = 1.0f;
            config.sunDirection[2] = 0.0f;
            config.sunIntensity = 0.4f;
            config.sunColor[0] = 0.9f;
            config.sunColor[1] = 0.85f;
            config.sunColor[2] = 0.7f;

            // Dark organic ambient
            config.ambientColor[0] = 0.1f;
            config.ambientColor[1] = 0.12f;
            config.ambientColor[2] = 0.08f;
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
            config.enabled = false;
            config.giIntensity = 1.0f;
            config.maxBounces = 3;

            // Stained-glass light creates warm/cool contrast
            config.skyColor[0] = 0.2f;
            config.skyColor[1] = 0.22f;
            config.skyColor[2] = 0.35f;
            config.skyHorizonColor[0] = 0.2f;
            config.skyHorizonColor[1] = 0.22f;
            config.skyHorizonColor[2] = 0.35f;

            // Dramatic shaft of light from the front windows
            config.sunDirection[0] = 0.0f;
            config.sunDirection[1] = 0.5f;
            config.sunDirection[2] = -0.87f;
            config.sunIntensity = 1.8f;
            config.sunColor[0] = 1.0f;
            config.sunColor[1] = 0.95f;
            config.sunColor[2] = 0.8f;

            // Cool stone ambient
            config.ambientColor[0] = 0.12f;
            config.ambientColor[1] = 0.14f;
            config.ambientColor[2] = 0.2f;
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
    s_configLoaded = true;
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
    }

    fclose(f);
    return config;
}

// ============================================================================
// Current Config State
// ============================================================================

const SceneConfig& GetCurrentConfig() {
    if (s_hasOverride) {
        return s_overrideConfig;
    }
    if (!s_configLoaded) {
        // Return a static default if no scene has been loaded yet
        static SceneConfig defaultConfig = GetDefaultConfig();
        return defaultConfig;
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
    // Clamp input to [0, 1]
    if (timeNormalized < 0.0f) timeNormalized = 0.0f;
    if (timeNormalized > 1.0f) timeNormalized = 1.0f;

    // Compute a daylight factor: peaks at 0.5 (noon), zero at 0.0/1.0 (midnight).
    // Uses a raised cosine: daylight = 0.5 * (1 + cos(2*PI*(time - 0.5)))
    // This gives: midnight=0, sunrise(0.25)=0.5, noon(0.5)=1, sunset(0.75)=0.5
    constexpr float PI = 3.14159265f;
    float daylight = 0.5f * (1.0f + cosf(2.0f * PI * (timeNormalized - 0.5f)));

    // GI intensity: brighter during daytime, reduced at night
    float baseGI = config.giIntensity;
    config.giIntensity = 0.3f + daylight * (baseGI - 0.3f);

    // Sun intensity: follows daylight directly
    config.sunIntensity *= daylight;

    // Sun color: warm at sunrise/sunset, neutral at noon
    float duskDawnFactor = sinf(2.0f * PI * timeNormalized);
    duskDawnFactor = duskDawnFactor * duskDawnFactor;
    // At dusk/dawn: shift sun color toward orange/red
    config.sunColor[0] = config.sunColor[0] * (1.0f + 0.2f * duskDawnFactor);
    config.sunColor[1] = config.sunColor[1] * (1.0f - 0.1f * duskDawnFactor);
    config.sunColor[2] = config.sunColor[2] * (1.0f - 0.3f * duskDawnFactor);
    // Clamp
    if (config.sunColor[0] > 1.0f) config.sunColor[0] = 1.0f;

    // Ambient intensity: boost slightly at night to simulate moonlight
    float nightFactor = 1.0f - daylight;
    config.ambientIntensity = config.ambientIntensity * (0.6f + 0.4f * daylight);

    // Ambient color: shift toward blue at night (moonlight)
    config.ambientColor[0] *= (0.5f + 0.5f * daylight);
    config.ambientColor[2] *= (0.8f + 0.4f * nightFactor);

    // Sky color: darker at night, brighter at day
    config.skyColor[0] *= (0.1f + 0.9f * daylight);
    config.skyColor[1] *= (0.1f + 0.9f * daylight);
    config.skyColor[2] *= (0.2f + 0.8f * daylight);

    // Emissive scale: torches/lights matter more at night
    config.emissiveScale = config.emissiveScale * (1.0f + nightFactor * 2.0f);

    // AO intensity: slightly stronger during daytime (more contrast from shadows)
    config.aoIntensity = config.aoIntensity * (0.7f + 0.3f * daylight);

    // Water reflectivity: slightly higher at dusk/dawn for dramatic reflections
    float duskBoost = 0.15f * duskDawnFactor;
    config.waterReflectivity += duskBoost;
    if (config.waterReflectivity > 1.0f) config.waterReflectivity = 1.0f;
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
