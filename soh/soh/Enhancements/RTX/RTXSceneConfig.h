#pragma once
#ifndef RTX_SCENE_CONFIG_H
#define RTX_SCENE_CONFIG_H

#ifdef ENABLE_DX12_RTX

#include <cstdint>
#include <string>
#include <unordered_map>

namespace RTX {

// Scene IDs from OoT (z64scene.h) — all scenes.
constexpr uint16_t SCENE_DEKU_TREE                          = 0x00;
constexpr uint16_t SCENE_DODONGOS_CAVERN                    = 0x01;
constexpr uint16_t SCENE_JABU_JABU                          = 0x02;
constexpr uint16_t SCENE_FOREST_TEMPLE                      = 0x03;
constexpr uint16_t SCENE_FIRE_TEMPLE                        = 0x04;
constexpr uint16_t SCENE_WATER_TEMPLE                       = 0x05;
constexpr uint16_t SCENE_SPIRIT_TEMPLE                      = 0x06;
constexpr uint16_t SCENE_SHADOW_TEMPLE                      = 0x07;
constexpr uint16_t SCENE_BOTTOM_OF_THE_WELL                 = 0x08;
constexpr uint16_t SCENE_ICE_CAVERN                         = 0x09;
constexpr uint16_t SCENE_GANONS_TOWER                       = 0x0A;
constexpr uint16_t SCENE_GERUDO_TRAINING_GROUND             = 0x0B;
constexpr uint16_t SCENE_THIEVES_HIDEOUT                    = 0x0C;
constexpr uint16_t SCENE_INSIDE_GANONS_CASTLE               = 0x0D;
constexpr uint16_t SCENE_GANONS_TOWER_COLLAPSE_INTERIOR     = 0x0E;
constexpr uint16_t SCENE_INSIDE_GANONS_CASTLE_COLLAPSE      = 0x0F;
constexpr uint16_t SCENE_TREASURE_BOX_SHOP                  = 0x10;
constexpr uint16_t SCENE_DEKU_TREE_BOSS                     = 0x11;
constexpr uint16_t SCENE_DODONGOS_CAVERN_BOSS               = 0x12;
constexpr uint16_t SCENE_JABU_JABU_BOSS                     = 0x13;
constexpr uint16_t SCENE_FOREST_TEMPLE_BOSS                 = 0x14;
constexpr uint16_t SCENE_FIRE_TEMPLE_BOSS                   = 0x15;
constexpr uint16_t SCENE_WATER_TEMPLE_BOSS                  = 0x16;
constexpr uint16_t SCENE_SPIRIT_TEMPLE_BOSS                 = 0x17;
constexpr uint16_t SCENE_SHADOW_TEMPLE_BOSS                 = 0x18;
constexpr uint16_t SCENE_GANONDORF_BOSS                     = 0x19;
constexpr uint16_t SCENE_GANONS_TOWER_COLLAPSE_EXTERIOR     = 0x1A;
constexpr uint16_t SCENE_MARKET_ENTRANCE_DAY                = 0x1B;
constexpr uint16_t SCENE_MARKET_ENTRANCE_NIGHT              = 0x1C;
constexpr uint16_t SCENE_MARKET_ENTRANCE_RUINS              = 0x1D;
constexpr uint16_t SCENE_BACK_ALLEY_DAY                     = 0x1E;
constexpr uint16_t SCENE_BACK_ALLEY_NIGHT                   = 0x1F;
constexpr uint16_t SCENE_MARKET_DAY                         = 0x20;
constexpr uint16_t SCENE_MARKET_NIGHT                       = 0x21;
constexpr uint16_t SCENE_MARKET_RUINS                       = 0x22;
constexpr uint16_t SCENE_TEMPLE_OF_TIME_EXTERIOR_DAY        = 0x23;
constexpr uint16_t SCENE_TEMPLE_OF_TIME_EXTERIOR_NIGHT      = 0x24;
constexpr uint16_t SCENE_TEMPLE_OF_TIME_EXTERIOR_RUINS      = 0x25;
constexpr uint16_t SCENE_KNOW_IT_ALL_BROS_HOUSE             = 0x26;
constexpr uint16_t SCENE_TWINS_HOUSE                        = 0x27;
constexpr uint16_t SCENE_MIDOS_HOUSE                        = 0x28;
constexpr uint16_t SCENE_SARIAS_HOUSE                       = 0x29;
constexpr uint16_t SCENE_KAKARIKO_CENTER_GUEST_HOUSE        = 0x2A;
constexpr uint16_t SCENE_BACK_ALLEY_HOUSE                   = 0x2B;
constexpr uint16_t SCENE_BAZAAR                             = 0x2C;
constexpr uint16_t SCENE_KOKIRI_SHOP                        = 0x2D;
constexpr uint16_t SCENE_GORON_SHOP                         = 0x2E;
constexpr uint16_t SCENE_ZORA_SHOP                          = 0x2F;
constexpr uint16_t SCENE_POTION_SHOP_KAKARIKO               = 0x30;
constexpr uint16_t SCENE_POTION_SHOP_MARKET                 = 0x31;
constexpr uint16_t SCENE_BOMBCHU_SHOP                       = 0x32;
constexpr uint16_t SCENE_HAPPY_MASK_SHOP                    = 0x33;
constexpr uint16_t SCENE_LINKS_HOUSE                        = 0x34;
constexpr uint16_t SCENE_DOG_LADY_HOUSE                     = 0x35;
constexpr uint16_t SCENE_STABLE                             = 0x36;
constexpr uint16_t SCENE_IMPAS_HOUSE                        = 0x37;
constexpr uint16_t SCENE_LAKESIDE_LABORATORY                = 0x38;
constexpr uint16_t SCENE_CARPENTERS_TENT                    = 0x39;
constexpr uint16_t SCENE_GRAVEKEEPERS_HUT                   = 0x3A;
constexpr uint16_t SCENE_GREAT_FAIRYS_FOUNTAIN_MAGIC        = 0x3B;
constexpr uint16_t SCENE_FAIRYS_FOUNTAIN                    = 0x3C;
constexpr uint16_t SCENE_GREAT_FAIRYS_FOUNTAIN_SPELLS       = 0x3D;
constexpr uint16_t SCENE_GROTTOS                            = 0x3E;
constexpr uint16_t SCENE_REDEAD_GRAVE                       = 0x3F;
constexpr uint16_t SCENE_GRAVE_WITH_FAIRYS_FOUNTAIN         = 0x40;
constexpr uint16_t SCENE_ROYAL_FAMILYS_TOMB                 = 0x41;
constexpr uint16_t SCENE_SHOOTING_GALLERY                   = 0x42;
constexpr uint16_t SCENE_TEMPLE_OF_TIME                     = 0x43;
constexpr uint16_t SCENE_CHAMBER_OF_THE_SAGES               = 0x44;
constexpr uint16_t SCENE_CASTLE_COURTYARD_GUARDS_DAY        = 0x45;
constexpr uint16_t SCENE_CASTLE_COURTYARD_GUARDS_NIGHT      = 0x46;
constexpr uint16_t SCENE_CUTSCENE_MAP                       = 0x47;
constexpr uint16_t SCENE_WINDMILL_AND_DAMPES_GRAVE          = 0x48;
constexpr uint16_t SCENE_FISHING_POND                       = 0x49;
constexpr uint16_t SCENE_CASTLE_COURTYARD_ZELDA             = 0x4A;
constexpr uint16_t SCENE_BOMBCHU_BOWLING_ALLEY              = 0x4B;
constexpr uint16_t SCENE_LON_LON_BUILDINGS                  = 0x4C;
constexpr uint16_t SCENE_MARKET_GUARD_HOUSE                 = 0x4D;
constexpr uint16_t SCENE_POTION_SHOP_GRANNY                 = 0x4E;
constexpr uint16_t SCENE_GANON_BOSS                         = 0x4F;
constexpr uint16_t SCENE_HOUSE_OF_SKULLTULA                 = 0x50;
constexpr uint16_t SCENE_HYRULE_FIELD                       = 0x51;
constexpr uint16_t SCENE_KAKARIKO_VILLAGE                   = 0x52;
constexpr uint16_t SCENE_GRAVEYARD                          = 0x53;
constexpr uint16_t SCENE_ZORAS_RIVER                        = 0x54;
constexpr uint16_t SCENE_KOKIRI_FOREST                      = 0x55;
constexpr uint16_t SCENE_SACRED_FOREST_MEADOW               = 0x56;
constexpr uint16_t SCENE_LAKE_HYLIA                         = 0x57;
constexpr uint16_t SCENE_ZORAS_DOMAIN                       = 0x58;
constexpr uint16_t SCENE_ZORAS_FOUNTAIN                     = 0x59;
constexpr uint16_t SCENE_GERUDO_VALLEY                      = 0x5A;
constexpr uint16_t SCENE_LOST_WOODS                         = 0x5B;
constexpr uint16_t SCENE_DESERT_COLOSSUS                    = 0x5C;
constexpr uint16_t SCENE_GERUDOS_FORTRESS                   = 0x5D;
constexpr uint16_t SCENE_HAUNTED_WASTELAND                  = 0x5E;
constexpr uint16_t SCENE_HYRULE_CASTLE                      = 0x5F;
constexpr uint16_t SCENE_DEATH_MOUNTAIN_TRAIL               = 0x60;
constexpr uint16_t SCENE_DEATH_MOUNTAIN_CRATER              = 0x61;
constexpr uint16_t SCENE_GORON_CITY                         = 0x62;
constexpr uint16_t SCENE_LON_LON_RANCH                      = 0x63;
constexpr uint16_t SCENE_OUTSIDE_GANONS_CASTLE              = 0x64;
constexpr uint16_t SCENE_HAIRAL_NIWA2                       = 0x6B;

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

    SceneLightConfig() : sunIntensity(1.8f), ambientIntensity(0.35f), fogNear(0.0f), fogFar(0.0f), fogDensity(1.0f) {
        // Default sun direction: upper-left for good shadow patterns
        sunDirection[0] = 0.5145f; sunDirection[1] = 0.8232f; sunDirection[2] = 0.3087f;
        // PURE WHITE daylight — no warm bias that amplifies green N64 textures
        sunColor[0] = 1.0f; sunColor[1] = 1.0f; sunColor[2] = 1.0f;
        // COOL BLUE ambient — counteract naturally warm/green N64 textures
        ambientColor[0] = 0.25f; ambientColor[1] = 0.30f; ambientColor[2] = 0.50f;
        skyColor[0] = 0.40f; skyColor[1] = 0.60f; skyColor[2] = 1.0f;
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

    // Ambient minimum (indirect bounce light floor)
    // Prevents fully black shadows. Represents minimum indirect bounce light.
    // Typical range: 0.05–0.15. Higher = brighter shadow fill.
    float ambientMinIntensity;      // Minimum ambient/indirect light intensity [0, 0.3]

    // Tone mapping parameters (PostProcess is pure passthrough — these are for reference only)
    float exposure;                  // Exposure multiplier (default 1.0, neutral — no adjustment)
    uint32_t toneMapMode;           // 0=ACES, 1=Reinhard, 2=Linear (PostProcess forced to 2=Linear)

    // Denoiser settings — ENABLED by default for noise reduction.
    // Temporal accumulation is essential for stochastic ray tracing (1 sample/pixel).
    bool denoiserEnabled;           // Master denoiser enable flag (true = ENABLED)
    float temporalWeight;           // Temporal accumulation blend weight (0.85 = keep 85% history)
    int blurRadius;                 // Spatial blur/filter radius in pixels (2 = moderate)
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
