#ifdef ENABLE_DX12_RTX

#include "RTXSceneConfig.h"

namespace RTX {

SceneConfig GetSceneConfig(uint16_t sceneId) {
    SceneConfig config = {};
    config.enabled = false;
    config.giIntensity = 1.0f;
    config.maxBounces = 2;

    switch (sceneId) {
        case SCENE_KOKIRI_FOREST:
            config.enabled = true;
            config.giIntensity = 1.0f;
            config.maxBounces = 2; // primary + 1 GI bounce
            // Use game-provided fog values by default (zeros mean "use game values")
            config.fogColorDefault[0] = 0.0f;
            config.fogColorDefault[1] = 0.0f;
            config.fogColorDefault[2] = 0.0f;
            config.fogNearDefault = 0.0f;
            config.fogFarDefault = 0.0f;
            break;

        default:
            // RTX not enabled for this scene
            break;
    }

    return config;
}

} // namespace RTX

#endif // ENABLE_DX12_RTX
