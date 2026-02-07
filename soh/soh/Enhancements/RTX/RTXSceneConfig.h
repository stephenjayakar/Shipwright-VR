#pragma once
#ifndef RTX_SCENE_CONFIG_H
#define RTX_SCENE_CONFIG_H

#ifdef ENABLE_DX12_RTX

#include <cstdint>
#include <string>
#include <unordered_map>

namespace RTX {

// Scene IDs from OoT (z64scene.h).
// Only SCENE_KOKIRI_FOREST is RTX-enabled for now; the others are listed
// for future expansion and for reference by scene-transition logic.
constexpr uint16_t SCENE_KOKIRI_FOREST     = 0x55;
constexpr uint16_t SCENE_HYRULE_FIELD      = 0x51;
constexpr uint16_t SCENE_LOST_WOODS        = 0x5B;
constexpr uint16_t SCENE_SACRED_FOREST_MEADOW = 0x56;
constexpr uint16_t SCENE_KOKIRI_SHOP       = 0x2D;
constexpr uint16_t SCENE_LINKS_HOUSE       = 0x34;
constexpr uint16_t SCENE_DEKU_TREE         = 0x00;
constexpr uint16_t SCENE_TEMPLE_OF_TIME    = 0x43;

// ============================================================================
// RTXMaterialOverride — Per-texture material property overrides.
//
// Allows specifying PBR and classification properties for specific N64 textures
// identified by their OTR path substring or texture hash. This enables hardcoded
// defaults for common OoT materials (water surfaces, lava, metal, torches, etc.)
// and user-overridable material tuning.
// ============================================================================
struct RTXMaterialOverride {
    float roughness;          // Surface roughness [0, 1] (0 = mirror, 1 = fully diffuse)
    float metallic;           // Metalness [0, 1] (0 = dielectric, 1 = metal)
    float emissiveStrength;   // Emissive intensity multiplier [0, ...] (0 = no emission)
    float subsurface;         // Subsurface scattering intensity [0, 1] (for foliage, skin)
    bool  isWater;            // True if this material is a water surface
    bool  isGlass;            // True if this material is glass/transparent
    bool  isLava;             // True if this material is lava (emissive + scroll)

    RTXMaterialOverride()
        : roughness(0.5f)
        , metallic(0.0f)
        , emissiveStrength(0.0f)
        , subsurface(0.0f)
        , isWater(false)
        , isGlass(false)
        , isLava(false) {}

    RTXMaterialOverride(float r, float m, float e, float ss, bool water, bool glass, bool lava)
        : roughness(r), metallic(m), emissiveStrength(e), subsurface(ss)
        , isWater(water), isGlass(glass), isLava(lava) {}
};

// ============================================================================
// SceneLightConfig — Per-scene directional/ambient light configuration.
//
// Extracted from SceneConfig for convenience; represents the lighting state
// that the RTX renderer should use for a given scene.
// ============================================================================
struct SceneLightConfig {
    float sunDirection[3];
    float sunColor[3];
    float sunIntensity;
    float ambientColor[3];
    float ambientIntensity;
    float skyColor[3];
    float fogColor[3];
    float fogNear;
    float fogFar;
    float fogDensity;

    SceneLightConfig() : sunIntensity(1.0f), ambientIntensity(1.0f), fogNear(0.0f), fogFar(0.0f), fogDensity(1.0f) {
        sunDirection[0] = -0.5f; sunDirection[1] = 0.8f; sunDirection[2] = -0.3f;
        sunColor[0] = sunColor[1] = sunColor[2] = 1.0f;
        ambientColor[0] = ambientColor[1] = ambientColor[2] = 0.2f;
        skyColor[0] = 0.4f; skyColor[1] = 0.6f; skyColor[2] = 1.0f;
        fogColor[0] = fogColor[1] = fogColor[2] = 0.0f;
    }
};

// Per-scene RTX configuration.
// Contains sky, sun, ambient, fog, GI, material, reflection, and AO settings
// that allow fine-tuning of the raytraced output per scene.
struct SceneConfig {
    // Core settings
    bool enabled;                   // Whether RTX is enabled for this scene
    float giIntensity;              // Global illumination intensity multiplier [0, 2]
    int maxBounces;                 // Max ray recursion depth (primary + N bounces)

    // Sky settings
    float skyColor[3];              // Sky/zenith color RGB [0, 1]
    float skyHorizonColor[3];       // Sky horizon color RGB [0, 1] (blended with zenith)

    // Sun / directional light defaults
    float sunDirection[3];          // Default sun direction (normalized, pointing toward sun)
    float sunIntensity;             // Sun light intensity multiplier [0, 5]
    float sunColor[3];              // Sun light color RGB [0, 1]

    // Ambient light defaults
    float ambientColor[3];          // Default ambient color RGB [0, 1]
    float ambientIntensity;         // Ambient intensity multiplier [0, 2]

    // Fog settings (0 = use game values from LightContext)
    float fogColorDefault[3];       // Default fog color override
    float fogNearDefault;           // Default fog near distance
    float fogFarDefault;            // Default fog far distance
    float fogDensity;               // Fog density multiplier [0, 2] (for volumetric effects)

    // GI probe parameters
    float probeDensity;             // Probe grid spacing in world units (lower = more probes)
    float probeRadius;              // Probe influence radius in world units

    // Material property overrides
    float baseReflectivity;         // Scene-wide base reflectivity for non-metallic surfaces [0, 1]
    float roughnessScale;           // Multiplier for surface roughness (higher = more diffuse) [0, 2]
    float emissiveScale;            // Multiplier for emissive surfaces (torches, lava, etc.) [0, 5]

    // Reflection settings
    float reflectionQuality;        // Reflection ray budget multiplier [0, 1] (1 = full quality)
    float waterReflectivity;        // Override reflectivity for water surfaces [0, 1]
    float waterRoughness;           // Override roughness for water surfaces [0, 1]

    // Ambient occlusion
    float aoRadius;                 // Ambient occlusion sample radius in world units
    float aoIntensity;              // AO darkening intensity [0, 2]
};

// Get the RTX configuration for a given scene ID.
// Returns a config with enabled=false for unsupported scenes.
SceneConfig GetSceneConfig(uint16_t sceneId);

// Check if a scene supports RTX rendering
inline bool IsRTXScene(uint16_t sceneId) {
    return GetSceneConfig(sceneId).enabled;
}

// Get a default SceneConfig with reasonable baseline values.
// Used as a starting point for scene-specific overrides and as fallback
// when no scene-specific config is available.
SceneConfig GetDefaultConfig();

// Load the scene config for the given sceneId and set it as the current config.
// Equivalent to GetSceneConfig() + setting internal state.
// Returns the loaded config.
SceneConfig LoadSceneConfig(int sceneId);

// Save the given scene config to a file for persistence.
// configPath: file path to write to (key=value text format).
// If configPath is empty, uses a default path based on sceneId.
// Returns true on success.
bool SaveSceneConfig(const SceneConfig& config, uint16_t sceneId, const std::string& configPath = "");

// Load a scene config from a file.
// configPath: file path to read from (key=value text format).
// If the file doesn't exist or is invalid, returns the default config for sceneId.
SceneConfig LoadSceneConfigFromFile(uint16_t sceneId, const std::string& configPath = "");

// Get the current scene config (set by the most recent LoadSceneConfig call).
// Returns the default config if no scene has been loaded.
const SceneConfig& GetCurrentConfig();

// Set a manual scene config override. When active, GetCurrentConfig() returns
// this override instead of the scene-loaded config. Pass a config with
// enabled=false to clear the override and revert to the scene default.
void SetSceneOverride(const SceneConfig& overrideConfig);

// Clear any active scene override, reverting GetCurrentConfig() to the
// config set by the most recent LoadSceneConfig call.
void ClearSceneOverride();

// Apply time-of-day modulation to a SceneConfig's lighting parameters.
// timeNormalized: [0.0, 1.0] where 0.0/1.0 = midnight, 0.25 = sunrise,
//                 0.5 = noon, 0.75 = sunset.
// Adjusts GI intensity, emissive scale, fog defaults, sky color, sun intensity,
// ambient color, and AO based on time of day.
void ApplyTimeOfDay(SceneConfig& config, float timeNormalized);

// === Accessor methods for common scene config fields ===

// Get the sky color for the current config (convenience accessor).
void GetSkyColor(float outColor[3]);

// Get the sun direction for the current config (convenience accessor).
void GetSunDirection(float outDir[3]);

// Get the sun intensity for the current config.
float GetSunIntensity();

// Get the ambient color for the current config (convenience accessor).
void GetAmbientColor(float outColor[3]);

// Get the fog settings for the current config (convenience accessor).
void GetFogSettings(float outColor[3], float& outNear, float& outFar);

// Get the GI parameters for the current config (convenience accessor).
void GetGIParams(float& outIntensity, int& outMaxBounces, float& outProbeDensity);

// ============================================================================
// Material Override System
// ============================================================================

// Get a material override for a given texture name/path.
// textureName: the OTR texture path or a substring to match (e.g., "water", "lava", "metal").
// Returns a default RTXMaterialOverride if no override is found.
// The system first checks exact matches, then substring matches.
RTXMaterialOverride GetMaterialForTexture(const std::string& textureName);

// Get the scene lighting configuration for a given scene ID.
// Extracts directional light, ambient, sky, and fog settings from the SceneConfig.
SceneLightConfig GetSceneLightConfig(uint16_t sceneId);

// Load material overrides from a configuration file.
// configPath: path to a key=value config file with material overrides.
// If configPath is empty, loads from a default "rtx_materials.cfg" path.
// Returns the number of overrides loaded.
uint32_t LoadOverrides(const std::string& configPath = "");

// Register a material override for a texture name/path pattern.
// This allows runtime addition of material overrides beyond the hardcoded defaults.
void RegisterMaterialOverride(const std::string& texturePattern, const RTXMaterialOverride& override_);

// Get all registered material overrides (read-only).
const std::unordered_map<std::string, RTXMaterialOverride>& GetMaterialOverrides();

// ============================================================================
// Scene Transition Helpers
// ============================================================================

// Check if a scene transition requires RTX state changes.
// Returns true if going from prevScene to newScene requires
// either initializing or tearing down RTX rendering.
inline bool RequiresRTXTransition(uint16_t prevScene, uint16_t newScene) {
    return IsRTXScene(prevScene) != IsRTXScene(newScene);
}

// Check if a scene transition is between two RTX-enabled scenes
// (e.g., Kokiri Forest interior -> exterior). In this case, we may
// want to keep the DX12 device alive and only rebuild acceleration structures.
inline bool IsRTXToRTXTransition(uint16_t prevScene, uint16_t newScene) {
    return IsRTXScene(prevScene) && IsRTXScene(newScene);
}

} // namespace RTX

#endif // ENABLE_DX12_RTX
#endif // RTX_SCENE_CONFIG_H
