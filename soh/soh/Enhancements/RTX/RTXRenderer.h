#pragma once
#ifndef RTX_RENDERER_H
#define RTX_RENDERER_H

#ifdef ENABLE_DX12_RTX

#include "DX12Device.h"
#include "DXRPipeline.h"
#include "SceneGeometryExtractor.h"
#include "AccelerationStructure.h"
#include "GISystem.h"
#include "RTXSceneConfig.h"
#include "RTXTypes.h"
#include "TextureManager.h"
#include <memory>
#include <vector>
#include <cstdint>

namespace RTX {

class RTXRenderer {
public:
    static RTXRenderer* Instance();
    static bool IsActive();

    RTXRenderer();
    ~RTXRenderer();

    // Lifecycle
    bool Initialize(HWND hwnd, uint32_t width, uint32_t height);
    void Shutdown();

    // Called from scene hooks
    void OnSceneLoaded(int sceneNum);
    void OnSceneUnload();
    void OnRoomLoaded(void* play, int roomNum);

    // Per-frame update: extract scene params from PlayState
    // (Called from func_8009E0B8 hook)
    void UpdateSceneParams(
        uint32_t gameplayFrames,
        int sceneSetupIndex,
        float roomUnk74,
        bool dekuTreeDead,
        bool isAdultLink,
        const float viewMatrix[16],
        const float projMatrix[16],
        const float cameraPos[3],
        const float ambientColor[3],
        const float fogColor[3],
        float fogNear,
        float fogFar,
        const float sunDir1[3],
        const float sunColor1[3],
        const float sunDir2[3],
        const float sunColor2[3]
    );

    // Dispatch rays and present
    // (Called from Graph_ProcessGfxCommands hook)
    void DispatchAndPresent();

    // Accessors
    DX12Device* GetDevice() { return m_device.get(); }
    DXRPipeline* GetPipeline() { return m_pipeline.get(); }
    bool IsSceneLoaded() const { return m_sceneLoaded; }

private:
    void InvertMatrix4x4(const float m[16], float out[16]);

    // Resolve raw texture address hashes in Material::textureIndex fields
    // to actual SRV descriptor heap indices from the TextureManager.
    // Called after geometry extraction and before BLAS building.
    // Materials whose textures haven't been intercepted yet get SRV index 0
    // (the default white texture), and will be re-resolved on future frames
    // as textures are intercepted via RTX_InterceptTexture.
    void ResolveMaterialTextures(RoomGeometry& geometry);

    static RTXRenderer* s_instance;

    std::unique_ptr<DX12Device> m_device;
    std::unique_ptr<DXRPipeline> m_pipeline;

    // Scene state
    bool m_sceneLoaded = false;
    int m_currentScene = -1;
    SceneConstants m_sceneConstants = {};
    SceneConfig m_currentSceneConfig = {};

    // Geometry extraction (Phase 2)
    std::unique_ptr<SceneGeometryExtractor> m_geometryExtractor;
    std::vector<RoomGeometry> m_roomGeometry;

    // Acceleration structures (Phase 3)
    std::unique_ptr<AccelerationStructure> m_accelerationStructure;

    // GI system (temporal accumulation + denoise parameters)
    // All temporal accumulation is delegated to GISystem.
    std::unique_ptr<GISystem> m_giSystem;

    // Texture manager (Phase 6)
    std::unique_ptr<TextureManager> m_textureManager;
};

} // namespace RTX

#endif // ENABLE_DX12_RTX
#endif // RTX_RENDERER_H
