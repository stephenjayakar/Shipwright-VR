#ifndef RTX_SCENE_CONFIG_H
#define RTX_SCENE_CONFIG_H

#ifdef ENABLE_DX12_RTX

#include <cstdint>

namespace RTX {

// Scene IDs that have RTX support
constexpr uint16_t SCENE_KOKIRI_FOREST = 0x55;

// Per-scene RTX configuration
struct SceneConfig {
    bool enabled;           // Whether RTX is enabled for this scene
    float giIntensity;      // Global illumination intensity multiplier
    int maxBounces;         // Max ray recursion depth
    float fogColorDefault[3];   // Default fog color override (0 = use game values)
    float fogNearDefault;       // Default fog near (0 = use game values)
    float fogFarDefault;        // Default fog far (0 = use game values)
};

// Get the RTX configuration for a given scene ID.
// Returns a config with enabled=false for unsupported scenes.
SceneConfig GetSceneConfig(uint16_t sceneId);

// Check if a scene supports RTX rendering
inline bool IsRTXScene(uint16_t sceneId) {
    return GetSceneConfig(sceneId).enabled;
}

} // namespace RTX

#endif // ENABLE_DX12_RTX
#endif // RTX_SCENE_CONFIG_H
