#ifdef ENABLE_DX12_RTX

#include "RTXRenderer.h"
#include "RTXSceneConfig.h"
#include "TextureManager.h"
#include <spdlog/spdlog.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#ifdef _WIN32
#include <Windows.h>
#endif

#include "RTXDiagLog.h"

// These headers have their own extern "C" guards internally.
#include "global.h"
#include "z64.h"

// Forward declarations for OTR resource loading functions used by texture resolution.
extern "C" {
    char* ResourceMgr_LoadTexOrDListByName(const char* filePath);
    uint16_t ResourceMgr_LoadTexWidthByName(char* texPath);
    uint16_t ResourceMgr_LoadTexHeightByName(char* texPath);
    size_t ResourceGetTexSizeByName(const char* name);
}

namespace RTX {

RTXRenderer* RTXRenderer::s_instance = nullptr;

RTXRenderer* RTXRenderer::Instance() {
    if (!s_instance) {
        RTX_DIAG("RTXRenderer::Instance() creating new RTXRenderer singleton");
        s_instance = new RTXRenderer();
        RTX_DIAG("RTXRenderer::Instance() singleton created at %p", (void*)s_instance);
    }
    return s_instance;
}

bool RTXRenderer::IsActive() {
    // RTX is active once the DX12 device is initialized, regardless of scene.
    // For non-RTX scenes we still present via DX12 (simple clear screen).
    bool result = s_instance && s_instance->m_device && s_instance->m_device->IsInitialized();
    static int s_isActiveCallCount = 0;
    static bool s_lastResult = false;
    s_isActiveCallCount++;
    if (result != s_lastResult || s_isActiveCallCount <= 3) {
        RTX_DIAG("RTXRenderer::IsActive() = %s (instance=%p, device=%p, deviceInit=%s) [call #%d]",
                 result ? "YES" : "NO",
                 (void*)s_instance,
                 s_instance ? (void*)s_instance->m_device.get() : nullptr,
                 (s_instance && s_instance->m_device) ? (s_instance->m_device->IsInitialized() ? "yes" : "no") : "N/A",
                 s_isActiveCallCount);
        s_lastResult = result;
    }
    return result;
}

RTXRenderer::RTXRenderer()
    : m_device(std::make_unique<DX12Device>())
    , m_pipeline(std::make_unique<DXRPipeline>())
    , m_geometryExtractor(std::make_unique<SceneGeometryExtractor>())
    , m_accelerationStructure(std::make_unique<AccelerationStructure>())
    , m_giSystem(std::make_unique<GISystem>()) {
}

RTXRenderer::~RTXRenderer() {
    Shutdown();
    if (s_instance == this) {
        s_instance = nullptr;
    }
}

bool RTXRenderer::ProbeRTXSupport() {
    RTX_DIAG("RTXRenderer::ProbeRTXSupport() called");
    SPDLOG_INFO("[RTX] Probing RTX support...");
    if (!m_device) {
        RTX_DIAG("RTXRenderer::ProbeRTXSupport() FAILED - no DX12Device instance");
        SPDLOG_ERROR("[RTX] No DX12Device instance");
        return false;
    }
    bool result = m_device->ProbeDevice();
    RTX_DIAG("RTXRenderer::ProbeRTXSupport() result=%s", result ? "SUCCESS" : "FAILED");
    return result;
}

bool RTXRenderer::CompleteInitialization(HWND hwnd, uint32_t width, uint32_t height) {
    RTX_DIAG("RTXRenderer::CompleteInitialization() hwnd=%p, %ux%u", (void*)hwnd, width, height);
    SPDLOG_INFO("[RTX] Completing RTX initialization ({}x{})", width, height);

    RTX_DIAG("RTXRenderer::CompleteInitialization() step 1/6: DX12 device completion...");
    if (!m_device->CompleteInitialization(hwnd, width, height)) {
        RTX_DIAG("RTXRenderer::CompleteInitialization() FAILED at step 1: DX12 device completion");
        SPDLOG_ERROR("[RTX] Failed to complete DX12 device initialization");
        return false;
    }
    RTX_DIAG("RTXRenderer::CompleteInitialization() step 1/6: DX12 device OK");

    RTX_DIAG("RTXRenderer::CompleteInitialization() step 2/6: DXR pipeline...");
    if (!m_pipeline->Initialize(m_device.get(), width, height)) {
        RTX_DIAG("RTXRenderer::CompleteInitialization() FAILED at step 2: DXR pipeline");
        SPDLOG_ERROR("[RTX] Failed to initialize DXR pipeline");
        m_device->Shutdown();
        return false;
    }
    RTX_DIAG("RTXRenderer::CompleteInitialization() step 2/6: DXR pipeline OK");

    RTX_DIAG("RTXRenderer::CompleteInitialization() step 3/6: Acceleration structures...");
    if (!m_accelerationStructure->Initialize(m_device.get())) {
        RTX_DIAG("RTXRenderer::CompleteInitialization() FAILED at step 3: Acceleration structures");
        SPDLOG_ERROR("[RTX] Failed to initialize acceleration structures");
        m_pipeline->Shutdown();
        m_device->Shutdown();
        return false;
    }
    RTX_DIAG("RTXRenderer::CompleteInitialization() step 3/6: Acceleration structures OK");

    RTX_DIAG("RTXRenderer::CompleteInitialization() step 4/6: GI system...");
    if (!m_giSystem->Initialize(m_device.get())) {
        RTX_DIAG("RTXRenderer::CompleteInitialization() FAILED at step 4: GI system");
        SPDLOG_ERROR("[RTX] Failed to initialize GI system");
        m_accelerationStructure->Shutdown();
        m_pipeline->Shutdown();
        m_device->Shutdown();
        return false;
    }
    RTX_DIAG("RTXRenderer::CompleteInitialization() step 4/6: GI system OK");

    RTX_DIAG("RTXRenderer::CompleteInitialization() step 5/6: TextureManager...");
    if (!TextureManager::GetInstance().Initialize(m_device->GetDevice(), 4096)) {
        RTX_DIAG("RTXRenderer::CompleteInitialization() FAILED at step 5: TextureManager");
        SPDLOG_ERROR("[RTX] TextureManager singleton init failed");
        m_giSystem->Shutdown();
        m_accelerationStructure->Shutdown();
        m_pipeline->Shutdown();
        m_device->Shutdown();
        return false;
    }
    RTX_DIAG("RTXRenderer::CompleteInitialization() step 5/6: TextureManager OK");

    RTX_DIAG("RTXRenderer::CompleteInitialization() step 6/6: UAV descriptors...");
    if (!m_pipeline->CreateUAVDescriptors()) {
        RTX_DIAG("RTXRenderer::CompleteInitialization() FAILED at step 6: UAV descriptors");
        SPDLOG_ERROR("[RTX] Failed to create UAV descriptors");
        TextureManager::GetInstance().Shutdown();
        m_giSystem->Shutdown();
        m_accelerationStructure->Shutdown();
        m_pipeline->Shutdown();
        m_device->Shutdown();
        return false;
    }
    RTX_DIAG("RTXRenderer::CompleteInitialization() step 6/6: UAV descriptors OK");

    RTX_DIAG("RTXRenderer::CompleteInitialization() SUCCESS - all 6 steps completed");
    SPDLOG_INFO("[RTX] RTX renderer fully initialized (via two-phase path)");
    return true;
}

bool RTXRenderer::Initialize(HWND hwnd, uint32_t width, uint32_t height) {
    RTX_DIAG("RTXRenderer::Initialize() single-phase init, hwnd=%p, %ux%u", (void*)hwnd, width, height);
    SPDLOG_INFO("[RTX] Initializing RTX renderer ({}x{})", width, height);

    RTX_DIAG("RTXRenderer::Initialize() step 1: DX12 device...");
    if (!m_device->Initialize(hwnd, width, height)) {
        RTX_DIAG("RTXRenderer::Initialize() FAILED at step 1: DX12 device");
        SPDLOG_ERROR("[RTX] Failed to initialize DX12 device");
        return false;
    }
    RTX_DIAG("RTXRenderer::Initialize() step 1: DX12 device OK");

    RTX_DIAG("RTXRenderer::Initialize() step 2: checking raytracing support...");
    if (!m_device->SupportsRaytracing()) {
        RTX_DIAG("RTXRenderer::Initialize() FAILED at step 2: DXR not supported");
        SPDLOG_ERROR("[RTX] Raytracing not supported - RTX renderer disabled");
        m_device->Shutdown();
        return false;
    }
    RTX_DIAG("RTXRenderer::Initialize() step 2: DXR supported");

    RTX_DIAG("RTXRenderer::Initialize() step 3: DXR pipeline...");
    if (!m_pipeline->Initialize(m_device.get(), width, height)) {
        RTX_DIAG("RTXRenderer::Initialize() FAILED at step 3: DXR pipeline");
        SPDLOG_ERROR("[RTX] Failed to initialize DXR pipeline");
        m_device->Shutdown();
        return false;
    }
    RTX_DIAG("RTXRenderer::Initialize() step 3: DXR pipeline OK");

    RTX_DIAG("RTXRenderer::Initialize() step 4: Acceleration structures...");
    if (!m_accelerationStructure->Initialize(m_device.get())) {
        RTX_DIAG("RTXRenderer::Initialize() FAILED at step 4: Acceleration structures");
        SPDLOG_ERROR("[RTX] Failed to initialize acceleration structures");
        m_pipeline->Shutdown();
        m_device->Shutdown();
        return false;
    }
    RTX_DIAG("RTXRenderer::Initialize() step 4: Acceleration structures OK");

    RTX_DIAG("RTXRenderer::Initialize() step 5: GI system...");
    if (!m_giSystem->Initialize(m_device.get())) {
        RTX_DIAG("RTXRenderer::Initialize() FAILED at step 5: GI system");
        SPDLOG_ERROR("[RTX] Failed to initialize GI system");
        m_accelerationStructure->Shutdown();
        m_pipeline->Shutdown();
        m_device->Shutdown();
        return false;
    }
    RTX_DIAG("RTXRenderer::Initialize() step 5: GI system OK");

    // Initialize the TextureManager singleton so that RTXHooks texture interception
    // and ResolveMaterialTextures can use TextureManager::GetInstance().
    // This MUST happen before CreateUAVDescriptors() because the UAV descriptors
    // are placed in the TextureManager's SRV heap.
    RTX_DIAG("RTXRenderer::Initialize() step 6: TextureManager...");
    if (!TextureManager::GetInstance().Initialize(m_device->GetDevice(), 4096)) {
        RTX_DIAG("RTXRenderer::Initialize() FAILED at step 6: TextureManager");
        SPDLOG_ERROR("[RTX] TextureManager singleton init failed — textures will not be available");
        m_giSystem->Shutdown();
        m_accelerationStructure->Shutdown();
        m_pipeline->Shutdown();
        m_device->Shutdown();
        return false;
    }
    RTX_DIAG("RTXRenderer::Initialize() step 6: TextureManager OK");

    // Now that TextureManager's SRV heap is available, create the UAV descriptors
    // for the raytracing output, accumulation, and denoise temp buffers in the
    // shared shader-visible heap. This must happen after TextureManager::Initialize().
    RTX_DIAG("RTXRenderer::Initialize() step 7: UAV descriptors...");
    if (!m_pipeline->CreateUAVDescriptors()) {
        RTX_DIAG("RTXRenderer::Initialize() FAILED at step 7: UAV descriptors");
        SPDLOG_ERROR("[RTX] Failed to create UAV descriptors in TextureManager SRV heap");
        TextureManager::GetInstance().Shutdown();
        m_giSystem->Shutdown();
        m_accelerationStructure->Shutdown();
        m_pipeline->Shutdown();
        m_device->Shutdown();
        return false;
    }
    RTX_DIAG("RTXRenderer::Initialize() step 7: UAV descriptors OK");

    RTX_DIAG("RTXRenderer::Initialize() SUCCESS - all steps completed (single-phase)");
    SPDLOG_INFO("[RTX] RTX renderer initialized successfully");
    return true;
}

void RTXRenderer::Shutdown() {
    RTX_DIAG("RTXRenderer::Shutdown() starting (scene=%d, sceneLoaded=%s)",
             m_currentScene, m_sceneLoaded ? "yes" : "no");
    // Shut down the TextureManager singleton (Device5 path) first
    TextureManager::GetInstance().Shutdown();

    // Shutdown in reverse initialization order:
    // Init:     DX12Device -> DXRPipeline -> AccelStruct -> GISystem -> TextureManager(singleton)
    // Shutdown: TextureManager(singleton) -> GISystem -> AccelStruct -> DXRPipeline -> DX12Device
    if (m_giSystem) m_giSystem->Shutdown();
    if (m_accelerationStructure) m_accelerationStructure->Shutdown();
    if (m_pipeline) m_pipeline->Shutdown();
    if (m_device) m_device->Shutdown();
    m_roomGeometry.clear();
    m_sceneLoaded = false;
    m_currentScene = -1;
    SPDLOG_INFO("[RTX] RTX renderer shut down");
}

void RTXRenderer::OnSceneLoaded(int sceneNum) {
    RTX_DIAG("RTXRenderer::OnSceneLoaded() scene=0x%02X (%d), previous scene=%d", sceneNum, sceneNum, m_currentScene);
    m_currentScene = sceneNum;
    m_sceneLoaded = true;
    m_roomGeometry.clear();

    // Load and apply per-scene configuration from RTXSceneConfig.
    // LoadSceneConfig updates both the returned config and the internal
    // GetCurrentConfig() state so other subsystems see the correct scene.
    m_currentSceneConfig = LoadSceneConfig(sceneNum);
    if (m_giSystem) {
        m_giSystem->ResetAccumulation();
        m_giSystem->ApplySceneConfig(m_currentSceneConfig);
    }

    SPDLOG_INFO("[RTX] Scene loaded: 0x{:02X} (RTX enabled: {}, GI intensity: {:.2f}, max bounces: {})",
                sceneNum, m_currentSceneConfig.enabled, m_currentSceneConfig.giIntensity, m_currentSceneConfig.maxBounces);
    // Geometry extraction and BLAS building triggered per-room via OnRoomLoaded()
}

void RTXRenderer::OnSceneUnload() {
    RTX_DIAG("RTXRenderer::OnSceneUnload() scene=%d, roomGeometryCount=%zu", m_currentScene, m_roomGeometry.size());
    m_sceneLoaded = false;
    m_currentScene = -1;
    m_currentSceneConfig = GetDefaultConfig();
    ClearSceneOverride();

    // Release acceleration structures
    if (m_accelerationStructure) {
        m_accelerationStructure->ReleaseAll();
    }
    m_roomGeometry.clear();

    // Release cached textures from the singleton (keeps default textures).
    // The singleton is what RTXHooks uses for texture interception, so its
    // cache is what actually contains uploaded textures from the game.
    TextureManager::GetInstance().ReleaseAllTextures();

    if (m_giSystem) {
        m_giSystem->ResetAccumulation();
    }

    SPDLOG_INFO("[RTX] Scene unloaded");
}

void RTXRenderer::OnRoomLoaded(void* playPtr, int roomNum) {
    RTX_DIAG("RTXRenderer::OnRoomLoaded() room=%d, play=%p, scene=%d", roomNum, playPtr, m_currentScene);
    SPDLOG_INFO("[RTX] Room {} loaded", roomNum);

    if (!playPtr || !m_geometryExtractor || !m_accelerationStructure) {
        RTX_DIAG("RTXRenderer::OnRoomLoaded() FAILED - missing dependencies (play=%p, extractor=%p, accel=%p)",
                 playPtr, (void*)m_geometryExtractor.get(), (void*)m_accelerationStructure.get());
        SPDLOG_ERROR("[RTX] OnRoomLoaded: missing dependencies");
        return;
    }

    // Get the Room struct from PlayState
    PlayState* play = (PlayState*)playPtr;
    Room* room = nullptr;

    // Check current room
    if (play->roomCtx.curRoom.num == roomNum) {
        room = &play->roomCtx.curRoom;
    }
    // Check previous room (for room transitions)
    else if (play->roomCtx.prevRoom.num == roomNum) {
        room = &play->roomCtx.prevRoom;
    }

    if (!room || !room->segment) {
        SPDLOG_WARN("[RTX] OnRoomLoaded: could not find Room struct for room {}", roomNum);
        return;
    }

    // Phase 2: Extract geometry from room display lists
    RTX_DIAG("RTXRenderer::OnRoomLoaded() extracting geometry for room %d...", roomNum);
    RoomGeometry geometry = m_geometryExtractor->ExtractRoomGeometry(room, (uint32_t)roomNum);

    bool hasGeometry = !geometry.opaqueMesh.vertices.empty() || !geometry.alphaMesh.vertices.empty();
    RTX_DIAG("RTXRenderer::OnRoomLoaded() room %d extraction: hasGeometry=%s, opaqueVerts=%zu, alphaVerts=%zu, opaqueTris=%zu, alphaTris=%zu",
             roomNum, hasGeometry ? "yes" : "no",
             geometry.opaqueMesh.vertices.size(), geometry.alphaMesh.vertices.size(),
             geometry.opaqueMesh.indices.size() / 3, geometry.alphaMesh.indices.size() / 3);
    if (!hasGeometry) {
        RTX_DIAG("RTXRenderer::OnRoomLoaded() room %d: NO geometry extracted - skipping BLAS build", roomNum);
        SPDLOG_WARN("[RTX] OnRoomLoaded: no geometry extracted for room {}", roomNum);
        return;
    }

    // Phase 6: Resolve raw texture address hashes in Material::textureIndex
    // to actual SRV descriptor heap indices from the TextureManager.
    // Textures not yet intercepted get SRV index 0 (default white).
    RTX_DIAG("RTXRenderer::OnRoomLoaded() room %d: resolving material textures (opaqueMats=%zu, alphaMats=%zu)...",
             roomNum, geometry.opaqueMesh.materials.size(), geometry.alphaMesh.materials.size());
    ResolveMaterialTextures(geometry);

    // Phase 3: Build BLAS for this room
    RTX_DIAG("RTXRenderer::OnRoomLoaded() room %d: building BLAS...", roomNum);
    if (!m_accelerationStructure->BuildBLAS(geometry)) {
        RTX_DIAG("RTXRenderer::OnRoomLoaded() room %d: BLAS build FAILED", roomNum);
        SPDLOG_ERROR("[RTX] OnRoomLoaded: failed to build BLAS for room {}", roomNum);
        return;
    }
    RTX_DIAG("RTXRenderer::OnRoomLoaded() room %d: BLAS build SUCCESS", roomNum);

    // Store for reference
    m_roomGeometry.push_back(std::move(geometry));

    // Update the hit group shader table with per-geometry buffer GPU addresses.
    // This must happen after BuildBLAS so the geometry buffer list is populated.
    {
        const auto& geomBuffers = m_accelerationStructure->GetGeometryBuffers();
        std::vector<DXRPipeline::GeometryBufferAddresses> addrs;
        addrs.reserve(geomBuffers.size());
        for (const auto& gb : geomBuffers) {
            DXRPipeline::GeometryBufferAddresses a;
            a.vertexBuffer    = gb.vertexBufferAddress;
            a.indexBuffer     = gb.indexBufferAddress;
            a.materialIDBuffer = gb.materialIDBufferAddress;
            a.materialTable   = gb.materialTableAddress;
            addrs.push_back(a);
        }
        m_pipeline->UpdateHitGroupShaderTable(addrs);
    }

    SPDLOG_INFO("[RTX] Room {} fully loaded: BLAS built, {} instances total",
                roomNum, m_accelerationStructure->GetInstanceCount());
}

void RTXRenderer::UpdateSceneParams(
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
) {
    // Delegate temporal accumulation + per-scene GI config to GISystem.
    // GISystem::Update() re-applies the scene config each frame so that dynamic
    // adjustments (e.g., time-of-day modulation) take effect continuously.
    uint32_t accumulationFrameCount = 0;
    if (m_giSystem) {
        m_giSystem->Update(viewMatrix, m_currentSceneConfig);
        accumulationFrameCount = m_giSystem->GetAccumulationFrameCount();
    }

    // Compute inverse matrices for ray generation
    InvertMatrix4x4(viewMatrix, m_sceneConstants.viewInverse);
    InvertMatrix4x4(projMatrix, m_sceneConstants.projInverse);

    // Camera
    m_sceneConstants.cameraPos[0] = cameraPos[0];
    m_sceneConstants.cameraPos[1] = cameraPos[1];
    m_sceneConstants.cameraPos[2] = cameraPos[2];
    m_sceneConstants.frameCount = accumulationFrameCount;

    // Ambient light
    m_sceneConstants.ambientColor[0] = ambientColor[0];
    m_sceneConstants.ambientColor[1] = ambientColor[1];
    m_sceneConstants.ambientColor[2] = ambientColor[2];

    // Fog
    // Kokiri Forest fog logic from func_8009E0B8
    float actualFogNear = fogNear;
    float vegetationAlpha = 128.0f / 255.0f;

    if (sceneSetupIndex == 4) {
        // Deku Tree death: fade alpha during cutscene
        vegetationAlpha = (255.0f - roomUnk74) / 255.0f;
    } else if (sceneSetupIndex == 6) {
        // Deku Tree death: increase fog during cutscene
        actualFogNear = roomUnk74 + 500.0f;
    } else if ((sceneSetupIndex < 4 || isAdultLink) && dekuTreeDead) {
        // After Deku Tree is dead: permanent fog distance change
        actualFogNear = 2150.0f;
    }

    m_sceneConstants.fogNear = actualFogNear;
    m_sceneConstants.fogFar = fogFar;
    m_sceneConstants.fogColor[0] = fogColor[0];
    m_sceneConstants.fogColor[1] = fogColor[1];
    m_sceneConstants.fogColor[2] = fogColor[2];

    // Directional lights
    memcpy(m_sceneConstants.sunDirection1, sunDir1, sizeof(float) * 3);
    memcpy(m_sceneConstants.sunColor1, sunColor1, sizeof(float) * 3);
    memcpy(m_sceneConstants.sunDirection2, sunDir2, sizeof(float) * 3);
    memcpy(m_sceneConstants.sunColor2, sunColor2, sizeof(float) * 3);

    // Apply scene config fog overrides (non-zero values override game values)
    if (m_currentSceneConfig.fogNearDefault > 0.0f) {
        m_sceneConstants.fogNear = m_currentSceneConfig.fogNearDefault;
    }
    if (m_currentSceneConfig.fogFarDefault > 0.0f) {
        m_sceneConstants.fogFar = m_currentSceneConfig.fogFarDefault;
    }
    if (m_currentSceneConfig.fogColorDefault[0] > 0.0f ||
        m_currentSceneConfig.fogColorDefault[1] > 0.0f ||
        m_currentSceneConfig.fogColorDefault[2] > 0.0f) {
        m_sceneConstants.fogColor[0] = m_currentSceneConfig.fogColorDefault[0];
        m_sceneConstants.fogColor[1] = m_currentSceneConfig.fogColorDefault[1];
        m_sceneConstants.fogColor[2] = m_currentSceneConfig.fogColorDefault[2];
    }

    // Time and per-frame values
    m_sceneConstants.time = static_cast<float>(gameplayFrames);
    m_sceneConstants.dekuTreeAlpha = vegetationAlpha;
    m_sceneConstants.fogBlendAlpha = actualFogNear * 0.1f / 255.0f;
    m_sceneConstants.waterScrollOffset = roomUnk74 * 0.02f;

    // Per-scene material overrides from RTXSceneConfig
    m_sceneConstants.giIntensity = m_currentSceneConfig.giIntensity;
    m_sceneConstants.baseReflectivity = m_currentSceneConfig.baseReflectivity;
    m_sceneConstants.roughnessScale = m_currentSceneConfig.roughnessScale;
    m_sceneConstants.emissiveScale = m_currentSceneConfig.emissiveScale;
    m_sceneConstants.waterReflectivity = m_currentSceneConfig.waterReflectivity;
    m_sceneConstants.waterRoughness = m_currentSceneConfig.waterRoughness;
    m_sceneConstants.aoRadius = m_currentSceneConfig.aoRadius;
    m_sceneConstants.aoIntensity = m_currentSceneConfig.aoIntensity;

    // Upload to GPU
    m_pipeline->UpdateSceneConstants(m_sceneConstants);
}

bool RTXRenderer::IsRTXSceneActive() const {
    return m_sceneLoaded && IsRTXScene(static_cast<uint16_t>(m_currentScene));
}

void RTXRenderer::DispatchAndPresent() {
    if (!m_device->IsInitialized()) {
        static bool s_loggedUninit = false;
        if (!s_loggedUninit) {
            RTX_DIAG("RTXRenderer::DispatchAndPresent() skipped - device not initialized");
            s_loggedUninit = true;
        }
        return;
    }

    static uint32_t s_renderFrameCount = 0;
    s_renderFrameCount++;
    if (s_renderFrameCount <= 5 || (s_renderFrameCount % 300) == 0) {
        printf("[RTX] RTXRenderer::Render() called (frame #%u)\n", s_renderFrameCount);
        uint32_t instanceCount = m_accelerationStructure ? m_accelerationStructure->GetInstanceCount() : 0;
        bool deviceValid = m_device && m_device->IsInitialized();
        RTX_DIAG("[DIAG] RTXRenderer::Render() called (frame #%u, scene=%d, rtxSceneActive=%s, DX12DeviceValid=%s, TLASInstances=%u)",
                 s_renderFrameCount, m_currentScene, IsRTXSceneActive() ? "yes" : "no",
                 deviceValid ? "YES" : "NO", instanceCount);
    }

    // For non-RTX scenes (or if scene not loaded), just clear and present
    if (!IsRTXSceneActive()) {
        if (s_renderFrameCount <= 5 || (s_renderFrameCount % 300) == 0) {
            RTX_DIAG("RTXRenderer::DispatchAndPresent() non-RTX scene path (frame #%u, scene=%d)", s_renderFrameCount, m_currentScene);
        }
        m_device->BeginFrame();
        auto* cmdList = m_device->GetCommandList();

        // Transition back buffer to render target
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = m_device->GetCurrentBackBuffer();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmdList->ResourceBarrier(1, &barrier);

        // Clear to black
        float clearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        cmdList->ClearRenderTargetView(m_device->GetCurrentRTVHandle(), clearColor, 0, nullptr);

        // Transition back to present
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        cmdList->ResourceBarrier(1, &barrier);

        cmdList->Close();
        ID3D12CommandList* ppCmdLists[] = { cmdList };
        m_device->GetCommandQueue()->ExecuteCommandLists(1, ppCmdLists);
        m_device->Present();
        return;
    }

    // Advance the TextureManager's frame counter for LRU eviction tracking
    TextureManager::GetInstance().AdvanceFrame();

    // Begin frame
    m_device->BeginFrame();
    auto* cmdList = m_device->GetCommandList();

    // Transition back buffer to render target
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = m_device->GetCurrentBackBuffer();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmdList->ResourceBarrier(1, &barrier);

    // Set descriptor heaps (SRV heap is needed for textures and UAVs).
    // If the TextureManager singleton has its own SRV heap, bind both the
    // device heap and the texture heap.  DX12 allows up to one of each type,
    // so we prefer the TextureManager's heap when available (it contains the
    // bindless texture table) alongside the device's SRV heap for UAVs.
    {
        ID3D12DescriptorHeap* texHeap = TextureManager::GetInstance().GetSRVHeap();
        if (texHeap && texHeap != m_device->GetSRVHeap()) {
            // DX12 only allows one CBV_SRV_UAV heap at a time.
            // Bind TextureManager's heap so texture SRVs are accessible during
            // DispatchRays; output UAVs are referenced via root descriptors.
            ID3D12DescriptorHeap* heaps[] = { texHeap };
            cmdList->SetDescriptorHeaps(1, heaps);
        } else {
            ID3D12DescriptorHeap* heaps[] = { m_device->GetSRVHeap() };
            cmdList->SetDescriptorHeaps(1, heaps);
        }
    }

    // Rebuild TLAS with all loaded rooms (must happen before DispatchRays)
    if (m_accelerationStructure && m_accelerationStructure->GetInstanceCount() > 0) {
        if (s_renderFrameCount <= 3 || (s_renderFrameCount % 300) == 0) {
            RTX_DIAG("[DIAG] RTXRenderer: Building TLAS with %u BLAS instances (frame #%u)",
                     m_accelerationStructure->GetInstanceCount(), s_renderFrameCount);
        }
        m_accelerationStructure->RebuildTLAS(cmdList);
    }

    // Dispatch rays (pass TLAS address and texture table handle if available)
    D3D12_GPU_VIRTUAL_ADDRESS tlasAddr = 0;
    if (m_accelerationStructure && m_accelerationStructure->HasTLAS()) {
        tlasAddr = m_accelerationStructure->GetTLASAddress();
    }

    // Skip DispatchRays if there's no TLAS (no geometry loaded yet).
    // Without a TLAS, the RayGen shader would reference an uninitialized acceleration
    // structure SRV, which is undefined behavior on the GPU. Instead, we just clear
    // the back buffer and present, which will show a black screen until geometry loads.
    if (tlasAddr == 0) {
        if (s_renderFrameCount <= 5 || (s_renderFrameCount % 60) == 0) {
            RTX_DIAG("RTXRenderer: No TLAS available (tlasAddr=0), clearing to dark blue (frame #%u)", s_renderFrameCount);
        }
        float clearColor[4] = { 0.0f, 0.0f, 0.05f, 1.0f }; // Dark blue to indicate RTX is active but no geometry
        cmdList->ClearRenderTargetView(m_device->GetCurrentRTVHandle(), clearColor, 0, nullptr);

        D3D12_RESOURCE_BARRIER presentBarrier = {};
        presentBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        presentBarrier.Transition.pResource = m_device->GetCurrentBackBuffer();
        presentBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        presentBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        presentBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmdList->ResourceBarrier(1, &presentBarrier);

        cmdList->Close();
        ID3D12CommandList* ppCmdLists[] = { cmdList };
        m_device->GetCommandQueue()->ExecuteCommandLists(1, ppCmdLists);
        m_device->Present();
        return;
    }

    // Get the texture table GPU descriptor handle from the TextureManager singleton.
    // This is the start of the bindless SRV table that shaders use to sample textures.
    D3D12_GPU_DESCRIPTOR_HANDLE texTableGPU = {};
    texTableGPU.ptr = 0;
    {
        auto& texMgr = TextureManager::GetInstance();
        ID3D12DescriptorHeap* texHeap = texMgr.GetSRVHeap();
        if (texHeap) {
            texTableGPU = texMgr.GetSRVTableStart();
        }
    }

    if (s_renderFrameCount <= 3 || (s_renderFrameCount % 300) == 0) {
        RTX_DIAG("RTXRenderer: DispatchRays %ux%u, TLAS=0x%llX, texTable=0x%llX",
                 m_device->GetWidth(), m_device->GetHeight(),
                 (unsigned long long)tlasAddr, (unsigned long long)texTableGPU.ptr);
    }
    m_pipeline->DispatchRays(cmdList, m_device->GetWidth(), m_device->GetHeight(), tlasAddr, texTableGPU);
    if (s_renderFrameCount <= 5 || (s_renderFrameCount % 300) == 0) {
        printf("[RTX] DispatchRays completed (frame #%u)\n", s_renderFrameCount);
    }

    // UAV barrier between ray tracing output and denoise input.
    // Use a null-resource UAV barrier (pResource = nullptr) which acts as a
    // global UAV barrier across all resources. This is necessary because
    // denoise ping-pongs between the output buffer and temp buffer, so we
    // need to ensure all UAV writes are visible regardless of which buffer.
    D3D12_RESOURCE_BARRIER uavBarrier = {};
    uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarrier.UAV.pResource = nullptr; // Global UAV barrier for all resources
    cmdList->ResourceBarrier(1, &uavBarrier);

    // Denoise passes (3 A-trous wavelet passes)
    // Use GISystem's per-scene tuned denoise constants when available
    constexpr int numDenoisePasses = GISystem::NUM_DENOISE_PASSES;
    for (int pass = 0; pass < numDenoisePasses; pass++) {
        if (m_giSystem) {
            DenoiseConstants dc = m_giSystem->GetDenoiseConstants(pass);
            m_pipeline->DispatchDenoise(cmdList, m_device->GetWidth(), m_device->GetHeight(), pass, dc);
        } else {
            m_pipeline->DispatchDenoise(cmdList, m_device->GetWidth(), m_device->GetHeight(), pass);
        }

        // Global UAV barrier between denoise passes to ensure writes from the
        // current pass are visible to the next pass's reads (ping-pong buffers).
        cmdList->ResourceBarrier(1, &uavBarrier);
    }

    // After ping-pong denoise, determine which buffer has the final result.
    // With 3 passes (odd): result is in temp buffer.
    // With 0 passes or even count: result is in output buffer.
    ID3D12Resource* denoisedBuffer = m_pipeline->GetFinalDenoisedBuffer(numDenoisePasses);

    // If no denoised buffer is available (e.g., pipeline not fully initialized),
    // fall back to the output buffer, then to just presenting a cleared back buffer.
    if (!denoisedBuffer) {
        denoisedBuffer = m_pipeline->GetOutputBuffer();
    }

    if (denoisedBuffer) {
        // Copy denoised result to back buffer
        // Transition the denoised buffer from UAV to copy source
        D3D12_RESOURCE_BARRIER copyBarriers[2] = {};
        copyBarriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        copyBarriers[0].Transition.pResource = denoisedBuffer;
        copyBarriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        copyBarriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        copyBarriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

        copyBarriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        copyBarriers[1].Transition.pResource = m_device->GetCurrentBackBuffer();
        copyBarriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        copyBarriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        copyBarriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

        cmdList->ResourceBarrier(2, copyBarriers);
        cmdList->CopyResource(m_device->GetCurrentBackBuffer(), denoisedBuffer);

        // Transition back buffer to present, denoised buffer back to UAV
        D3D12_RESOURCE_BARRIER presentBarriers[2] = {};
        presentBarriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        presentBarriers[0].Transition.pResource = m_device->GetCurrentBackBuffer();
        presentBarriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        presentBarriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        presentBarriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

        presentBarriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        presentBarriers[1].Transition.pResource = denoisedBuffer;
        presentBarriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        presentBarriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        presentBarriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

        cmdList->ResourceBarrier(2, presentBarriers);
    } else {
        // No output buffer available; just transition back buffer to present
        D3D12_RESOURCE_BARRIER presentBarrier = {};
        presentBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        presentBarrier.Transition.pResource = m_device->GetCurrentBackBuffer();
        presentBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        presentBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        presentBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmdList->ResourceBarrier(1, &presentBarrier);
    }

    // Close and execute command list, then present
    cmdList->Close();
    ID3D12CommandList* ppCommandLists[] = { cmdList };
    m_device->GetCommandQueue()->ExecuteCommandLists(1, ppCommandLists);

    m_device->Present();
}

// Helper: check if a uintptr_t value looks like a valid host pointer to a string.
// On 64-bit Windows, user-mode addresses start at 0x10000+.
static bool LooksLikeStringPointer(uintptr_t addr) {
    if (addr < 0x10000) return false;
#ifdef _WIN32
    if (IsBadReadPtr((const void*)addr, 2)) return false;
#endif
    const char* s = (const char*)addr;
    // Check for OTR path: starts with "__" (from "__OTR__")
    return (s[0] == '_' && s[1] == '_') || (s[0] >= 0x20 && s[0] <= 0x7E);
}

// Helper: try to load a texture from an OTR path and upload it to the TextureManager.
// Returns the SRV index (>0) on success, or 0 on failure.
static uint32_t TryLoadOTRTexture(const char* otrPath, TextureManager& texMgr) {
    // Load raw texture data from OTR archive
    char* rawData = nullptr;
    uint32_t texWidth = 0;
    uint32_t texHeight = 0;

    try {
        rawData = ResourceMgr_LoadTexOrDListByName(otrPath);
        texWidth = ResourceMgr_LoadTexWidthByName((char*)otrPath);
        texHeight = ResourceMgr_LoadTexHeightByName((char*)otrPath);
    } catch (...) {
        SPDLOG_TRACE("[RTX] Exception loading OTR texture: {}", otrPath);
        return 0;
    }

    if (!rawData || texWidth == 0 || texHeight == 0) {
        return 0;
    }

    // The OTR resource data for textures is pre-decoded RGBA32 by the SoH resource system.
    // SoH's texture resource loader decodes N64 format textures to RGBA8888 during OTR extraction.
    // So rawData is already RGBA8 pixel data of size width * height * 4.
    size_t dataSize = ResourceGetTexSizeByName(otrPath);
    size_t expectedSize = (size_t)texWidth * texHeight * 4;

    // If the resource size matches expected RGBA32 size, treat as pre-decoded
    const uint8_t* rgbaData = nullptr;
    if (dataSize >= expectedSize) {
        rgbaData = (const uint8_t*)rawData;
    } else {
        // Size doesn't match — likely raw N64 format, can't easily decode without
        // knowing the exact N64 format. Fall back to white texture.
        SPDLOG_TRACE("[RTX] OTR texture {} size mismatch: got {} expected {}",
                     otrPath, dataSize, expectedSize);
        return 0;
    }

    // Compute a hash for this texture based on the OTR path string
    constexpr uint64_t FNV_OFFSET = 0xcbf29ce484222325ULL;
    constexpr uint64_t FNV_PRIME  = 0x100000001b3ULL;
    uint64_t hash = FNV_OFFSET;
    for (const char* p = otrPath; *p; p++) {
        hash ^= (uint64_t)(uint8_t)*p;
        hash *= FNV_PRIME;
    }

    // Upload to TextureManager
    RTXTextureHandle handle = texMgr.GetOrUploadTexture(rgbaData, texWidth, texHeight, hash);

    if (handle.srvIndex > 0) {
        // Also register the address-based hash as an alias so future lookups work
        uintptr_t addr = (uintptr_t)otrPath;
        uint64_t addrHash = FNV_OFFSET;
        for (size_t b = 0; b < sizeof(addr); b++) {
            addrHash ^= static_cast<uint64_t>((addr >> (b * 8)) & 0xFF);
            addrHash *= FNV_PRIME;
        }
        texMgr.RegisterHashAlias(addrHash, handle.srvIndex);
        texMgr.RegisterHashAlias(static_cast<uint64_t>(addr), handle.srvIndex);
        texMgr.RegisterHashAlias(hash, handle.srvIndex);

        SPDLOG_DEBUG("[RTX] Loaded OTR texture '{}' ({}x{}) -> SRV index {}",
                     otrPath, texWidth, texHeight, handle.srvIndex);
    }

    return handle.srvIndex;
}

void RTXRenderer::ResolveMaterialTextures(RoomGeometry& geometry) {
    auto& texMgr = TextureManager::GetInstance();

    // Helper lambda: resolve all materials in an ExtractedMesh.
    // Each material has a corresponding entry in materialTextureAddrs[]
    // containing the full N64/OTR texture address (uintptr_t).
    //
    // Resolution strategy (in priority order):
    // 1. Check TextureManager cache by address-based FNV-1a hash
    // 2. Check TextureManager cache by raw address value
    // 3. If the address looks like an OTR path string, load the texture
    //    directly from the OTR archive and upload it to TextureManager
    // 4. Fall back to default white texture (SRV index 0)
    auto resolveMesh = [&](ExtractedMesh& mesh) {
        uint32_t resolved = 0;
        uint32_t loadedFromOTR = 0;
        uint32_t unresolved = 0;

        for (size_t i = 0; i < mesh.materials.size(); i++) {
            Material& mat = mesh.materials[i];

            // Get the full texture address from the parallel vector.
            uintptr_t textureAddr = 0;
            if (i < mesh.materialTextureAddrs.size()) {
                textureAddr = mesh.materialTextureAddrs[i];
            }

            if (textureAddr == 0) {
                // No texture set for this material; use default white (SRV index 0).
                mat.textureIndex = 0;
                continue;
            }

            // Compute address-based FNV-1a hash
            constexpr uint64_t FNV_OFFSET = 0xcbf29ce484222325ULL;
            constexpr uint64_t FNV_PRIME  = 0x100000001b3ULL;
            uint64_t hash = FNV_OFFSET;
            for (size_t b = 0; b < sizeof(textureAddr); b++) {
                hash ^= static_cast<uint64_t>((textureAddr >> (b * 8)) & 0xFF);
                hash *= FNV_PRIME;
            }

            // Try the address-only hash first
            uint32_t srvIndex = texMgr.GetSRVIndexForHash(hash);

            if (srvIndex > 0) {
                mat.textureIndex = srvIndex;
                resolved++;
                continue;
            }

            // Try the raw address value as a fallback hash key
            srvIndex = texMgr.GetSRVIndexForHash(static_cast<uint64_t>(textureAddr));
            if (srvIndex > 0) {
                mat.textureIndex = srvIndex;
                resolved++;
                continue;
            }

            // Not in cache yet. Try to load from OTR if the address looks like a path.
            if (LooksLikeStringPointer(textureAddr)) {
                const char* otrPath = (const char*)textureAddr;
                srvIndex = TryLoadOTRTexture(otrPath, texMgr);
                if (srvIndex > 0) {
                    mat.textureIndex = srvIndex;
                    loadedFromOTR++;
                    continue;
                }
            }

            // Texture not available. Fall back to default white (SRV index 0).
            mat.textureIndex = 0;
            unresolved++;
        }

        if (resolved > 0 || loadedFromOTR > 0 || unresolved > 0) {
            SPDLOG_INFO("[RTX] ResolveMaterialTextures: {} cached, {} loaded from OTR, {} unresolved (white)",
                        resolved, loadedFromOTR, unresolved);
        }
    };

    resolveMesh(geometry.opaqueMesh);
    resolveMesh(geometry.alphaMesh);
}

// Basic 4x4 matrix inversion (row-major)
void RTXRenderer::InvertMatrix4x4(const float m[16], float out[16]) {
    float inv[16];

    inv[0] = m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15]
           + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4] = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15]
           - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8] = m[4]*m[9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15]
           + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14]
            - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];

    inv[1] = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15]
           - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5] = m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15]
           + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9] = -m[0]*m[9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15]
           - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] = m[0]*m[9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14]
            + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];

    inv[2] = m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15]
           + m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
    inv[6] = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15]
           - m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
    inv[10] = m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15]
            + m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14]
            - m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];

    inv[3] = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11]
           - m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
    inv[7] = m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11]
           + m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9] + m[4]*m[1]*m[11]
            - m[4]*m[3]*m[9] - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
    inv[15] = m[0]*m[5]*m[10] - m[0]*m[6]*m[9] - m[4]*m[1]*m[10]
            + m[4]*m[2]*m[9] + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];

    float det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
    if (fabsf(det) < 1e-10f) {
        // Singular matrix, return identity
        memset(out, 0, sizeof(float) * 16);
        out[0] = out[5] = out[10] = out[15] = 1.0f;
        return;
    }

    det = 1.0f / det;
    for (int i = 0; i < 16; i++) {
        out[i] = inv[i] * det;
    }
}

} // namespace RTX

#endif // ENABLE_DX12_RTX
