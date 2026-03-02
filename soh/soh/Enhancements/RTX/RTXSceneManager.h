#pragma once
#ifndef RTX_SCENE_MANAGER_H
#define RTX_SCENE_MANAGER_H

#ifdef ENABLE_DX12_RTX

#include "RTXTypes.h"
#include "RTXSceneConfig.h"
#include "SceneGeometryExtractor.h"
#include "AccelerationStructure.h"
#include "TextureManager.h"
#include <memory>
#include <vector>
#include <cstdint>

namespace RTX {

/**
 * RTXSceneManager - Manages scene lifecycle for the RTX renderer.
 *
 * Centralizes scene/room loading, geometry extraction, BLAS/TLAS building,
 * and per-scene configuration.  Extracted from RTXRenderer to keep the
 * renderer class focused on dispatch and present.
 */
class RTXSceneManager {
public:
    RTXSceneManager();
    ~RTXSceneManager();

    bool Initialize(class DX12Device* device);
    void Shutdown();

    // Scene lifecycle
    void OnSceneLoaded(int sceneNum);
    void OnSceneUnload();
    void OnRoomLoaded(void* play, int roomNum);

    // Accessors
    bool IsSceneLoaded() const { return m_sceneLoaded; }
    int GetCurrentScene() const { return m_currentScene; }
    const SceneConfig& GetCurrentSceneConfig() const { return m_currentSceneConfig; }
    AccelerationStructure* GetAccelerationStructure() { return m_accelerationStructure.get(); }
    const std::vector<RoomGeometry>& GetRoomGeometry() const { return m_roomGeometry; }

private:
    void ResolveMaterialTextures(RoomGeometry& geometry);

    DX12Device* m_device = nullptr;
    bool m_sceneLoaded = false;
    int m_currentScene = -1;
    SceneConfig m_currentSceneConfig = {};

    std::unique_ptr<SceneGeometryExtractor> m_geometryExtractor;
    std::unique_ptr<AccelerationStructure> m_accelerationStructure;
    std::vector<RoomGeometry> m_roomGeometry;
};

} // namespace RTX

#endif // ENABLE_DX12_RTX
#endif // RTX_SCENE_MANAGER_H
