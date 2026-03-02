#pragma once
#ifndef RTX_RENDERER_H
#define RTX_RENDERER_H

#ifdef ENABLE_DX12_RTX

#include "DX12Device.h"
#include "DXRPipeline.h"
#include "SceneGeometryExtractor.h"
#include "AccelerationStructure.h"
#include "GISystem.h"
#include "UICompositor.h"
#include "RTXSceneConfig.h"
#include "RTXTypes.h"
#include "TextureManager.h"
#include "RTXScreenCapture.h"
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

    // Two-phase initialization for safe DX11->DX12 handoff.
    // ProbeRTXSupport() checks if the system supports DX12 + DXR without
    // touching the DX11 swap chain. Returns true if RTX is possible.
    bool ProbeRTXSupport();
    // CompleteInitialization() finishes setup after DX11 swap chain is released.
    bool CompleteInitialization(HWND hwnd, uint32_t width, uint32_t height);

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
    
    // Bridge-mode rendering: called from DX12Bridge.cpp via gfx_dxgi's Present path.
    // Renders the frame using DX12 into the shared swap chain's back buffer and presents.
    // In diagnostic mode, just clears to magenta to prove DX12 is presenting.
    void RenderAndPresentFrame();
    
    // Complete initialization using the shared swap chain from the bridge.
    // Called after SetSharedSwapChain() has been called on DX12Device.
    bool CompleteInitializationFromBridge();

    // Upload UI overlay pixel data for compositing over the RTX output.
    // Called from Graph_ProcessGfxCommands after the Fast3D interpreter renders
    // UI elements to an offscreen framebuffer. pixelData is RGBA8, may be nullptr.
    void UploadUIOverlay(const uint8_t* pixelData, uint32_t width, uint32_t height);

    // Get the UICompositor (for external access, e.g., from hooks)
    UICompositor* GetUICompositor() { return m_uiCompositor.get(); }

    // Accessors
    DX12Device* GetDevice() { return m_device.get(); }
    DXRPipeline* GetPipeline() { return m_pipeline.get(); }
    bool IsSceneLoaded() const { return m_sceneLoaded; }
    int GetCurrentScene() const { return m_currentScene; }
    bool IsRTXSceneActive() const;

    // Proactively load all rooms in the current scene at once.
    // OoT normally loads rooms on-demand (1 at a time), but for RTX we want
    // the entire scene's geometry in the TLAS from the start.
    void LoadAllRoomsProactively(void* playPtr);

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
    bool m_allRoomsLoaded = false;  // True after proactive all-room loading completes
    int m_currentScene = -1;
    SceneConstants m_sceneConstants = {};
    SceneConfig m_currentSceneConfig = {};
    bool m_sceneConstantsValid = false;  // True after UpdateSceneParams() called at least once

    // Geometry extraction (Phase 2)
    std::unique_ptr<SceneGeometryExtractor> m_geometryExtractor;
    std::vector<RoomGeometry> m_roomGeometry;

    // Acceleration structures (Phase 3)
    std::unique_ptr<AccelerationStructure> m_accelerationStructure;

    // GI system (temporal accumulation + denoise parameters)
    // All temporal accumulation is delegated to GISystem.
    std::unique_ptr<GISystem> m_giSystem;

    // UI compositor for overlaying game HUD on RTX output
    std::unique_ptr<UICompositor> m_uiCompositor;

    // Pending UI frame data for upload during next DispatchAndPresent
    std::vector<uint8_t> m_pendingUIData;
    uint32_t m_pendingUIWidth = 0;
    uint32_t m_pendingUIHeight = 0;
    bool m_hasPendingUI = false;

    // Texture re-resolve counter. Tracks how many frames since the current scene
    // loaded, for aggressive texture re-resolution scheduling. Reset to 0 on
    // scene load so new scenes get immediate texture resolution instead of
    // waiting for the periodic 30-frame cycle from a previous scene's counter.
    uint32_t m_reResolveCounter = 0;

    // Debug visualization mode (toggled by F9 key):
    //   0 = normal rendering (default)
    //   1 = albedo only (raw texture * vertex color, no lighting)
    //   2 = normals only (world-space normals as RGB)
    //   3 = lighting only (sun + ambient contribution without texture)
    // Stored in SceneConstants.debugMode (b0 cbuffer) and PostProcessConstants.debugMode field.
    // Debug mode: 0=normal rendering, 1=albedo, 2=normals, 3=lighting, 4=depth
    // Set to 0 for standard rendering. Toggle with F9 at runtime.
    int m_debugMode = 0;
};

} // namespace RTX

#endif // ENABLE_DX12_RTX
#endif // RTX_RENDERER_H
