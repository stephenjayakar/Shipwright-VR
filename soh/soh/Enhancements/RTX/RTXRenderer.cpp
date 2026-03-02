#ifdef ENABLE_DX12_RTX

#include "RTXRenderer.h"
#include "RTXSceneConfig.h"
#include "TextureManager.h"
#include <spdlog/spdlog.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <fstream>
#ifdef _WIN32
#include <Windows.h>
#endif

#include "RTXDiagLog.h"

// These headers have their own extern "C" guards internally.
#include "global.h"
#include "z64.h"

// gSegments is defined in C code; we need it for SEGMENTED_TO_VIRTUAL in proactive room loading
extern "C" uintptr_t gSegments[];

// SOH resource types for proactive room loading
#include "soh/resource/type/Scene.h"
#include "soh/resource/type/scenecommand/SetMesh.h"
#include "soh/resource/type/scenecommand/SceneCommand.h"
#include "soh/ResourceManagerHelpers.h"

// Forward declarations for OTR resource loading functions used by texture resolution.
extern "C" {
    char* ResourceMgr_LoadTexOrDListByName(const char* filePath);
    uint16_t ResourceMgr_LoadTexWidthByName(char* texPath);
    uint16_t ResourceMgr_LoadTexHeightByName(char* texPath);
    size_t ResourceGetTexSizeByName(const char* name);
    char* ResourceMgr_LoadTexDataForRTX(const char* texPath, uint32_t* outType,
                                         uint16_t* outWidth, uint16_t* outHeight,
                                         uint32_t* outDataSize);
}

// Fast::TextureType enum values (from libultraship/include/fast/resource/type/Texture.h)
// Duplicated here to avoid including the C++ header in this translation unit.
enum OTR_TextureType : uint32_t {
    OTR_TEX_ERROR           = 0,
    OTR_TEX_RGBA32          = 1,  // RGBA 8-8-8-8 (32bpp, already decoded)
    OTR_TEX_RGBA16          = 2,  // RGBA 5-5-5-1 (16bpp, N64 format)
    OTR_TEX_CI4             = 3,  // CI 4-bit (palette indexed)
    OTR_TEX_CI8             = 4,  // CI 8-bit (palette indexed)
    OTR_TEX_I4              = 5,  // Intensity 4-bit
    OTR_TEX_I8              = 6,  // Intensity 8-bit
    OTR_TEX_IA4             = 7,  // Intensity+Alpha 4-bit (3+1)
    OTR_TEX_IA8             = 8,  // Intensity+Alpha 8-bit (4+4)
    OTR_TEX_IA16            = 9,  // Intensity+Alpha 16-bit (8+8)
};

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
    // RTX is active once the DX12 device is initialized OR has a shared swap chain
    // (bridge mode — device may complete initialization lazily on first present).
    bool result = s_instance && s_instance->m_device && 
                  (s_instance->m_device->IsInitialized() || s_instance->m_device->HasSharedSwapChain());
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
    , m_giSystem(std::make_unique<GISystem>())
    , m_uiCompositor(std::make_unique<UICompositor>()) {
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
    // Use DX12Device context so TextureManager shares the renderer's command queue.
    if (!TextureManager::GetInstance().Initialize(m_device.get())) {
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

    // Initialize UI compositor for HUD overlay (optional - non-fatal if it fails)
    RTX_DIAG("RTXRenderer::CompleteInitialization() initializing UI compositor...");
    if (m_uiCompositor && !m_uiCompositor->Initialize(m_device.get(), width, height)) {
        RTX_DIAG("RTXRenderer::CompleteInitialization() UI compositor init deferred (non-fatal)");
        SPDLOG_WARN("[RTX] UI compositor initialization deferred — HUD compositing unavailable");
    } else {
        RTX_DIAG("RTXRenderer::CompleteInitialization() UI compositor OK");
    }

    // Initialize screenshot capture system (with null safety for swap chain)
    {
        auto* d3dDevice = m_device->GetDevice();
        auto* cmdQueue = m_device->GetCommandQueue();
        auto* swapChain = m_device->GetSwapChain();
        if (d3dDevice && cmdQueue) {
            if (swapChain) {
                RTXScreenCapture::Initialize(d3dDevice, cmdQueue, swapChain);
            } else {
                RTX_DIAG("RTXRenderer::CompleteInitialization() swap chain null - screenshot capture deferred");
                RTXScreenCapture::Initialize(d3dDevice, cmdQueue);
            }
            RTXScreenCapture::SetAutoCapture(60); // Capture every 60 frames (~1 second) for faster diagnostic feedback
#ifdef _WIN32
            CreateDirectoryA("screenshots", nullptr);
#endif
            RTXScreenCapture::SetOutputDir("screenshots/");
        } else {
            RTX_DIAG("RTXRenderer::CompleteInitialization() device or queue null - screenshot capture unavailable");
            OutputDebugStringA("[RTX] WARNING: Screenshot capture unavailable - null device or queue\n");
        }
    }

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
    // Use DX12Device context so TextureManager shares the renderer's command queue.
    // This avoids cross-queue synchronization issues where textures uploaded on a
    // separate queue are not visible to the render queue during DispatchRays.
    if (!TextureManager::GetInstance().Initialize(m_device.get())) {
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

    // Initialize UI compositor for HUD overlay (optional - non-fatal if it fails)
    RTX_DIAG("RTXRenderer::Initialize() initializing UI compositor...");
    if (m_uiCompositor && !m_uiCompositor->Initialize(m_device.get(), width, height)) {
        RTX_DIAG("RTXRenderer::Initialize() UI compositor init deferred (non-fatal)");
        SPDLOG_WARN("[RTX] UI compositor initialization deferred — HUD compositing unavailable");
    } else {
        RTX_DIAG("RTXRenderer::Initialize() UI compositor OK");
    }

    // Initialize screenshot capture system (with null safety for swap chain)
    {
        auto* d3dDevice = m_device->GetDevice();
        auto* cmdQueue = m_device->GetCommandQueue();
        auto* swapChain = m_device->GetSwapChain();
        if (d3dDevice && cmdQueue) {
            if (swapChain) {
                RTXScreenCapture::Initialize(d3dDevice, cmdQueue, swapChain);
            } else {
                RTX_DIAG("RTXRenderer::Initialize() swap chain null - screenshot capture deferred");
                RTXScreenCapture::Initialize(d3dDevice, cmdQueue);
            }
            RTXScreenCapture::SetAutoCapture(60); // Capture every 60 frames (~1 second) for faster diagnostic feedback
#ifdef _WIN32
            CreateDirectoryA("screenshots", nullptr);
#endif
            RTXScreenCapture::SetOutputDir("screenshots/");
        } else {
            RTX_DIAG("RTXRenderer::Initialize() device or queue null - screenshot capture unavailable");
            OutputDebugStringA("[RTX] WARNING: Screenshot capture unavailable - null device or queue\n");
        }
    }

    return true;
}

bool RTXRenderer::CompleteInitializationFromBridge() {
    RTX_DIAG("RTXRenderer::CompleteInitializationFromBridge() called");
    printf("[RTX] RTXRenderer::CompleteInitializationFromBridge()\n");
    
    if (!m_device) {
        RTX_DIAG("RTXRenderer::CompleteInitializationFromBridge() FAILED - no DX12Device");
        return false;
    }
    
    // The DX12Device should already have the shared swap chain set via SetSharedSwapChain()
    if (!m_device->HasSharedSwapChain()) {
        RTX_DIAG("RTXRenderer::CompleteInitializationFromBridge() FAILED - no shared swap chain");
        return false;
    }
    
    // Complete DX12 device init with shared swap chain
    uint32_t w = m_device->GetWidth();
    uint32_t h = m_device->GetHeight();
    if (w == 0 || h == 0) {
        w = 1280; h = 960; // fallback
    }
    
    if (!m_device->CompleteInitializationWithSharedSwapChain(w, h)) {
        RTX_DIAG("RTXRenderer::CompleteInitializationFromBridge() FAILED - device init failed");
        return false;
    }
    
    RTX_DIAG("RTXRenderer::CompleteInitializationFromBridge() DX12 device ready, initializing pipeline...");
    
    // Initialize the DXR pipeline
    if (!m_pipeline->Initialize(m_device.get(), w, h)) {
        RTX_DIAG("RTXRenderer::CompleteInitializationFromBridge() FAILED - DXR pipeline init");
        SPDLOG_ERROR("[RTX] Bridge: Failed to initialize DXR pipeline");
        // Don't shutdown device — it's still usable for magenta test
    }
    
    // Initialize acceleration structures
    if (!m_accelerationStructure->Initialize(m_device.get())) {
        RTX_DIAG("RTXRenderer::CompleteInitializationFromBridge() accel struct init FAILED (non-fatal for diagnostic)");
    }
    
    // Initialize GI system
    if (!m_giSystem->Initialize(m_device.get())) {
        RTX_DIAG("RTXRenderer::CompleteInitializationFromBridge() GI system init FAILED (non-fatal)");
    }
    
    // Initialize TextureManager
    if (!TextureManager::GetInstance().Initialize(m_device.get())) {
        RTX_DIAG("RTXRenderer::CompleteInitializationFromBridge() TextureManager init FAILED (non-fatal)");
    }
    
    // Create UAV descriptors
    m_pipeline->CreateUAVDescriptors();
    
    // Initialize UI compositor
    if (m_uiCompositor) {
        m_uiCompositor->Initialize(m_device.get(), w, h);
    }
    
    RTX_DIAG("RTXRenderer::CompleteInitializationFromBridge() SUCCESS");
    printf("[RTX] RTXRenderer::CompleteInitializationFromBridge() SUCCESS (%ux%u)\n", w, h);
    
    // Log to diagnostic file
    {
        FILE* f = fopen("rtx_bridge_debug.log", "a");
        if (f) {
            fprintf(f, "=== RTXRenderer CompleteInitializationFromBridge SUCCESS ===\n");
            fprintf(f, "  %ux%u device=%p cmdQueue=%p swapChain=%p\n",
                    w, h,
                    (void*)m_device->GetDevice(),
                    (void*)m_device->GetCommandQueue(),
                    (void*)m_device->GetSwapChain());
            fflush(f);
            fclose(f);
        }
    }
    
    return true;
}

void RTXRenderer::RenderAndPresentFrame() {
    static uint32_t s_frameCount = 0;
    s_frameCount++;

    // Direct file logging for crash diagnosis on early frames
    if (s_frameCount <= 5) {
        FILE* f = fopen("rtx_bridge_debug.log", "a");
        if (f) {
            fprintf(f, "RenderAndPresentFrame #%u ENTRY: device=%p init=%s sharedSC=%s\n",
                    s_frameCount,
                    (void*)m_device.get(),
                    m_device->IsInitialized() ? "YES" : "NO",
                    m_device->HasSharedSwapChain() ? "YES" : "NO");
            fflush(f);
            fclose(f);
        }
    }
    
    // Lazy initialization: if the device isn't fully initialized yet but we have
    // a shared swap chain, complete initialization now.
    if (!m_device->IsInitialized() && m_device->HasSharedSwapChain()) {
        {
            FILE* f = fopen("rtx_bridge_debug.log", "a");
            if (f) {
                fprintf(f, "RenderAndPresentFrame #%u: starting lazy init from bridge...\n", s_frameCount);
                fflush(f);
                fclose(f);
            }
        }
        RTX_DIAG("RTXRenderer::RenderAndPresentFrame() lazy init from bridge");
        CompleteInitializationFromBridge();
        {
            FILE* f = fopen("rtx_bridge_debug.log", "a");
            if (f) {
                fprintf(f, "RenderAndPresentFrame #%u: lazy init completed, device init=%s\n",
                        s_frameCount, m_device->IsInitialized() ? "YES" : "NO");
                fflush(f);
                fclose(f);
            }
        }
    }
    
    if (!m_device->IsInitialized()) {
        if (s_frameCount <= 5) {
            RTX_DIAG("RTXRenderer::RenderAndPresentFrame() device not initialized, frame #%u", s_frameCount);
            FILE* f = fopen("rtx_bridge_debug.log", "a");
            if (f) {
                fprintf(f, "RenderAndPresentFrame #%u: device NOT initialized, returning early\n", s_frameCount);
                fflush(f);
                fclose(f);
            }
        }
        return;
    }

    // Pre-flight check: log critical state before entering the full pipeline.
    // This helps diagnose crashes on early frames.
    if (s_frameCount <= 5) {
        bool hasPipeline = m_pipeline != nullptr;
        bool hasAccel = m_accelerationStructure != nullptr;
        bool hasGI = m_giSystem != nullptr;
        bool sceneActive = IsRTXSceneActive();
        uint32_t instances = hasAccel ? m_accelerationStructure->GetInstanceCount() : 0;
        ID3D12StateObject* stateObj = hasPipeline ? m_pipeline->GetStateObject() : nullptr;

        char preflightMsg[512];
        snprintf(preflightMsg, sizeof(preflightMsg),
                 "[RTX] RenderAndPresentFrame preflight (frame #%u): sceneActive=%s, pipeline=%p, "
                 "stateObj=%p, accel=%p, instances=%u, gi=%p, sceneLoaded=%s, scene=0x%02X\n",
                 s_frameCount, sceneActive ? "YES" : "NO",
                 (void*)m_pipeline.get(), (void*)stateObj,
                 (void*)m_accelerationStructure.get(), instances,
                 (void*)m_giSystem.get(),
                 m_sceneLoaded ? "YES" : "NO", m_currentScene);
        OutputDebugStringA(preflightMsg);
        printf("%s", preflightMsg);
        RTX_DIAG("%s", preflightMsg);
        fflush(stdout);
    }

    // Delegate to the real raytracing pipeline
    DispatchAndPresent();
}

void RTXRenderer::Shutdown() {
    RTX_DIAG("RTXRenderer::Shutdown() starting (scene=%d, sceneLoaded=%s)",
             m_currentScene, m_sceneLoaded ? "yes" : "no");
    // Shut down the TextureManager singleton (Device5 path) first
    TextureManager::GetInstance().Shutdown();

    // Shutdown in reverse initialization order:
    // Init:     DX12Device -> DXRPipeline -> AccelStruct -> GISystem -> UICompositor -> TextureManager(singleton)
    // Shutdown: TextureManager(singleton) -> UICompositor -> GISystem -> AccelStruct -> DXRPipeline -> DX12Device
    if (m_uiCompositor) m_uiCompositor->Shutdown();
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
    m_allRoomsLoaded = false;
    m_roomGeometry.clear();

    // Reset the texture re-resolve counter so this scene gets aggressive
    // re-resolution from frame 0 (every frame for 60 frames, then tapering).
    // Without this, a stale high counter from the previous scene causes new
    // textures to only re-resolve every 30 frames, leaving white geometry.
    m_reResolveCounter = 0;
    RTX_DIAG("RTXRenderer::OnSceneLoaded() reset m_reResolveCounter to 0");

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
    m_sceneConstantsValid = false;  // Reset so next scene gets fresh defaults
    // Clear scene constants completely to prevent stale camera data from the previous
    // scene bleeding into the first frame of the new scene. Without this, the camera
    // position from the old scene persists until UpdateSceneParams() is called, which
    // can cause the camera to appear "stuck below the scene" for scenes with very
    // different geometry positions (e.g., transitioning from Kokiri Forest to Zora's Domain).
    memset(&m_sceneConstants, 0, sizeof(m_sceneConstants));
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
    if (m_pipeline) {
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
    } else {
        RTX_DIAG("RTXRenderer::OnRoomLoaded() room %d: pipeline null, cannot update hit group table", roomNum);
        OutputDebugStringA("[RTX] WARNING: pipeline null during OnRoomLoaded hit group table update\n");
    }

    SPDLOG_INFO("[RTX] Room {} fully loaded: BLAS built, {} instances total",
                roomNum, m_accelerationStructure->GetInstanceCount());

    // After loading the first room, proactively load all other rooms in the scene.
    // OoT normally loads rooms on-demand, but for RTX we want the entire scene visible.
    if (!m_allRoomsLoaded) {
        LoadAllRoomsProactively(playPtr);
    }
}

void RTXRenderer::LoadAllRoomsProactively(void* playPtr) {
    // OoT loads rooms on-demand (1 at a time), so the TLAS normally only has 1 room.
    // For RTX we want ALL rooms loaded into the TLAS at scene init so the entire
    // scene is visible. This function force-loads every room's resource, extracts
    // its MeshHeader via the SetMesh scene command, builds a temporary Room struct,
    // and runs the normal geometry extraction + BLAS building pipeline.
    
    PlayState* play = (PlayState*)playPtr;
    if (!play || play->numRooms <= 1) {
        RTX_DIAG("LoadAllRoomsProactively: skipping (play=%p, numRooms=%d)", playPtr, play ? play->numRooms : 0);
        return;
    }

    RTX_DIAG("LoadAllRoomsProactively: scene=0x%02X, numRooms=%d, loading all rooms...", m_currentScene, play->numRooms);

    for (int i = 0; i < play->numRooms; i++) {
        // Skip the room that was already loaded normally by the game
        bool alreadyLoaded = false;
        for (const auto& rg : m_roomGeometry) {
            if (rg.roomIndex == (uint32_t)i) {
                alreadyLoaded = true;
                break;
            }
        }
        if (alreadyLoaded) {
            RTX_DIAG("LoadAllRoomsProactively: room %d already loaded, skipping", i);
            continue;
        }

        // Get the room's resource file name from the scene's room list
        if (!play->roomList || !play->roomList[i].fileName) {
            RTX_DIAG("LoadAllRoomsProactively: room %d has no fileName, skipping", i);
            continue;
        }

        const char* roomFileName = play->roomList[i].fileName;
        RTX_DIAG("LoadAllRoomsProactively: loading room %d from '%s'", i, roomFileName);

        // Load the room resource directly (bypasses game's DMA/RoomContext)
        std::shared_ptr<Ship::IResource> roomResource;
        try {
            roomResource = ResourceMgr_GetResourceByNameHandlingMQ(roomFileName);
        } catch (...) {
            RTX_DIAG("LoadAllRoomsProactively: room %d resource load threw exception, skipping", i);
            continue;
        }
        if (!roomResource) {
            RTX_DIAG("LoadAllRoomsProactively: room %d resource load returned null, skipping", i);
            continue;
        }

        // The room resource is a SOH::Scene. Find the SetMesh command to get the MeshHeader.
        auto* sceneRes = dynamic_cast<SOH::Scene*>(roomResource.get());
        if (!sceneRes) {
            RTX_DIAG("LoadAllRoomsProactively: room %d resource is not a Scene, skipping", i);
            continue;
        }

        SOH::SetMesh* meshCmd = nullptr;
        for (auto& cmd : sceneRes->commands) {
            if (cmd && cmd->cmdId == SOH::SceneCommandID::SetMesh) {
                meshCmd = dynamic_cast<SOH::SetMesh*>(cmd.get());
                break;
            }
        }
        if (!meshCmd) {
            RTX_DIAG("LoadAllRoomsProactively: room %d has no SetMesh command, skipping", i);
            continue;
        }

        // Build a temporary Room struct with the MeshHeader from the resource.
        // ExtractRoomGeometry only needs room->meshHeader to be set.
        Room tempRoom = {};
        tempRoom.num = (s8)i;
        tempRoom.meshHeader = (::MeshHeader*)meshCmd->GetRawPointer();
        tempRoom.segment = (void*)roomResource.get();  // non-null so the null check passes

        if (!tempRoom.meshHeader) {
            RTX_DIAG("LoadAllRoomsProactively: room %d MeshHeader is null, skipping", i);
            continue;
        }

        // We need gSegments[3] set correctly for SEGMENTED_TO_VIRTUAL resolution of
        // the PolygonDlist array pointer. For OTR resources, the dlist start pointer
        // was already patched by SetMeshFactory to point to the dlists vector data,
        // but SEGMENTED_TO_VIRTUAL may still try to use gSegments[3].
        // Save and restore the current segment 3. (gSegments declared at file scope above)
        uintptr_t savedSeg3 = gSegments[3];
        gSegments[3] = (uintptr_t)roomResource.get();

        RTX_DIAG("LoadAllRoomsProactively: extracting geometry for room %d (meshType=%d)", i, tempRoom.meshHeader->base.type);

        RoomGeometry geometry = m_geometryExtractor->ExtractRoomGeometry(&tempRoom, (uint32_t)i);

        // Restore segment 3
        gSegments[3] = savedSeg3;

        bool hasGeometry = !geometry.opaqueMesh.vertices.empty() || !geometry.alphaMesh.vertices.empty();
        RTX_DIAG("LoadAllRoomsProactively: room %d: hasGeometry=%s, opaqueVerts=%zu, alphaVerts=%zu",
                 i, hasGeometry ? "yes" : "no",
                 geometry.opaqueMesh.vertices.size(), geometry.alphaMesh.vertices.size());

        if (!hasGeometry) continue;

        // Resolve material textures and build BLAS (same as OnRoomLoaded)
        ResolveMaterialTextures(geometry);

        if (!m_accelerationStructure->BuildBLAS(geometry)) {
            RTX_DIAG("LoadAllRoomsProactively: room %d BLAS build FAILED", i);
            continue;
        }

        m_roomGeometry.push_back(std::move(geometry));

        // Update hit group shader table
        if (m_pipeline) {
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

        RTX_DIAG("LoadAllRoomsProactively: room %d loaded successfully (%zu instances total)",
                 i, m_accelerationStructure->GetInstanceCount());
    }

    m_allRoomsLoaded = true;
    RTX_DIAG("LoadAllRoomsProactively: DONE. Total BLAS instances: %zu, total room geometries: %zu",
             m_accelerationStructure->GetInstanceCount(), m_roomGeometry.size());
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

    // =========================================================================
    // NEW LAYOUT: Fill the 384-byte SceneConstants struct.
    // This layout uses full 4x4 matrices instead of extracted basis vectors,
    // eliminating potential row/column-major interpretation mismatches.
    // =========================================================================

    // Zero the entire struct first to ensure no stale data
    memset(&m_sceneConstants, 0, sizeof(m_sceneConstants));

    // --- View Matrix (offset 0, 64 bytes) ---
    // Guard against degenerate (all-zero) view matrices during scene transitions.
    {
        float rightMag = viewMatrix[0]*viewMatrix[0] + viewMatrix[1]*viewMatrix[1] + viewMatrix[2]*viewMatrix[2];
        float upMag = viewMatrix[4]*viewMatrix[4] + viewMatrix[5]*viewMatrix[5] + viewMatrix[6]*viewMatrix[6];

        if (rightMag > 0.001f && upMag > 0.001f) {
            memcpy(m_sceneConstants.viewMatrix, viewMatrix, sizeof(float) * 16);
        } else {
            // Degenerate view matrix — use identity
            m_sceneConstants.viewMatrix[0] = 1.0f;
            m_sceneConstants.viewMatrix[5] = 1.0f;
            m_sceneConstants.viewMatrix[10] = 1.0f;
            m_sceneConstants.viewMatrix[15] = 1.0f;
            RTX_DIAG("RTXRenderer: Degenerate view matrix (rightMag=%.6f, upMag=%.6f), using identity (scene=0x%02X)",
                     rightMag, upMag, m_currentScene);
        }
    }

    // --- Projection Matrix (offset 64, 64 bytes) ---
    {
        bool projValid = (projMatrix[0] != 0.0f && projMatrix[5] != 0.0f);
        if (projValid) {
            memcpy(m_sceneConstants.projMatrix, projMatrix, sizeof(float) * 16);
        } else {
            // Fallback projection (60 degree vertical FOV, 4:3 aspect)
            float fovY = 1.047f; // 60 degrees in radians
            float aspect = 4.0f / 3.0f;
            float nearZ = 10.0f;
            float farZ = 12800.0f;
            float tanHalfFovY = tanf(fovY * 0.5f);
            m_sceneConstants.projMatrix[0] = 1.0f / (aspect * tanHalfFovY);
            m_sceneConstants.projMatrix[5] = 1.0f / tanHalfFovY;
            m_sceneConstants.projMatrix[10] = farZ / (farZ - nearZ);
            m_sceneConstants.projMatrix[11] = 1.0f;
            m_sceneConstants.projMatrix[14] = -(nearZ * farZ) / (farZ - nearZ);
        }
    }

    // --- Inverse View Matrix (offset 128, 64 bytes) ---
    // For an orthonormal view matrix V (row-major) with last row [0,0,0,1]:
    //   V = [Rx Ry Rz tx]   where tx = -dot(R, eye)
    //       [Ux Uy Uz ty]         ty = -dot(U, eye)
    //       [Lx Ly Lz tz]         tz = -dot(L, eye)
    //       [0  0  0  1 ]
    // The inverse is:
    //   V^-1 = [Rx Ux Lx ex]   (transpose the 3x3 rotation part)
    //          [Ry Uy Ly ey]   ex = -(Rx*tx + Ux*ty + Lx*tz)
    //          [Rz Uz Lz ez]   ey = -(Ry*tx + Uy*ty + Ly*tz)
    //          [0  0  0  1 ]   ez = -(Rz*tx + Uz*ty + Lz*tz)
    // This is more numerically robust than the generic cofactor inverse,
    // which produces zero in positions [12-14] due to the [0,0,0,1] row
    // zeroing out all cofactor terms.
    {
        const float* V = m_sceneConstants.viewMatrix;
        float* I = m_sceneConstants.invViewMatrix;
        // Transpose the 3x3 rotation
        I[0]  = V[0]; I[1]  = V[4]; I[2]  = V[8];
        I[4]  = V[1]; I[5]  = V[5]; I[6]  = V[9];
        I[8]  = V[2]; I[9]  = V[6]; I[10] = V[10];
        // Translation: eye = -R^T * t
        float tx = V[3], ty = V[7], tz = V[11];
        I[3]  = -(I[0]*tx + I[1]*ty + I[2]*tz);
        I[7]  = -(I[4]*tx + I[5]*ty + I[6]*tz);
        I[11] = -(I[8]*tx + I[9]*ty + I[10]*tz);
        // Last row
        I[12] = 0.0f; I[13] = 0.0f; I[14] = 0.0f; I[15] = 1.0f;
    }

    // --- Inverse Projection Matrix (offset 192, 64 bytes) ---
    InvertMatrix4x4(m_sceneConstants.projMatrix, m_sceneConstants.invProjMatrix);

    // Log view/proj matrix details and camera position from invViewMatrix
    {
        static uint32_t s_matLogCount = 0;
        s_matLogCount++;
        if (s_matLogCount <= 5 || (s_matLogCount % 60) == 0) {
            // The shader reads camera position from invViewMatrix column 3 (indices 3,7,11)
            RTX_DIAG("UpdateSceneParams #%u: inputCamPos=(%.1f,%.1f,%.1f) invViewMtx[3,7,11]=(%.1f,%.1f,%.1f) viewMtx[3,7,11]=(%.4f,%.4f,%.4f)",
                     s_matLogCount,
                     cameraPos[0], cameraPos[1], cameraPos[2],
                     m_sceneConstants.invViewMatrix[3], m_sceneConstants.invViewMatrix[7], m_sceneConstants.invViewMatrix[11],
                     m_sceneConstants.viewMatrix[3], m_sceneConstants.viewMatrix[7], m_sceneConstants.viewMatrix[11]);
        }
    }

    // --- Sun Direction (offset 256, float4) ---
    {
        float sunDir[3] = { m_currentSceneConfig.sunDirection[0],
                            m_currentSceneConfig.sunDirection[1],
                            m_currentSceneConfig.sunDirection[2] };
        // Normalize
        float sunDirLen = sqrtf(sunDir[0]*sunDir[0] + sunDir[1]*sunDir[1] + sunDir[2]*sunDir[2]);
        if (sunDirLen > 0.001f) {
            sunDir[0] /= sunDirLen;
            sunDir[1] /= sunDirLen;
            sunDir[2] /= sunDirLen;
        } else {
            // Fallback: sun from upper-left
            sunDir[0] = 0.5f; sunDir[1] = 0.8f; sunDir[2] = 0.3f;
            float len = sqrtf(sunDir[0]*sunDir[0] + sunDir[1]*sunDir[1] + sunDir[2]*sunDir[2]);
            sunDir[0] /= len; sunDir[1] /= len; sunDir[2] /= len;
        }
        m_sceneConstants.sunDirection[0] = sunDir[0];
        m_sceneConstants.sunDirection[1] = sunDir[1];
        m_sceneConstants.sunDirection[2] = sunDir[2];
        m_sceneConstants.sunDirection[3] = 0.0f;
    }

    // --- Sun Color (offset 272, float4) --- warm WHITE, NOT green
    m_sceneConstants.sunColor[0] = m_currentSceneConfig.sunColor[0];
    m_sceneConstants.sunColor[1] = m_currentSceneConfig.sunColor[1];
    m_sceneConstants.sunColor[2] = m_currentSceneConfig.sunColor[2];
    m_sceneConstants.sunColor[3] = 1.0f;

    // --- Ambient Color (offset 288, float4) ---
    if (m_currentSceneConfig.enabled) {
        m_sceneConstants.ambientColor[0] = m_currentSceneConfig.ambientColor[0];
        m_sceneConstants.ambientColor[1] = m_currentSceneConfig.ambientColor[1];
        m_sceneConstants.ambientColor[2] = m_currentSceneConfig.ambientColor[2];
    } else {
        m_sceneConstants.ambientColor[0] = ambientColor[0];
        m_sceneConstants.ambientColor[1] = ambientColor[1];
        m_sceneConstants.ambientColor[2] = ambientColor[2];
    }
    m_sceneConstants.ambientColor[3] = 1.0f;

    // --- Fog Color (offset 304, float4) ---
    // Apply scene config fog color overrides (non-zero values override game values)
    if (m_currentSceneConfig.fogColorDefault[0] > 0.0f ||
        m_currentSceneConfig.fogColorDefault[1] > 0.0f ||
        m_currentSceneConfig.fogColorDefault[2] > 0.0f) {
        m_sceneConstants.fogColor[0] = m_currentSceneConfig.fogColorDefault[0];
        m_sceneConstants.fogColor[1] = m_currentSceneConfig.fogColorDefault[1];
        m_sceneConstants.fogColor[2] = m_currentSceneConfig.fogColorDefault[2];
    } else {
        m_sceneConstants.fogColor[0] = fogColor[0];
        m_sceneConstants.fogColor[1] = fogColor[1];
        m_sceneConstants.fogColor[2] = fogColor[2];
    }
    m_sceneConstants.fogColor[3] = 1.0f;

    // --- Fog distances (offset 320-327) ---
    {
        float actualFogNear = fogNear;
        if (sceneSetupIndex == 6) {
            actualFogNear = roomUnk74 + 500.0f;
        } else if ((sceneSetupIndex < 4 || isAdultLink) && dekuTreeDead) {
            actualFogNear = 2150.0f;
        }
        if (m_currentSceneConfig.fogNearDefault > 0.0f) {
            actualFogNear = m_currentSceneConfig.fogNearDefault;
        }

        m_sceneConstants.fogStart = actualFogNear;
        m_sceneConstants.fogEnd = (m_currentSceneConfig.fogFarDefault > 0.0f)
            ? m_currentSceneConfig.fogFarDefault : fogFar;
    }

    // --- Sun and ambient intensity (offset 328-332) ---
    m_sceneConstants.sunIntensity = m_currentSceneConfig.sunIntensity;
    m_sceneConstants.ambientIntensity = m_currentSceneConfig.ambientIntensity;

    // --- GI, reflection, sky, exposure (offset 336-348) ---
    m_sceneConstants.giIntensity = m_currentSceneConfig.giIntensity;
    m_sceneConstants.reflectionIntensity = m_currentSceneConfig.waterReflectivity;
    m_sceneConstants.skyIntensity = 1.0f;
    m_sceneConstants.exposure = m_currentSceneConfig.exposure;

    // --- Frame count, tone map, sky blend, debug mode (offset 352-364) ---
    m_sceneConstants.frameCount = accumulationFrameCount;
    m_sceneConstants.toneMapMode = m_currentSceneConfig.toneMapMode;
    m_sceneConstants.skyBlendFactor = 0.5f;
    m_sceneConstants.debugMode = m_debugMode;

    // --- Padding (offset 368-380) ---
    m_sceneConstants.pad0 = 0.0f;
    m_sceneConstants.pad1 = 0.0f;
    m_sceneConstants.pad2 = 0.0f;
    m_sceneConstants.pad3 = 0.0f;

    // Log final scene constants for verification (first 5 frames and periodically)
    {
        static uint32_t s_sunLogCount = 0;
        s_sunLogCount++;
        if (s_sunLogCount <= 5 || (s_sunLogCount % 300) == 0) {
            RTX_DIAG("SceneConstants FINAL sunDir=(%.3f,%.3f,%.3f) sunColor=(%.3f,%.3f,%.3f) sunInt=%.2f ambient=(%.3f,%.3f,%.3f) ambInt=%.2f gi=%.2f exp=%.2f debug=%d sizeof=%zu",
                     m_sceneConstants.sunDirection[0], m_sceneConstants.sunDirection[1], m_sceneConstants.sunDirection[2],
                     m_sceneConstants.sunColor[0], m_sceneConstants.sunColor[1], m_sceneConstants.sunColor[2],
                     m_sceneConstants.sunIntensity,
                     m_sceneConstants.ambientColor[0], m_sceneConstants.ambientColor[1], m_sceneConstants.ambientColor[2],
                     m_sceneConstants.ambientIntensity,
                     m_sceneConstants.giIntensity, m_sceneConstants.exposure, m_sceneConstants.debugMode,
                     sizeof(SceneConstants));
            // Also printf to console for reliable visibility
            printf("RTX CB: sun=(%f,%f,%f) amb=(%f,%f,%f) fog=(%f,%f,%f) sunI=%f ambI=%f exp=%f debug=%d\n",
                   m_sceneConstants.sunColor[0], m_sceneConstants.sunColor[1], m_sceneConstants.sunColor[2],
                   m_sceneConstants.ambientColor[0], m_sceneConstants.ambientColor[1], m_sceneConstants.ambientColor[2],
                   m_sceneConstants.fogColor[0], m_sceneConstants.fogColor[1], m_sceneConstants.fogColor[2],
                   m_sceneConstants.sunIntensity, m_sceneConstants.ambientIntensity, m_sceneConstants.exposure,
                   m_sceneConstants.debugMode);
        }
    }

    // =========================================================================
    // DIAGNOSTIC FILE DUMP: Write CB values to disk for reliable inspection.
    // OutputDebugString can be missed; a file on disk is always readable.
    // Writes on frames 1,2,3 and every 600 frames thereafter.
    // =========================================================================
    {
        static uint32_t s_fileDumpCount = 0;
        s_fileDumpCount++;
        if (s_fileDumpCount <= 3 || (s_fileDumpCount % 600) == 0) {
            FILE* dumpFile = fopen("rtx_cb_dump.txt", (s_fileDumpCount == 1) ? "w" : "a");
            if (dumpFile) {
                fprintf(dumpFile, "\n====== SceneConstants Dump (frame %u, scene=0x%02X) ======\n", s_fileDumpCount, m_currentScene);
                fprintf(dumpFile, "sizeof(SceneConstants) = %zu (expected 384)\n", sizeof(SceneConstants));
                fprintf(dumpFile, "--- Matrices ---\n");
                fprintf(dumpFile, "  viewMatrix[0..3]:  %.6f %.6f %.6f %.6f\n", m_sceneConstants.viewMatrix[0], m_sceneConstants.viewMatrix[1], m_sceneConstants.viewMatrix[2], m_sceneConstants.viewMatrix[3]);
                fprintf(dumpFile, "  viewMatrix[4..7]:  %.6f %.6f %.6f %.6f\n", m_sceneConstants.viewMatrix[4], m_sceneConstants.viewMatrix[5], m_sceneConstants.viewMatrix[6], m_sceneConstants.viewMatrix[7]);
                fprintf(dumpFile, "  viewMatrix[8..11]: %.6f %.6f %.6f %.6f\n", m_sceneConstants.viewMatrix[8], m_sceneConstants.viewMatrix[9], m_sceneConstants.viewMatrix[10], m_sceneConstants.viewMatrix[11]);
                fprintf(dumpFile, "  viewMatrix[12..15]: %.6f %.6f %.6f %.6f\n", m_sceneConstants.viewMatrix[12], m_sceneConstants.viewMatrix[13], m_sceneConstants.viewMatrix[14], m_sceneConstants.viewMatrix[15]);
                fprintf(dumpFile, "  projMatrix[0,5,10,11,14]: %.6f %.6f %.6f %.6f %.6f\n",
                        m_sceneConstants.projMatrix[0], m_sceneConstants.projMatrix[5],
                        m_sceneConstants.projMatrix[10], m_sceneConstants.projMatrix[11],
                        m_sceneConstants.projMatrix[14]);
                fprintf(dumpFile, "  invViewMatrix[12..15]: %.6f %.6f %.6f %.6f (camera pos in [12,13,14])\n",
                        m_sceneConstants.invViewMatrix[12], m_sceneConstants.invViewMatrix[13],
                        m_sceneConstants.invViewMatrix[14], m_sceneConstants.invViewMatrix[15]);
                fprintf(dumpFile, "--- Lighting ---\n");
                fprintf(dumpFile, "  sunDirection:   (%.6f, %.6f, %.6f, %.6f)\n",
                        m_sceneConstants.sunDirection[0], m_sceneConstants.sunDirection[1],
                        m_sceneConstants.sunDirection[2], m_sceneConstants.sunDirection[3]);
                fprintf(dumpFile, "  sunColor:       (%.6f, %.6f, %.6f, %.6f)\n",
                        m_sceneConstants.sunColor[0], m_sceneConstants.sunColor[1],
                        m_sceneConstants.sunColor[2], m_sceneConstants.sunColor[3]);
                fprintf(dumpFile, "  ambientColor:   (%.6f, %.6f, %.6f, %.6f)\n",
                        m_sceneConstants.ambientColor[0], m_sceneConstants.ambientColor[1],
                        m_sceneConstants.ambientColor[2], m_sceneConstants.ambientColor[3]);
                fprintf(dumpFile, "  fogColor:       (%.6f, %.6f, %.6f, %.6f)\n",
                        m_sceneConstants.fogColor[0], m_sceneConstants.fogColor[1],
                        m_sceneConstants.fogColor[2], m_sceneConstants.fogColor[3]);
                fprintf(dumpFile, "--- Scalars ---\n");
                fprintf(dumpFile, "  fogStart=%.2f  fogEnd=%.2f\n", m_sceneConstants.fogStart, m_sceneConstants.fogEnd);
                fprintf(dumpFile, "  sunIntensity=%.4f  ambientIntensity=%.4f\n", m_sceneConstants.sunIntensity, m_sceneConstants.ambientIntensity);
                fprintf(dumpFile, "  giIntensity=%.4f  reflectionIntensity=%.4f\n", m_sceneConstants.giIntensity, m_sceneConstants.reflectionIntensity);
                fprintf(dumpFile, "  skyIntensity=%.4f  exposure=%.4f\n", m_sceneConstants.skyIntensity, m_sceneConstants.exposure);
                fprintf(dumpFile, "--- Integer/Mixed ---\n");
                fprintf(dumpFile, "  frameCount=%u  toneMapMode=%u  skyBlendFactor=%.4f  debugMode=%d\n",
                        m_sceneConstants.frameCount, m_sceneConstants.toneMapMode,
                        m_sceneConstants.skyBlendFactor, m_sceneConstants.debugMode);
                fprintf(dumpFile, "  pad0=%.4f  pad1=%.4f  pad2=%.4f  pad3=%.4f\n",
                        m_sceneConstants.pad0, m_sceneConstants.pad1,
                        m_sceneConstants.pad2, m_sceneConstants.pad3);
                fprintf(dumpFile, "--- Computed GPU values ---\n");
                float gpuSunR = m_sceneConstants.sunColor[0] * m_sceneConstants.sunIntensity;
                float gpuSunG = m_sceneConstants.sunColor[1] * m_sceneConstants.sunIntensity;
                float gpuSunB = m_sceneConstants.sunColor[2] * m_sceneConstants.sunIntensity;
                fprintf(dumpFile, "  GPU sunColor*sunIntensity = (%.4f, %.4f, %.4f) [shader multiplies these]\n", gpuSunR, gpuSunG, gpuSunB);
                float gpuAmbR = m_sceneConstants.ambientColor[0] * m_sceneConstants.ambientIntensity;
                float gpuAmbG = m_sceneConstants.ambientColor[1] * m_sceneConstants.ambientIntensity;
                float gpuAmbB = m_sceneConstants.ambientColor[2] * m_sceneConstants.ambientIntensity;
                fprintf(dumpFile, "  GPU ambientColor*ambientIntensity = (%.4f, %.4f, %.4f)\n", gpuAmbR, gpuAmbG, gpuAmbB);
                fprintf(dumpFile, "--- Raw bytes at key offsets (hex) ---\n");
                const uint8_t* raw = (const uint8_t*)&m_sceneConstants;
                fprintf(dumpFile, "  offset 256 (sunDirection): %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\n",
                        raw[256], raw[257], raw[258], raw[259], raw[260], raw[261], raw[262], raw[263],
                        raw[264], raw[265], raw[266], raw[267], raw[268], raw[269], raw[270], raw[271]);
                fprintf(dumpFile, "  offset 272 (sunColor):     %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\n",
                        raw[272], raw[273], raw[274], raw[275], raw[276], raw[277], raw[278], raw[279],
                        raw[280], raw[281], raw[282], raw[283], raw[284], raw[285], raw[286], raw[287]);
                fprintf(dumpFile, "  offset 288 (ambientColor): %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\n",
                        raw[288], raw[289], raw[290], raw[291], raw[292], raw[293], raw[294], raw[295],
                        raw[296], raw[297], raw[298], raw[299], raw[300], raw[301], raw[302], raw[303]);
                fprintf(dumpFile, "  offset 320 (fogStart..):   %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\n",
                        raw[320], raw[321], raw[322], raw[323], raw[324], raw[325], raw[326], raw[327],
                        raw[328], raw[329], raw[330], raw[331], raw[332], raw[333], raw[334], raw[335]);
                fprintf(dumpFile, "  offset 352 (frameCount..): %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\n",
                        raw[352], raw[353], raw[354], raw[355], raw[356], raw[357], raw[358], raw[359],
                        raw[360], raw[361], raw[362], raw[363], raw[364], raw[365], raw[366], raw[367]);
                fprintf(dumpFile, "--- SceneConfig source values ---\n");
                fprintf(dumpFile, "  config.enabled=%d  config.sunIntensity=%.4f  config.ambientIntensity=%.4f\n",
                        m_currentSceneConfig.enabled, m_currentSceneConfig.sunIntensity, m_currentSceneConfig.ambientIntensity);
                fprintf(dumpFile, "  config.sunColor=(%.4f, %.4f, %.4f)\n",
                        m_currentSceneConfig.sunColor[0], m_currentSceneConfig.sunColor[1], m_currentSceneConfig.sunColor[2]);
                fprintf(dumpFile, "  config.ambientColor=(%.4f, %.4f, %.4f)\n",
                        m_currentSceneConfig.ambientColor[0], m_currentSceneConfig.ambientColor[1], m_currentSceneConfig.ambientColor[2]);
                fprintf(dumpFile, "  config.exposure=%.4f  config.toneMapMode=%u  config.denoiserEnabled=%d\n",
                        m_currentSceneConfig.exposure, m_currentSceneConfig.toneMapMode, m_currentSceneConfig.denoiserEnabled);
                fprintf(dumpFile, "  m_debugMode=%d (controls shader debug output)\n", m_debugMode);
                fprintf(dumpFile, "====== End CB Dump ======\n");
                fclose(dumpFile);
            }
        }
    }

    // Mark scene constants as valid (first UpdateSceneParams call initializes them)
    m_sceneConstantsValid = true;

    // Upload to GPU
    if (m_pipeline) {
        m_pipeline->UpdateSceneConstants(m_sceneConstants);
    }
}

bool RTXRenderer::IsRTXSceneActive() const {
    return m_sceneLoaded && IsRTXScene(static_cast<uint16_t>(m_currentScene));
}

// Forward declarations for helpers defined later in this file, used by DispatchAndPresent's re-resolve block.
static bool LooksLikeStringPointer(uintptr_t addr);
static uint32_t TryLoadOTRTexture(const char* otrPath, TextureManager& texMgr, const char* tlutPath = nullptr);

void RTXRenderer::DispatchAndPresent() {
    static uint32_t s_renderFrameCount = 0;
    s_renderFrameCount++;

    if (s_renderFrameCount <= 10 || (s_renderFrameCount % 300) == 0) {
        uint32_t instanceCount = m_accelerationStructure ? m_accelerationStructure->GetInstanceCount() : 0;
        bool deviceValid = m_device && m_device->IsInitialized();
        bool pipelineValid = m_pipeline && m_pipeline->GetStateObject();
        char frameMsg[512];
        snprintf(frameMsg, sizeof(frameMsg),
                 "[RTX] RTXRenderer::Render() frame #%u: scene=0x%02X, rtxActive=%s, device=%s, "
                 "pipeline=%s, stateObj=%p, TLASInstances=%u, sceneLoaded=%s\n",
                 s_renderFrameCount, m_currentScene,
                 IsRTXSceneActive() ? "YES" : "NO",
                 deviceValid ? "YES" : "NO",
                 m_pipeline ? "YES" : "NO",
                 m_pipeline ? (void*)m_pipeline->GetStateObject() : nullptr,
                 instanceCount,
                 m_sceneLoaded ? "YES" : "NO");
        OutputDebugStringA(frameMsg);
        printf("%s", frameMsg);
        RTX_DIAG("%s", frameMsg);
    }

    // For non-RTX scenes (or if scene not loaded), just clear and present
    if (!IsRTXSceneActive()) {
        if (s_renderFrameCount <= 5 || (s_renderFrameCount % 300) == 0) {
            RTX_DIAG("RTXRenderer::DispatchAndPresent() non-RTX scene path (frame #%u, scene=%d)", s_renderFrameCount, m_currentScene);
        }
        m_device->BeginFrame();
        auto* cmdList = m_device->GetCommandList();
        if (!cmdList) {
            RTX_DIAG("RTXRenderer: non-RTX path skipped - null command list after BeginFrame (frame #%u)", s_renderFrameCount);
            OutputDebugStringA("[RTX] CRITICAL: null command list after BeginFrame in non-RTX path\n");
            // BeginFrame was called but we can't record commands. Present to keep swap chain in sync.
            m_device->Present();
            return;
        }

        ID3D12Resource* bb = m_device->GetCurrentBackBuffer();
        if (!bb) {
            RTX_DIAG("RTXRenderer: non-RTX path skipped - null back buffer (frame #%u)", s_renderFrameCount);
            OutputDebugStringA("[RTX] CRITICAL: null back buffer in non-RTX path\n");
            // Close the command list empty and present to keep swap chain in sync.
            cmdList->Close();
            ID3D12CommandList* ppEmpty[] = { cmdList };
            auto* queue = m_device->GetCommandQueue();
            if (queue) queue->ExecuteCommandLists(1, ppEmpty);
            m_device->Present();
            return;
        }

        // Transition back buffer to render target
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = bb;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmdList->ResourceBarrier(1, &barrier);

        // Clear to dark blue while no RTX scene is active
        float clearColor[4] = { 0.05f, 0.05f, 0.15f, 1.0f };
        cmdList->ClearRenderTargetView(m_device->GetCurrentRTVHandle(), clearColor, 0, nullptr);

        // Transition back to present
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        cmdList->ResourceBarrier(1, &barrier);

        cmdList->Close();
        ID3D12CommandList* ppCmdLists[] = { cmdList };
        auto* cmdQueue = m_device->GetCommandQueue();
        if (cmdQueue) {
            cmdQueue->ExecuteCommandLists(1, ppCmdLists);
        } else {
            RTX_DIAG("RTXRenderer: non-RTX path - null command queue, cannot execute (frame #%u)", s_renderFrameCount);
            OutputDebugStringA("[RTX] CRITICAL: null command queue in non-RTX path\n");
        }
        // Save back buffer reference BEFORE Present (Present->MoveToNextFrame changes the frame index)
        ID3D12Resource* presentedBackBuffer = m_device->GetCurrentBackBuffer();
        D3D12_RESOURCE_DESC presentedBBDesc = presentedBackBuffer ? presentedBackBuffer->GetDesc() : D3D12_RESOURCE_DESC{};
        m_device->Present();
        // Capture screenshot after Present using the buffer that was just presented
        RTXScreenCapture::CheckKeyPress();
        if (presentedBackBuffer && m_device->GetCommandQueue()) {
            RTX_CaptureAfterPresent(presentedBackBuffer, m_device->GetCommandQueue(),
                                    presentedBBDesc.Format, static_cast<UINT>(presentedBBDesc.Width), presentedBBDesc.Height);
        }
        return;
    }

    // Advance the TextureManager's frame counter for LRU eviction tracking
    TextureManager::GetInstance().AdvanceFrame();

    // Re-resolve material textures periodically for textures that were not available
    // at OnRoomLoaded time but have since been intercepted via RTX_InterceptTexture.
    // This fixes the "white textures" issue where geometry loads before the game's
    // normal renderer has had a chance to render (and intercept) the textures.
    // Only re-resolve every N frames to avoid per-frame overhead.
    {
        m_reResolveCounter++;
        // Re-resolve aggressively for the first 180 frames (every frame for first 60,
        // every 2 frames for frames 61-180, then every 30 frames after that).
        // This ensures textures are resolved as soon as the OTR resources are available.
        // The longer aggressive window catches textures that load asynchronously.
        // The first 60 frames are every frame because the game may take several frames
        // to finish loading all OTR resources after a room loads.
        // m_reResolveCounter is reset to 0 in OnSceneLoaded() so each new scene
        // gets the full aggressive re-resolution window.
        bool shouldReResolve = (m_reResolveCounter <= 60) ||
                               (m_reResolveCounter <= 180 && (m_reResolveCounter % 2) == 0) ||
                               ((m_reResolveCounter % 30) == 0);
        if (shouldReResolve && !m_roomGeometry.empty()) {
            uint32_t totalUpdated = 0;
            for (auto& roomGeo : m_roomGeometry) {
                auto reResolve = [&](ExtractedMesh& mesh) {
                    auto& texMgr = TextureManager::GetInstance();
                    for (size_t i = 0; i < mesh.materials.size(); i++) {
                        Material& mat = mesh.materials[i];
                        // Only re-resolve materials that are still at default white (index 0)
                        if (mat.textureIndex != 0) continue;

                        // Use the durable OTR path string (preferred over raw pointer)
                        const char* otrPath = nullptr;
                        if (i < mesh.materialTexturePaths.size() && !mesh.materialTexturePaths[i].empty()) {
                            otrPath = mesh.materialTexturePaths[i].c_str();
                        }

                        // Get the TLUT path (for CI4/CI8 textures)
                        const char* tlutPath = nullptr;
                        if (i < mesh.materialTlutPaths.size() && !mesh.materialTlutPaths[i].empty()) {
                            tlutPath = mesh.materialTlutPaths[i].c_str();
                        }

                        uintptr_t textureAddr = 0;
                        if (i < mesh.materialTextureAddrs.size()) {
                            textureAddr = mesh.materialTextureAddrs[i];
                        }
                        if (textureAddr == 0 && !otrPath) continue;

                        // Strategy 1: Try OTR path hash lookup
                        uint32_t srvIndex = 0;
                        if (otrPath) {
                            constexpr uint64_t FNV_OFFSET = 0xcbf29ce484222325ULL;
                            constexpr uint64_t FNV_PRIME  = 0x100000001b3ULL;
                            uint64_t pathHash = FNV_OFFSET;
                            for (const char* p = otrPath; *p; p++) {
                                pathHash ^= (uint64_t)(uint8_t)*p;
                                pathHash *= FNV_PRIME;
                            }
                            srvIndex = texMgr.GetSRVIndexForHash(pathHash);
                        }

                        // Strategy 2: Try address-based hash
                        if (srvIndex == 0 && textureAddr != 0) {
                            constexpr uint64_t FNV_OFFSET = 0xcbf29ce484222325ULL;
                            constexpr uint64_t FNV_PRIME  = 0x100000001b3ULL;
                            uint64_t hash = FNV_OFFSET;
                            for (size_t b = 0; b < sizeof(textureAddr); b++) {
                                hash ^= static_cast<uint64_t>((textureAddr >> (b * 8)) & 0xFF);
                                hash *= FNV_PRIME;
                            }
                            srvIndex = texMgr.GetSRVIndexForHash(hash);
                        }

                        // Strategy 3: Try raw address as hash key
                        if (srvIndex == 0 && textureAddr != 0) {
                            srvIndex = texMgr.GetSRVIndexForHash(static_cast<uint64_t>(textureAddr));
                        }

                        // Strategy 4: Try EagerResolveOTRTexture with format info (preferred)
                        // This uses the stored per-material format info to ensure correct N64 decode.
                        if (srvIndex == 0 && otrPath) {
                            uint8_t texFmt = 0, texSiz = 0;
                            uint16_t texW = 0, texH = 0;
                            if (i < mesh.materialTexInfos.size()) {
                                texFmt = mesh.materialTexInfos[i].texFormat;
                                texSiz = mesh.materialTexInfos[i].texSize;
                                texW   = mesh.materialTexInfos[i].texWidth;
                                texH   = mesh.materialTexInfos[i].texHeight;
                            }
                            srvIndex = texMgr.EagerResolveOTRTexture(
                                otrPath, texFmt, texSiz, texW, texH, tlutPath);
                            // Also try with/without __OTR__ prefix
                            if (srvIndex == 0 && otrPath[0] != '_') {
                                std::string prefixedPath = std::string("__OTR__") + otrPath;
                                srvIndex = texMgr.EagerResolveOTRTexture(
                                    prefixedPath.c_str(), texFmt, texSiz, texW, texH, tlutPath);
                            }
                            if (srvIndex == 0 && otrPath[0] == '_' && otrPath[1] == '_' && strlen(otrPath) > 7) {
                                srvIndex = texMgr.EagerResolveOTRTexture(
                                    otrPath + 7, texFmt, texSiz, texW, texH, tlutPath);
                            }
                        }

                        // Strategy 5: Load from OTR directly via TryLoadOTRTexture (fallback)
                        if (srvIndex == 0) {
                            const char* loadPath = otrPath;
                            if (!loadPath && textureAddr != 0 && LooksLikeStringPointer(textureAddr)) {
                                loadPath = (const char*)textureAddr;
                            }
                            if (loadPath) {
                                srvIndex = TryLoadOTRTexture(loadPath, texMgr, tlutPath);
                                // If the path failed, try with __OTR__ prefix if it doesn't have one
                                if (srvIndex == 0 && loadPath[0] != '_') {
                                    std::string prefixedPath = std::string("__OTR__") + loadPath;
                                    srvIndex = TryLoadOTRTexture(prefixedPath.c_str(), texMgr, tlutPath);
                                }
                                // If path has __OTR__ prefix but failed, try without it
                                if (srvIndex == 0 && loadPath[0] == '_' && loadPath[1] == '_' && strlen(loadPath) > 7) {
                                    srvIndex = TryLoadOTRTexture(loadPath + 7, texMgr, tlutPath);
                                }
                            }
                        }

                        if (srvIndex > 0) {
                            mat.textureIndex = srvIndex;
                            totalUpdated++;
                        }
                    }
                };
                reResolve(roomGeo.opaqueMesh);
                reResolve(roomGeo.alphaMesh);
            }
            // Count total unresolved materials for logging
            uint32_t totalUnresolved = 0;
            for (const auto& roomGeo : m_roomGeometry) {
                for (const auto& mat : roomGeo.opaqueMesh.materials) {
                    if (mat.textureIndex == 0) totalUnresolved++;
                }
                for (const auto& mat : roomGeo.alphaMesh.materials) {
                    if (mat.textureIndex == 0) totalUnresolved++;
                }
            }
            if (m_reResolveCounter <= 30 || totalUpdated > 0 || (m_reResolveCounter % 60) == 0) {
                RTX_DIAG("Re-resolve frame %u: %u updated, %u still unresolved (textureIndex==0)",
                         m_reResolveCounter, totalUpdated, totalUnresolved);
            }

            if (totalUpdated > 0) {
                RTX_DIAG("Re-resolved %u material textures on frame %u", totalUpdated, m_reResolveCounter);
                SPDLOG_INFO("[RTX] Re-resolved {} material textures on frame {}", totalUpdated, m_reResolveCounter);

                // Rebuild the hit group shader table with updated material buffers.
                // The material data needs to be re-uploaded to the GPU since textureIndex
                // values have changed.
                if (m_accelerationStructure && m_pipeline) {
                    // Re-upload all material buffers with the updated textureIndex values
                    for (auto& roomGeo : m_roomGeometry) {
                        m_accelerationStructure->UpdateMaterialBuffers(roomGeo);
                    }
                    // Update the hit group shader table with the new buffer addresses
                    const auto& geomBuffers = m_accelerationStructure->GetGeometryBuffers();
                    std::vector<DXRPipeline::GeometryBufferAddresses> addrs;
                    addrs.reserve(geomBuffers.size());
                    for (const auto& gb : geomBuffers) {
                        DXRPipeline::GeometryBufferAddresses a;
                        a.vertexBuffer     = gb.vertexBufferAddress;
                        a.indexBuffer      = gb.indexBufferAddress;
                        a.materialIDBuffer = gb.materialIDBufferAddress;
                        a.materialTable    = gb.materialTableAddress;
                        addrs.push_back(a);
                    }
                    m_pipeline->UpdateHitGroupShaderTable(addrs);
                }
            }
        }
    }

    // Begin frame
    m_device->BeginFrame();
    auto* cmdList = m_device->GetCommandList();
    if (!cmdList) {
        RTX_DIAG("RTXRenderer::DispatchAndPresent() skipped - null command list from device after BeginFrame (frame #%u)", s_renderFrameCount);
        OutputDebugStringA("[RTX] CRITICAL: null command list after BeginFrame in RTX render path\n");
        SPDLOG_ERROR("[RTX] DispatchAndPresent: null command list from DX12Device");
        // BeginFrame was called; must Present to keep swap chain in sync
        m_device->Present();
        return;
    }

    ID3D12Resource* backBuffer = m_device->GetCurrentBackBuffer();
    if (!backBuffer) {
        RTX_DIAG("RTXRenderer::DispatchAndPresent() skipped - null back buffer after BeginFrame (frame #%u)", s_renderFrameCount);
        OutputDebugStringA("[RTX] CRITICAL: null back buffer in RTX render path\n");
        SPDLOG_ERROR("[RTX] DispatchAndPresent: null back buffer");
        // Close command list empty and present to keep swap chain in sync
        cmdList->Close();
        ID3D12CommandList* ppEmpty[] = { cmdList };
        auto* queue = m_device->GetCommandQueue();
        if (queue) queue->ExecuteCommandLists(1, ppEmpty);
        m_device->Present();
        return;
    }

    // One-time verification: check that swap chain, back buffer, and output buffers have compatible dimensions/formats
    {
        static bool s_verifiedOnce = false;
        if (!s_verifiedOnce && m_pipeline) {
            s_verifiedOnce = true;
            D3D12_RESOURCE_DESC bbDesc = backBuffer->GetDesc();
            ID3D12Resource* ppBuf = m_pipeline->GetPostProcessBuffer();
            ID3D12Resource* outBuf = m_pipeline->GetOutputBuffer();

            FILE* verifyLog = fopen("rtx_nuclear_test.log", "a");
            if (verifyLog) {
                fprintf(verifyLog, "\n=== DX12 FORMAT/DIMENSION VERIFICATION ===\n");
                fprintf(verifyLog, "  BackBuffer: %llux%u format=%u(%s)\n",
                        (unsigned long long)bbDesc.Width, bbDesc.Height, (unsigned)bbDesc.Format,
                        bbDesc.Format == 28 ? "R8G8B8A8_UNORM" : "OTHER");
                if (ppBuf) {
                    D3D12_RESOURCE_DESC ppDesc = ppBuf->GetDesc();
                    fprintf(verifyLog, "  PostProcess: %llux%u format=%u(%s)\n",
                            (unsigned long long)ppDesc.Width, ppDesc.Height, (unsigned)ppDesc.Format,
                            ppDesc.Format == 28 ? "R8G8B8A8_UNORM" : "OTHER");
                    bool match = (ppDesc.Width == bbDesc.Width && ppDesc.Height == bbDesc.Height && ppDesc.Format == bbDesc.Format);
                    fprintf(verifyLog, "  PP <-> BB: dimensions=%s format=%s OVERALL=%s\n",
                            (ppDesc.Width == bbDesc.Width && ppDesc.Height == bbDesc.Height) ? "MATCH" : "MISMATCH",
                            ppDesc.Format == bbDesc.Format ? "MATCH" : "MISMATCH",
                            match ? "EXACT MATCH (CopyResource ok)" : "MISMATCH");
                } else {
                    fprintf(verifyLog, "  PostProcess: NULL (shader may not have loaded)\n");
                }
                if (outBuf) {
                    D3D12_RESOURCE_DESC outDesc = outBuf->GetDesc();
                    fprintf(verifyLog, "  OutputBuffer: %llux%u format=%u(%s)\n",
                            (unsigned long long)outDesc.Width, outDesc.Height, (unsigned)outDesc.Format,
                            outDesc.Format == 10 ? "R16G16B16A16_FLOAT" : "OTHER");
                }
                fprintf(verifyLog, "  SwapChain: %p  DeviceInitialized: %s\n",
                        (void*)m_device->GetSwapChain(),
                        m_device->IsInitialized() ? "YES" : "NO");
                fprintf(verifyLog, "=== END VERIFICATION ===\n\n");
                fflush(verifyLog);
                fclose(verifyLog);
            }
        }
    }

    // Transition back buffer to render target
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = backBuffer;
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
        ID3D12DescriptorHeap* deviceHeap = m_device->GetSRVHeap();
        if (texHeap && texHeap != deviceHeap) {
            // DX12 only allows one CBV_SRV_UAV heap at a time.
            // Bind TextureManager's heap so texture SRVs are accessible during
            // DispatchRays; output UAVs are referenced via root descriptors.
            ID3D12DescriptorHeap* heaps[] = { texHeap };
            cmdList->SetDescriptorHeaps(1, heaps);
        } else if (deviceHeap) {
            ID3D12DescriptorHeap* heaps[] = { deviceHeap };
            cmdList->SetDescriptorHeaps(1, heaps);
        } else if (texHeap) {
            // Fall back to TextureManager's heap if device heap is null
            ID3D12DescriptorHeap* heaps[] = { texHeap };
            cmdList->SetDescriptorHeaps(1, heaps);
        }
        // If both are null, skip SetDescriptorHeaps — DispatchRays will be skipped anyway
    }

    // Rebuild TLAS with all loaded rooms (must happen before DispatchRays)
    if (m_accelerationStructure && m_accelerationStructure->GetInstanceCount() > 0) {
        if (s_renderFrameCount <= 3 || (s_renderFrameCount % 300) == 0) {
            RTX_DIAG("[DIAG] RTXRenderer: Building TLAS with %u BLAS instances (frame #%u)",
                     m_accelerationStructure->GetInstanceCount(), s_renderFrameCount);
        }
        m_accelerationStructure->RebuildTLAS(cmdList);
    }

    // GPU-side clear of accumulation and output buffers on scene change.
    // This prevents stale data from a previous scene from contaminating the
    // new scene's first frames through temporal accumulation blending.
    // The GISystem sets the flag via ResetAccumulation() on scene load/unload.
    if (m_giSystem && m_giSystem->NeedsGPUBufferClear() && m_pipeline) {
        RTX_DIAG("RTXRenderer: GPU-clearing accumulation/output buffers on scene change (frame #%u)", s_renderFrameCount);
        const float clearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        // Clear output buffer (noisy raytrace result)
        ID3D12Resource* outputBuf = m_pipeline->GetOutputBuffer();
        if (outputBuf) {
            // Use ClearUnorderedAccessViewFloat to zero the UAV.
            // We need both CPU and GPU descriptor handles for this call.
            // Since we don't have easy access to the CPU descriptor handle,
            // use a resource barrier + discard approach instead:
            // transition to COPY_DEST, clear via mapping, transition back.
            // Actually, the simplest approach is to just let the accumulate shader
            // handle it via frameCount=0 which copies current frame as-is.
            // But for extra safety, issue a UAV barrier first.
            D3D12_RESOURCE_BARRIER clearBarrier = {};
            clearBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            clearBarrier.UAV.pResource = nullptr; // Global UAV barrier
            cmdList->ResourceBarrier(1, &clearBarrier);
        }
        // The Accumulate shader with frameCount=0 will treat the current frame
        // as the sole input (alpha=1.0), effectively discarding all history.
        // This is the correct GPU-side "clear" for our temporal accumulation system.
    }

    // Ensure scene constants are initialized before first DispatchRays.
    // If UpdateSceneParams hasn't been called yet (e.g., game hasn't provided
    // camera data), initialize with sensible defaults from the scene config
    // so the shader doesn't receive all-zero constants (which causes a black frame
    // that pollutes temporal accumulation history).
    if (!m_sceneConstantsValid && m_pipeline) {
        RTX_DIAG("RTXRenderer: Scene constants not yet initialized — applying defaults (frame #%u, scene=0x%02X)", s_renderFrameCount, m_currentScene);
        memset(&m_sceneConstants, 0, sizeof(m_sceneConstants));

        // Set identity view matrix (camera at origin looking down -Z)
        m_sceneConstants.viewMatrix[0] = 1.0f;
        m_sceneConstants.viewMatrix[5] = 1.0f;
        m_sceneConstants.viewMatrix[10] = 1.0f;
        m_sceneConstants.viewMatrix[15] = 1.0f;

        // Translate camera to a reasonable position per-scene
        float camY = 500.0f;
        switch (m_currentScene) {
            case 0x58: camY = 470.0f; m_sceneConstants.viewMatrix[3] = 260.0f; m_sceneConstants.viewMatrix[11] = 435.0f; break;
            case 0x55: camY = 200.0f; m_sceneConstants.viewMatrix[3] = 111.0f; m_sceneConstants.viewMatrix[11] = 920.0f; break;
            case 0x51: camY = 200.0f; break;
            case 0x54: camY = 300.0f; break;
            default: break;
        }
        m_sceneConstants.viewMatrix[7] = -camY;

        // Default projection (60 degree vertical FOV, 4:3 aspect)
        float fovY = 1.047f;
        float aspect = 4.0f / 3.0f;
        float nearZ = 10.0f;
        float farZ = 12800.0f;
        float tanHalfFovY = tanf(fovY * 0.5f);
        m_sceneConstants.projMatrix[0] = 1.0f / (aspect * tanHalfFovY);
        m_sceneConstants.projMatrix[5] = 1.0f / tanHalfFovY;
        m_sceneConstants.projMatrix[10] = farZ / (farZ - nearZ);
        m_sceneConstants.projMatrix[11] = 1.0f;
        m_sceneConstants.projMatrix[14] = -(nearZ * farZ) / (farZ - nearZ);

        // Compute inverses — use analytical inverse for view matrix (orthonormal rotation + translation)
        {
            const float* V = m_sceneConstants.viewMatrix;
            float* I = m_sceneConstants.invViewMatrix;
            I[0]  = V[0]; I[1]  = V[4]; I[2]  = V[8];
            I[4]  = V[1]; I[5]  = V[5]; I[6]  = V[9];
            I[8]  = V[2]; I[9]  = V[6]; I[10] = V[10];
            float tx = V[3], ty = V[7], tz = V[11];
            I[3]  = -(I[0]*tx + I[1]*ty + I[2]*tz);
            I[7]  = -(I[4]*tx + I[5]*ty + I[6]*tz);
            I[11] = -(I[8]*tx + I[9]*ty + I[10]*tz);
            I[12] = 0.0f; I[13] = 0.0f; I[14] = 0.0f; I[15] = 1.0f;
        }
        InvertMatrix4x4(m_sceneConstants.projMatrix, m_sceneConstants.invProjMatrix);

        // Sun direction — normalized (0.5, 0.8, 0.3)
        float sd[3] = { 0.5f, 0.8f, 0.3f };
        float sdLen = sqrtf(sd[0]*sd[0] + sd[1]*sd[1] + sd[2]*sd[2]);
        m_sceneConstants.sunDirection[0] = sd[0]/sdLen;
        m_sceneConstants.sunDirection[1] = sd[1]/sdLen;
        m_sceneConstants.sunDirection[2] = sd[2]/sdLen;
        m_sceneConstants.sunDirection[3] = 0.0f;

        // Pure white sun color — NO warm bias to avoid amplifying N64's naturally warm/green textures
        m_sceneConstants.sunColor[0] = 1.0f;
        m_sceneConstants.sunColor[1] = 1.0f;
        m_sceneConstants.sunColor[2] = 1.0f;
        m_sceneConstants.sunColor[3] = 1.0f;

        // Cool blue ambient — matches default scene config values
        m_sceneConstants.ambientColor[0] = 0.25f;
        m_sceneConstants.ambientColor[1] = 0.30f;
        m_sceneConstants.ambientColor[2] = 0.50f;
        m_sceneConstants.ambientColor[3] = 1.0f;

        // Fog
        m_sceneConstants.fogColor[0] = 0.7f;
        m_sceneConstants.fogColor[1] = 0.8f;
        m_sceneConstants.fogColor[2] = 0.9f;
        m_sceneConstants.fogColor[3] = 1.0f;
        m_sceneConstants.fogStart = m_currentSceneConfig.fogNearDefault > 0 ? m_currentSceneConfig.fogNearDefault : 3000.0f;
        m_sceneConstants.fogEnd = m_currentSceneConfig.fogFarDefault > 0 ? m_currentSceneConfig.fogFarDefault : 12000.0f;

        // Intensities — use moderate values matching the scene config defaults
        // to avoid over-exposure that amplifies color cast in N64 textures
        m_sceneConstants.sunIntensity = 1.8f;
        m_sceneConstants.ambientIntensity = 0.35f;
        m_sceneConstants.giIntensity = 0.3f;
        m_sceneConstants.reflectionIntensity = 0.2f;
        m_sceneConstants.skyIntensity = 1.0f;
        m_sceneConstants.exposure = 1.0f;  // Neutral exposure — scene configs override this

        // Integer params
        m_sceneConstants.frameCount = 0;
        m_sceneConstants.toneMapMode = 0; // ACES
        m_sceneConstants.skyBlendFactor = 0.5f;
        m_sceneConstants.debugMode = m_debugMode;  // Use member debug mode (default 0=normal rendering)

        m_pipeline->UpdateSceneConstants(m_sceneConstants);
        m_sceneConstantsValid = true;

        // Dump the default constants to file for diagnostic inspection
        {
            FILE* initDump = fopen("rtx_cb_dump.txt", "a");
            if (initDump) {
                fprintf(initDump, "\n====== DEFAULT SceneConstants Initialization (scene=0x%02X) ======\n", m_currentScene);
                fprintf(initDump, "  debugMode=%d (0=normal, 1=albedo-only, 2=normals, 3=lighting)\n", m_sceneConstants.debugMode);
                fprintf(initDump, "  sunColor=(%.3f, %.3f, %.3f, %.3f)\n",
                        m_sceneConstants.sunColor[0], m_sceneConstants.sunColor[1],
                        m_sceneConstants.sunColor[2], m_sceneConstants.sunColor[3]);
                fprintf(initDump, "  ambientColor=(%.3f, %.3f, %.3f, %.3f)\n",
                        m_sceneConstants.ambientColor[0], m_sceneConstants.ambientColor[1],
                        m_sceneConstants.ambientColor[2], m_sceneConstants.ambientColor[3]);
                fprintf(initDump, "  sunIntensity=%.3f  ambientIntensity=%.3f  exposure=%.3f\n",
                        m_sceneConstants.sunIntensity, m_sceneConstants.ambientIntensity, m_sceneConstants.exposure);
                fprintf(initDump, "====== End Default CB ======\n");
                fclose(initDump);
            }
        }
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
        // Clear to dark blue while waiting for geometry to load
        float clearColor[4] = { 0.02f, 0.02f, 0.05f, 1.0f };
        cmdList->ClearRenderTargetView(m_device->GetCurrentRTVHandle(), clearColor, 0, nullptr);

        D3D12_RESOURCE_BARRIER presentBarrier = {};
        presentBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        presentBarrier.Transition.pResource = backBuffer;
        presentBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        presentBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        presentBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmdList->ResourceBarrier(1, &presentBarrier);

        cmdList->Close();
        ID3D12CommandList* ppCmdLists[] = { cmdList };
        auto* cmdQueue = m_device->GetCommandQueue();
        if (cmdQueue) {
            cmdQueue->ExecuteCommandLists(1, ppCmdLists);
        } else {
            RTX_DIAG("RTXRenderer: TLAS=0 path - null command queue, cannot execute (frame #%u)", s_renderFrameCount);
            OutputDebugStringA("[RTX] CRITICAL: null command queue in TLAS=0 path\n");
        }
        // Save back buffer reference BEFORE Present (Present->MoveToNextFrame changes the frame index)
        ID3D12Resource* presentedBB2 = m_device->GetCurrentBackBuffer();
        D3D12_RESOURCE_DESC presentedBBDesc2 = presentedBB2 ? presentedBB2->GetDesc() : D3D12_RESOURCE_DESC{};
        m_device->Present();
        // Capture screenshot after Present using the buffer that was just presented
        RTXScreenCapture::CheckKeyPress();
        if (presentedBB2 && m_device->GetCommandQueue()) {
            RTX_CaptureAfterPresent(presentedBB2, m_device->GetCommandQueue(),
                                    presentedBBDesc2.Format, static_cast<UINT>(presentedBBDesc2.Width), presentedBBDesc2.Height);
        }
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

    if (s_renderFrameCount <= 10 || (s_renderFrameCount % 300) == 0) {
        // Log material texture index stats for debugging white-texture issues
        uint32_t totalMats = 0, texturedMats = 0, whiteMats = 0;
        for (const auto& roomGeo : m_roomGeometry) {
            for (const auto& mat : roomGeo.opaqueMesh.materials) {
                totalMats++;
                if (mat.textureIndex > 0) texturedMats++;
                else whiteMats++;
            }
            for (const auto& mat : roomGeo.alphaMesh.materials) {
                totalMats++;
                if (mat.textureIndex > 0) texturedMats++;
                else whiteMats++;
            }
        }
        RTX_DIAG("RTXRenderer: DispatchRays %ux%u, TLAS=0x%llX, texTable=0x%llX, materials: %u total, %u textured, %u white(idx=0)",
                 m_device->GetWidth(), m_device->GetHeight(),
                 (unsigned long long)tlasAddr, (unsigned long long)texTableGPU.ptr,
                 totalMats, texturedMats, whiteMats);
        auto& texMgr = TextureManager::GetInstance();
        RTX_DIAG("  TextureManager: nextSRV=%u, heap=%p, rooms=%zu",
                 texMgr.GetNextSRVIndex(), (void*)texMgr.GetSRVHeap(), m_roomGeometry.size());
    }
    if (!m_pipeline) {
        RTX_DIAG("RTXRenderer: pipeline null at DispatchRays (frame #%u)", s_renderFrameCount);
        OutputDebugStringA("[RTX] CRITICAL: pipeline null at DispatchRays\n");
        // Transition back buffer to present and bail
        D3D12_RESOURCE_BARRIER bailBarrier = {};
        bailBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        bailBarrier.Transition.pResource = backBuffer;
        bailBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        bailBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        bailBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmdList->ResourceBarrier(1, &bailBarrier);
        cmdList->Close();
        ID3D12CommandList* ppBail[] = { cmdList };
        auto* q = m_device->GetCommandQueue();
        if (q) q->ExecuteCommandLists(1, ppBail);
        m_device->Present();
        return;
    }
    if (s_renderFrameCount <= 10 || (s_renderFrameCount % 300) == 0) {
        char dispMsg[256];
        snprintf(dispMsg, sizeof(dispMsg),
                 "[RTX] DispatchRays CALLING: %ux%u, TLAS=0x%llX, pipeline=%p, stateObj=%p (frame #%u)\n",
                 m_device->GetWidth(), m_device->GetHeight(),
                 (unsigned long long)tlasAddr, (void*)m_pipeline.get(),
                 (void*)m_pipeline->GetStateObject(),
                 s_renderFrameCount);
        OutputDebugStringA(dispMsg);
        RTX_DIAG("%s", dispMsg);
    }
    m_pipeline->DispatchRays(cmdList, m_device->GetWidth(), m_device->GetHeight(), tlasAddr, texTableGPU);
    if (s_renderFrameCount <= 10 || (s_renderFrameCount % 300) == 0) {
        OutputDebugStringA("[RTX] DispatchRays COMPLETED successfully\n");
        printf("[RTX] DispatchRays completed (frame #%u)\n", s_renderFrameCount);
    }

    // =========================================================================
    // Post-Processing Pipeline (validated integration):
    //
    // Data flow:
    //   DispatchRays → output(u0), accumulation(u1), normals(u2), depth(u3)
    //       ↓
    //   Accumulate: reads output(u0) + history(u1), writes blended to both
    //       ↓
    //   Denoise pass 0 (even): reads output(u0), writes temp(u1)
    //   Denoise pass 1 (odd):  reads temp(u0),   writes output(u1)
    //   Denoise pass 2 (even): reads output(u0), writes temp(u1)
    //   Denoise pass 3 (odd):  reads temp(u0),   writes output(u1)  ← final denoised in output
    //       ↓
    //   PostProcess: reads output(u0), writes postProcessBuffer(u1) → LDR
    //       ↓
    //   Copy postProcessBuffer → back buffer → Present
    //
    // Register bindings (verified to match HLSL):
    //   Accumulate.hlsl: u0=g_current(output), u1=g_history(accum), b0=AccumulateConstants
    //   Denoise.hlsl:    u0=g_input(ping), u1=g_output(pong), t0=normals, t1=depth, b0=DenoiseConstants
    //   PostProcess.hlsl: u0=input(denoised), u1=output(postprocess), b0=PostProcessConstants
    //
    // Thread groups: all compute shaders use [numthreads(8,8,1)],
    //   dispatched with ceil(width/8) x ceil(height/8) groups.
    // =========================================================================

    // Validate denoiser pipeline parameters on first frame and periodically
    if (m_giSystem && (s_renderFrameCount == 1 || (s_renderFrameCount % 600) == 0)) {
        bool pipelineValid = m_giSystem->ValidateDenoiserPipeline(m_device->GetWidth(), m_device->GetHeight());
        RTX_DIAG("Denoiser pipeline validation: %s (frame #%u, accumFrames=%u/%u, blendAlpha=%.3f, colorSigma=%.4f, cameraMoved=%s)",
                 pipelineValid ? "PASS" : "FAIL",
                 s_renderFrameCount,
                 m_giSystem->GetAccumulationFrameCount(),
                 m_giSystem->GetMaxAccumulationFrames(),
                 m_giSystem->GetBlendAlpha(),
                 m_giSystem->GetDenoiseConstants(0).colorSigma,
                 m_giSystem->CameraMovedThisFrame() ? "yes" : "no");
        if (!pipelineValid) {
            SPDLOG_WARN("[RTX] Denoiser pipeline validation FAILED on frame {} — denoise parameters may be out of range",
                         s_renderFrameCount);
        }
    }

    D3D12_RESOURCE_BARRIER uavBarrier = {};
    uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarrier.UAV.pResource = nullptr; // Global UAV barrier for all resources

    // --- Step 1: UAV barrier after ray tracing ---
    cmdList->ResourceBarrier(1, &uavBarrier);

    // --- Step 2: Temporal accumulation ---
    // Transition normals+depth from UAV to SRV for later denoise reads.
    // This is needed because the denoise shader reads normals/depth as Texture2D (SRV),
    // not RWTexture2D (UAV). The transition must happen after ray tracing writes.
    {
        ID3D12Resource* normalsRes = m_pipeline->GetNormalsBuffer();
        ID3D12Resource* depthRes = m_pipeline->GetDepthBuffer();
        if (normalsRes && depthRes) {
            D3D12_RESOURCE_BARRIER srvBarriers[2] = {};
            srvBarriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            srvBarriers[0].Transition.pResource = normalsRes;
            srvBarriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            srvBarriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            srvBarriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

            srvBarriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            srvBarriers[1].Transition.pResource = depthRes;
            srvBarriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            srvBarriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            srvBarriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

            cmdList->ResourceBarrier(2, srvBarriers);
        }
    }

    // Dispatch temporal accumulation (Accumulate.hlsl):
    // Reads noisy current frame from output buffer (u0), history from accumulation buffer (u1).
    // Blends with variance-clipping + tonemapped EMA. On camera movement (frameCount==0),
    // resets to current frame only (no history blending). Writes result to both buffers.
    if (m_giSystem) {
        AccumulateConstants ac = m_giSystem->GetAccumulateConstants(m_device->GetWidth(), m_device->GetHeight());
        // Log accumulation state frequently for the first 60 frames to diagnose
        // if frameCount is staying at 0 (which would prevent temporal noise reduction)
        if (s_renderFrameCount <= 60 || (s_renderFrameCount % 300) == 0) {
            RTX_DIAG("Accumulate dispatch: %ux%u, frameCount=%u/%u, blendAlpha=%.3f, cameraMoved=%s (frame #%u)",
                     ac.resolutionX, ac.resolutionY, ac.frameCount,
                     m_giSystem->GetMaxAccumulationFrames(), ac.blendAlpha,
                     m_giSystem->CameraMovedThisFrame() ? "YES" : "no", s_renderFrameCount);
        }
        m_pipeline->DispatchAccumulate(cmdList, m_device->GetWidth(), m_device->GetHeight(), ac);
    }

    // --- Step 3: UAV barrier after accumulation ---
    cmdList->ResourceBarrier(1, &uavBarrier);

    // --- Step 4: A-trous wavelet denoise (Denoise.hlsl) ---
    // Uses edge-stopping weights based on normals (t0) and depth (t1) to preserve
    // geometric edges while smoothing stochastic noise from path tracing.
    // Ping-pong between output buffer and temp buffer via descriptor table swaps:
    //   Even passes: u0=output→u1=temp
    //   Odd passes:  u0=temp→u1=output
    // After 4 passes (last is pass 3, odd), result is in output buffer.
    constexpr int numDenoisePasses = GISystem::NUM_DENOISE_PASSES;
    for (int pass = 0; pass < numDenoisePasses; pass++) {
        if (m_giSystem) {
            DenoiseConstants dc = m_giSystem->GetDenoiseConstants(pass);
            if (s_renderFrameCount <= 3 || (s_renderFrameCount % 300) == 0) {
                if (pass == 0) {
                    RTX_DIAG("Denoise dispatch: %ux%u, %d passes, pass0: step=%d colorSigma=%.4f normalSigma=%.4f depthSigma=%.4f",
                             m_device->GetWidth(), m_device->GetHeight(), numDenoisePasses,
                             dc.stepSize, dc.colorSigma, dc.normalSigma, dc.depthSigma);
                }
            }
            m_pipeline->DispatchDenoise(cmdList, m_device->GetWidth(), m_device->GetHeight(), pass, dc);
        } else {
            m_pipeline->DispatchDenoise(cmdList, m_device->GetWidth(), m_device->GetHeight(), pass);
        }

        // UAV barrier between denoise passes (required for ping-pong correctness)
        cmdList->ResourceBarrier(1, &uavBarrier);
    }

    // --- Step 5: Post-process (tone mapping + gamma correction) ---
    // Exposure and tone map mode are pulled from the per-scene config so they
    // can be tuned per-scene (e.g., brighter exposure for dark interiors,
    // Reinhard for soft look in Lost Woods, etc.)
    {
        PostProcessConstants ppc = {};
        ppc.resolutionX = m_device->GetWidth();
        ppc.resolutionY = m_device->GetHeight();
        // In debug modes (albedo, normals, lighting, depth), force LINEAR tone mapping
        // and neutral exposure to prevent ACES from introducing color shifts.
        // ACES filmic tone mapping can significantly shift hues (especially yellow-green),
        // so we bypass it entirely for diagnostic output.
        if (m_debugMode != 0) {
            ppc.exposure = 1.0f;       // Neutral exposure for raw debug output
            ppc.toneMapMode = 2;       // 2=Linear — no tone mapping curve applied
        } else {
            ppc.exposure = m_currentSceneConfig.exposure;      // From scene config (default 1.2)
            ppc.toneMapMode = m_currentSceneConfig.toneMapMode; // From scene config (default 0=ACES Filmic)
        }
        ppc.vignetteStrength = 0.0f;
        ppc.saturation = 1.0f;  // Neutral — no saturation adjustment
        ppc.contrast = 1.0f;    // Neutral — no contrast adjustment
        ppc.debugMode = m_debugMode;  // Direct int field — matches HLSL `int debugMode` at offset 28

        // DIAGNOSTIC: Log PostProcessConstants on first 3 frames and periodically
        {
            static uint32_t s_ppcLogCount = 0;
            s_ppcLogCount++;
            if (s_ppcLogCount <= 3 || (s_ppcLogCount % 600) == 0) {
                char ppcBuf[512];
                snprintf(ppcBuf, sizeof(ppcBuf),
                    "[RTX] PostProcessConstants: res=%ux%u exposure=%.3f toneMapMode=%u "
                    "vignette=%.3f sat=%.3f contrast=%.3f debugMode=%d sizeof=%zu\n",
                    ppc.resolutionX, ppc.resolutionY, ppc.exposure, ppc.toneMapMode,
                    ppc.vignetteStrength, ppc.saturation, ppc.contrast, ppc.debugMode,
                    sizeof(PostProcessConstants));
                OutputDebugStringA(ppcBuf);
                RTX_DIAG("%s", ppcBuf);

                // Dump raw bytes of PPC for byte-level verification
                const uint8_t* ppcRaw = (const uint8_t*)&ppc;
                snprintf(ppcBuf, sizeof(ppcBuf),
                    "[RTX] PPC raw bytes: %02X%02X%02X%02X %02X%02X%02X%02X "
                    "%02X%02X%02X%02X %02X%02X%02X%02X "
                    "%02X%02X%02X%02X %02X%02X%02X%02X "
                    "%02X%02X%02X%02X %02X%02X%02X%02X\n",
                    ppcRaw[0], ppcRaw[1], ppcRaw[2], ppcRaw[3],
                    ppcRaw[4], ppcRaw[5], ppcRaw[6], ppcRaw[7],
                    ppcRaw[8], ppcRaw[9], ppcRaw[10], ppcRaw[11],
                    ppcRaw[12], ppcRaw[13], ppcRaw[14], ppcRaw[15],
                    ppcRaw[16], ppcRaw[17], ppcRaw[18], ppcRaw[19],
                    ppcRaw[20], ppcRaw[21], ppcRaw[22], ppcRaw[23],
                    ppcRaw[24], ppcRaw[25], ppcRaw[26], ppcRaw[27],
                    ppcRaw[28], ppcRaw[29], ppcRaw[30], ppcRaw[31]);
                OutputDebugStringA(ppcBuf);

                // Also dump to file
                FILE* ppcDump = fopen("rtx_cb_dump.txt", "a");
                if (ppcDump) {
                    fprintf(ppcDump, "\n--- PostProcessConstants (frame %u) ---\n", s_ppcLogCount);
                    fprintf(ppcDump, "  resolution: %ux%u  exposure: %.3f  toneMapMode: %u\n",
                            ppc.resolutionX, ppc.resolutionY, ppc.exposure, ppc.toneMapMode);
                    fprintf(ppcDump, "  vignetteStrength: %.3f  saturation: %.3f  contrast: %.3f\n",
                            ppc.vignetteStrength, ppc.saturation, ppc.contrast);
                    fprintf(ppcDump, "  debugMode: %d (0=normal, 1=albedo, 2=normals, 3=lighting)\n", ppc.debugMode);
                    fprintf(ppcDump, "  sizeof: %zu bytes (expected 32)\n", sizeof(PostProcessConstants));
                    fclose(ppcDump);
                }
            }
        }

        m_pipeline->DispatchPostProcess(cmdList, m_device->GetWidth(), m_device->GetHeight(), ppc);
    }

    // --- Step 6: UAV barrier after post-process ---
    cmdList->ResourceBarrier(1, &uavBarrier);

    // Transition normals+depth back to UAV for next frame's ray tracing
    {
        ID3D12Resource* normalsRes = m_pipeline->GetNormalsBuffer();
        ID3D12Resource* depthRes = m_pipeline->GetDepthBuffer();
        if (normalsRes && depthRes) {
            D3D12_RESOURCE_BARRIER uavTransitions[2] = {};
            uavTransitions[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            uavTransitions[0].Transition.pResource = normalsRes;
            uavTransitions[0].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            uavTransitions[0].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            uavTransitions[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

            uavTransitions[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            uavTransitions[1].Transition.pResource = depthRes;
            uavTransitions[1].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            uavTransitions[1].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            uavTransitions[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

            cmdList->ResourceBarrier(2, uavTransitions);
        }
    }

    // --- Step 6b: UI compositing (overlay game HUD on top of post-processed scene) ---
    // The post-process buffer now contains the tone-mapped sRGB output.
    // If we have pending UI data, upload it and composite over the scene.
    if (m_uiCompositor && m_uiCompositor->IsInitialized() && m_hasPendingUI) {
        // Upload the UI frame data to the DX12 UI texture
        m_uiCompositor->UploadUIFrame(cmdList, m_pendingUIData.data(),
                                       m_pendingUIWidth, m_pendingUIHeight);
        m_hasPendingUI = false;

        // UAV barrier to ensure post-process writes are visible
        cmdList->ResourceBarrier(1, &uavBarrier);

        // Composite UI over the post-process output
        // The post-process buffer UAV is at the postProcessBufferUAV location
        m_uiCompositor->Composite(cmdList, m_device->GetWidth(), m_device->GetHeight(),
                                   m_pipeline->GetPostProcessBufferUAV());

        // UAV barrier after composite
        cmdList->ResourceBarrier(1, &uavBarrier);
    }

    // --- Step 7: Copy final output to back buffer ---
    // The PostProcess buffer is R8G8B8A8_UNORM (matches swap chain).
    // If PostProcess shader compiled, use that. Otherwise fall back to
    // raw/denoised HDR buffer (R16G16B16A16_FLOAT).
    ID3D12Resource* finalBuffer = nullptr;
    ID3D12Resource* finalBufferForTransition = nullptr; // Track for post-copy transition back to UAV
    bool postProcessValid = m_pipeline->GetPostProcessPipelineState() != nullptr;
    if (postProcessValid) {
        finalBuffer = m_pipeline->GetPostProcessBuffer();
    }
    if (!finalBuffer) {
        // Fall back to the denoised/raw output buffer (R16G16B16A16_FLOAT).
        finalBuffer = m_pipeline->GetFinalDenoisedBuffer(numDenoisePasses);
    }
    if (!finalBuffer) {
        finalBuffer = m_pipeline->GetOutputBuffer();
    }
    if (s_renderFrameCount <= 10 || (s_renderFrameCount % 300) == 0) {
        char fbMsg[512];
        snprintf(fbMsg, sizeof(fbMsg),
                 "[RTX] Step7: postProcessValid=%s finalBuffer=%p backBuffer=%p (frame #%u)\n",
                 postProcessValid ? "YES" : "NO(shader not loaded)",
                 (void*)finalBuffer, (void*)backBuffer, s_renderFrameCount);
        OutputDebugStringA(fbMsg);
        printf("%s", fbMsg);

        // Log to file for reliable diagnostics
        if (s_renderFrameCount <= 3) {
            FILE* diagF = fopen("rtx_nuclear_test.log", "a");
            if (diagF) {
                fprintf(diagF, "\n=== DispatchAndPresent Step7 (frame #%u) ===\n", s_renderFrameCount);
                fprintf(diagF, "  postProcessValid=%s\n", postProcessValid ? "YES" : "NO");
                fprintf(diagF, "  finalBuffer=%p backBuffer=%p\n", (void*)finalBuffer, (void*)backBuffer);
                if (finalBuffer) {
                    D3D12_RESOURCE_DESC fd = finalBuffer->GetDesc();
                    fprintf(diagF, "  finalBuffer: %llux%u format=%u(%s)\n",
                            (unsigned long long)fd.Width, fd.Height, (unsigned)fd.Format,
                            fd.Format == 28 ? "R8G8B8A8_UNORM" :
                            fd.Format == 10 ? "R16G16B16A16_FLOAT" : "OTHER");
                }
                if (backBuffer) {
                    D3D12_RESOURCE_DESC bd = backBuffer->GetDesc();
                    fprintf(diagF, "  backBuffer: %llux%u format=%u(%s)\n",
                            (unsigned long long)bd.Width, bd.Height, (unsigned)bd.Format,
                            bd.Format == 28 ? "R8G8B8A8_UNORM" :
                            bd.Format == 10 ? "R16G16B16A16_FLOAT" : "OTHER");
                }
                fprintf(diagF, "  swapChain=%p overlay=%s\n",
                        (void*)m_device->GetSwapChain(),
                        m_device->GetSwapChain() ? "check DX12Device logs" : "NULL SWAP CHAIN!");
                fflush(diagF);
                fclose(diagF);
            }
        }
    }

    if (finalBuffer) {
        D3D12_RESOURCE_DESC finalDesc = finalBuffer->GetDesc();
        D3D12_RESOURCE_DESC bbDesc = backBuffer->GetDesc();

        if (s_renderFrameCount <= 10 || (s_renderFrameCount % 300) == 0) {
            char dimMsg[512];
            snprintf(dimMsg, sizeof(dimMsg),
                     "[RTX] COPY: final=%p (%llux%u fmt=%u) -> bb=%p (%llux%u fmt=%u) (frame #%u)\n",
                     (void*)finalBuffer, (unsigned long long)finalDesc.Width, finalDesc.Height, (unsigned)finalDesc.Format,
                     (void*)backBuffer, (unsigned long long)bbDesc.Width, bbDesc.Height, (unsigned)bbDesc.Format,
                     s_renderFrameCount);
            OutputDebugStringA(dimMsg);
            printf("%s", dimMsg);
        }

        bool dimensionsMatch = (finalDesc.Width == bbDesc.Width && finalDesc.Height == bbDesc.Height);
        bool formatMatch = (finalDesc.Format == bbDesc.Format);

        if (dimensionsMatch && formatMatch) {
            // Exact match — use CopyResource (fast path).
            // Transition: finalBuffer UAV → COPY_SOURCE, backBuffer RENDER_TARGET → COPY_DEST
            D3D12_RESOURCE_BARRIER copyBarriers[2] = {};
            copyBarriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            copyBarriers[0].Transition.pResource = finalBuffer;
            copyBarriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            copyBarriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            copyBarriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

            copyBarriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            copyBarriers[1].Transition.pResource = backBuffer;
            copyBarriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            copyBarriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            copyBarriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

            cmdList->ResourceBarrier(2, copyBarriers);
            cmdList->CopyResource(backBuffer, finalBuffer);
            finalBufferForTransition = finalBuffer;

            if (s_renderFrameCount <= 5) {
                OutputDebugStringA("[RTX] CopyResource: exact match path (R8G8B8A8_UNORM → R8G8B8A8_UNORM)\n");
                // Log to file for reliable verification
                FILE* diagCopy = fopen("rtx_nuclear_test.log", "a");
                if (diagCopy) {
                    fprintf(diagCopy, "Frame #%u: CopyResource EXACT MATCH: final(%llux%u fmt=%u) -> bb(%llux%u fmt=%u)\n",
                            s_renderFrameCount,
                            (unsigned long long)finalDesc.Width, finalDesc.Height, (unsigned)finalDesc.Format,
                            (unsigned long long)bbDesc.Width, bbDesc.Height, (unsigned)bbDesc.Format);
                    fflush(diagCopy);
                    fclose(diagCopy);
                }
            }
        } else if (!formatMatch) {
            // Format mismatch (e.g. R16G16B16A16_FLOAT → R8G8B8A8_UNORM).
            // CopyResource/CopyTextureRegion cannot handle cross-format copies with
            // different bits-per-pixel. Clear the back buffer to cyan diagnostic color
            // to show this path was taken.
            float cyan[4] = { 0.0f, 0.8f, 0.8f, 1.0f };
            cmdList->ClearRenderTargetView(m_device->GetCurrentRTVHandle(), cyan, 0, nullptr);
            if (s_renderFrameCount <= 10 || (s_renderFrameCount % 300) == 0) {
                char fmtMsg[256];
                snprintf(fmtMsg, sizeof(fmtMsg),
                         "[RTX] FORMAT_MISMATCH: final fmt=%u bb fmt=%u — cleared to cyan (frame #%u)\n",
                         (unsigned)finalDesc.Format, (unsigned)bbDesc.Format, s_renderFrameCount);
                OutputDebugStringA(fmtMsg);
                printf("%s", fmtMsg);
            }
            // Back buffer stays in RENDER_TARGET state → will transition to PRESENT below
            finalBufferForTransition = nullptr;
        } else {
            // Dimension mismatch but format matches — use CopyTextureRegion
            UINT copyWidth = (UINT)std::min(finalDesc.Width, bbDesc.Width);
            UINT copyHeight = std::min(finalDesc.Height, bbDesc.Height);

            D3D12_RESOURCE_BARRIER copyBarriers[2] = {};
            copyBarriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            copyBarriers[0].Transition.pResource = finalBuffer;
            copyBarriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            copyBarriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            copyBarriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

            copyBarriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            copyBarriers[1].Transition.pResource = backBuffer;
            copyBarriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            copyBarriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            copyBarriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

            cmdList->ResourceBarrier(2, copyBarriers);

            D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
            srcLoc.pResource = finalBuffer;
            srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            srcLoc.SubresourceIndex = 0;

            D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
            dstLoc.pResource = backBuffer;
            dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dstLoc.SubresourceIndex = 0;

            D3D12_BOX srcBox = {};
            srcBox.left = 0;
            srcBox.top = 0;
            srcBox.front = 0;
            srcBox.right = copyWidth;
            srcBox.bottom = copyHeight;
            srcBox.back = 1;

            cmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, &srcBox);
            finalBufferForTransition = finalBuffer;
        }

        // Transition resources to their post-copy states
        if (finalBufferForTransition) {
            // backBuffer: COPY_DEST → PRESENT
            // finalBuffer: COPY_SOURCE → UNORDERED_ACCESS
            D3D12_RESOURCE_BARRIER presentBarriers[2] = {};
            presentBarriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            presentBarriers[0].Transition.pResource = backBuffer;
            presentBarriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            presentBarriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
            presentBarriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

            presentBarriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            presentBarriers[1].Transition.pResource = finalBufferForTransition;
            presentBarriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
            presentBarriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            presentBarriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

            cmdList->ResourceBarrier(2, presentBarriers);
        } else {
            // Format mismatch path: back buffer is still in RENDER_TARGET
            D3D12_RESOURCE_BARRIER presentBarrier = {};
            presentBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            presentBarrier.Transition.pResource = backBuffer;
            presentBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            presentBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
            presentBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            cmdList->ResourceBarrier(1, &presentBarrier);
        }
    } else {
        // No output buffer available; clear back buffer to bright green (diagnostic)
        float brightGreen[4] = { 0.0f, 1.0f, 0.0f, 1.0f };
        cmdList->ClearRenderTargetView(m_device->GetCurrentRTVHandle(), brightGreen, 0, nullptr);

        D3D12_RESOURCE_BARRIER presentBarrier = {};
        presentBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        presentBarrier.Transition.pResource = backBuffer;
        presentBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        presentBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        presentBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmdList->ResourceBarrier(1, &presentBarrier);

        if (s_renderFrameCount <= 10 || (s_renderFrameCount % 300) == 0) {
            OutputDebugStringA("[RTX] No final buffer — cleared to green\n");
        }
    }

    // --- Step 8: Close and execute command list ---
    HRESULT closeHr = cmdList->Close();
    if (FAILED(closeHr)) {
        RTX_DIAG("RTXRenderer: cmdList->Close() FAILED hr=0x%08X (frame #%u)", (uint32_t)closeHr, s_renderFrameCount);
        SPDLOG_ERROR("[RTX] DispatchAndPresent: cmdList->Close() failed: 0x{:08X}", (uint32_t)closeHr);
        // Try to present anyway to keep swap chain in sync
        m_device->Present();
        return;
    }

    ID3D12CommandList* ppCommandLists[] = { cmdList };
    auto* cmdQueue = m_device->GetCommandQueue();
    if (cmdQueue) {
        cmdQueue->ExecuteCommandLists(1, ppCommandLists);
    } else {
        RTX_DIAG("RTXRenderer: null command queue at ExecuteCommandLists (frame #%u)", s_renderFrameCount);
    }

    // --- Step 9: Present (calls swapChain->Present + MoveToNextFrame for fence sync) ---
    // Save back buffer reference BEFORE Present (MoveToNextFrame changes frame index)
    ID3D12Resource* presentedBackBufferMain = m_device->GetCurrentBackBuffer();
    D3D12_RESOURCE_DESC presentedBBDescMain = presentedBackBufferMain ? presentedBackBufferMain->GetDesc() : D3D12_RESOURCE_DESC{};
    m_device->Present();

    // --- Step 10: Screenshot capture if requested ---
    RTXScreenCapture::CheckKeyPress();
    if (presentedBackBufferMain && m_device->GetCommandQueue()) {
        RTX_CaptureAfterPresent(presentedBackBufferMain, m_device->GetCommandQueue(),
                                presentedBBDescMain.Format, static_cast<UINT>(presentedBBDescMain.Width), presentedBBDescMain.Height);
    }
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
// If tlutPath is non-null, it's used as the palette for CI4/CI8 textures.
static uint32_t TryLoadOTRTexture(const char* otrPath, TextureManager& texMgr, const char* tlutPath) {
    static uint32_t s_otrLoadAttempts = 0;
    static uint32_t s_otrLoadSuccesses = 0;
    static uint32_t s_otrLoadFailures = 0;
    s_otrLoadAttempts++;

    if (s_otrLoadAttempts <= 30) {
        RTX_DIAG("TryLoadOTRTexture: attempt #%u for path='%.120s'",
                 s_otrLoadAttempts, otrPath);
    }

    // Load texture resource with format information
    uint32_t texType = 0;
    uint16_t texWidth = 0;
    uint16_t texHeight = 0;
    uint32_t dataSize = 0;
    char* rawData = nullptr;

    try {
        rawData = ResourceMgr_LoadTexDataForRTX(otrPath, &texType, &texWidth, &texHeight, &dataSize);

        if (s_otrLoadAttempts <= 30) {
            RTX_DIAG("TryLoadOTRTexture: loaded '%s' rawData=%p %ux%u type=%u dataSize=%u",
                     otrPath, (void*)rawData, texWidth, texHeight, texType, dataSize);
        }
    } catch (const std::exception& e) {
        s_otrLoadFailures++;
        if (s_otrLoadFailures <= 30) {
            RTX_DIAG("TryLoadOTRTexture: std::exception loading '%.80s': %s (fail #%u/%u)",
                     otrPath, e.what(), s_otrLoadFailures, s_otrLoadAttempts);
        }
        SPDLOG_WARN("[RTX] Exception loading OTR texture: {} - {}", otrPath, e.what());
        return 0;
    } catch (...) {
        s_otrLoadFailures++;
        if (s_otrLoadFailures <= 30) {
            RTX_DIAG("TryLoadOTRTexture: unknown exception loading '%.80s' (fail #%u/%u)",
                     otrPath, s_otrLoadFailures, s_otrLoadAttempts);
        }
        SPDLOG_WARN("[RTX] Unknown exception loading OTR texture: {}", otrPath);
        return 0;
    }

    if (!rawData || texWidth == 0 || texHeight == 0) {
        s_otrLoadFailures++;
        if (s_otrLoadFailures <= 30) {
            RTX_DIAG("TryLoadOTRTexture: null data or zero dims for '%s' (rawData=%p, %ux%u, fail #%u)",
                     otrPath, (void*)rawData, texWidth, texHeight, s_otrLoadFailures);
        }
        return 0;
    }

    // Convert N64 texture format to RGBA8 using TextureManager's decoder.
    // The OTR resource stores textures in their original N64 format, NOT pre-decoded RGBA.
    // We must decode them here before uploading to the GPU.
    std::vector<uint8_t> rgba8;
    size_t expectedRGBA8Size = (size_t)texWidth * texHeight * 4;

    switch (texType) {
        case OTR_TEX_RGBA32: {
            // Already RGBA 8-8-8-8 — direct copy
            if (dataSize >= expectedRGBA8Size) {
                rgba8.assign((const uint8_t*)rawData, (const uint8_t*)rawData + expectedRGBA8Size);
            } else if (dataSize > 0) {
                // Data smaller than expected — pad with white
                rgba8.resize(expectedRGBA8Size, 255);
                memcpy(rgba8.data(), rawData, dataSize);
            }
            break;
        }
        case OTR_TEX_RGBA16: {
            // RGBA 5-5-5-1 → RGBA 8-8-8-8
            rgba8 = TextureManager::ConvertN64Texture((const uint8_t*)rawData,
                0 /*G_IM_FMT_RGBA*/, 2 /*G_IM_SIZ_16b*/, texWidth, texHeight);
            break;
        }
        case OTR_TEX_CI4:
        case OTR_TEX_CI8: {
            // CI 4-bit or 8-bit — needs palette (TLUT).
            // Try to load the TLUT from OTR using the provided tlutPath.
            const uint8_t* paletteData = nullptr;
            uint32_t paletteDataSize = 0;
            char* palRaw = nullptr;

            // Try explicit TLUT path first
            if (tlutPath && tlutPath[0] != '\0') {
                uint32_t palType = 0;
                uint16_t palW = 0, palH = 0;
                try {
                    palRaw = ResourceMgr_LoadTexDataForRTX(tlutPath, &palType, &palW, &palH, &paletteDataSize);
                } catch (...) {
                    palRaw = nullptr;
                }
                if (palRaw && paletteDataSize > 0) {
                    paletteData = (const uint8_t*)palRaw;
                    static uint32_t s_tlutLoadLog = 0;
                    s_tlutLoadLog++;
                    if (s_tlutLoadLog <= 20) {
                        RTX_DIAG("TryLoadOTRTexture: loaded TLUT '%s' type=%u %ux%u dataSize=%u",
                                 tlutPath, palType, palW, palH, paletteDataSize);
                    }
                }
            }

            // If no explicit TLUT, try auto-detecting from scene path
            if (!paletteData) {
                // Try to find a TLUT resource by searching for "TLUT" in the scene directory
                // e.g., for "scenes/shared/spot04_scene/spot04_room_0Tex_011D08"
                // look for  "scenes/shared/spot04_scene/spot04_sceneTLUT_*"
                std::string texPathStr(otrPath);
                // Strip __OTR__ prefix if present
                if (texPathStr.substr(0, 7) == "__OTR__") {
                    texPathStr = texPathStr.substr(7);
                }
                // Extract scene directory: everything up to last '/'
                size_t lastSlash = texPathStr.rfind('/');
                if (lastSlash != std::string::npos) {
                    std::string sceneDir = texPathStr.substr(0, lastSlash + 1);
                    // Extract scene name: the directory name (e.g., "spot04_scene")
                    size_t prevSlash = texPathStr.rfind('/', lastSlash - 1);
                    std::string sceneName;
                    if (prevSlash != std::string::npos) {
                        sceneName = texPathStr.substr(prevSlash + 1, lastSlash - prevSlash - 1);
                    }
                    // Common TLUT naming: sceneName + "TLUT_" + hex offset
                    // Try the most common pattern first
                    if (!sceneName.empty()) {
                        // For spot04_scene, the TLUT is spot04_sceneTLUT_00E010
                        // Try loading with common OoT TLUT offsets
                        static const char* COMMON_TLUT_OFFSETS[] = {
                            "00E010", "00E000", "00C010", "00C000", "00A010", "00A000",
                            "010010", "008010", "006010", "004010", "002010",
                            nullptr
                        };
                        for (int i = 0; COMMON_TLUT_OFFSETS[i]; i++) {
                            std::string tlutCandidate = sceneDir + sceneName + "TLUT_" + COMMON_TLUT_OFFSETS[i];
                            uint32_t palType2 = 0;
                            uint16_t palW2 = 0, palH2 = 0;
                            uint32_t palSize2 = 0;
                            char* palRaw2 = nullptr;
                            try {
                                palRaw2 = ResourceMgr_LoadTexDataForRTX(tlutCandidate.c_str(), &palType2, &palW2, &palH2, &palSize2);
                            } catch (...) {
                                palRaw2 = nullptr;
                            }
                            if (palRaw2 && palSize2 > 0) {
                                paletteData = (const uint8_t*)palRaw2;
                                paletteDataSize = palSize2;
                                static uint32_t s_autoTlut = 0;
                                s_autoTlut++;
                                if (s_autoTlut <= 10) {
                                    RTX_DIAG("TryLoadOTRTexture: auto-detected TLUT '%s' type=%u %ux%u dataSize=%u",
                                             tlutCandidate.c_str(), palType2, palW2, palH2, palSize2);
                                }
                                break;
                            }
                        }
                    }
                }
            }

            uint32_t sizBits = (texType == OTR_TEX_CI4) ? 0 /*G_IM_SIZ_4b*/ : 1 /*G_IM_SIZ_8b*/;
            rgba8 = TextureManager::ConvertN64Texture((const uint8_t*)rawData,
                2 /*G_IM_FMT_CI*/, sizBits, texWidth, texHeight,
                paletteData, 2 /*G_IM_SIZ_16b - RGBA16 TLUT*/);

            if (!paletteData) {
                static uint32_t s_noPalette = 0;
                s_noPalette++;
                if (s_noPalette <= 20) {
                    RTX_DIAG("TryLoadOTRTexture: CI texture '%s' type=%u decoded WITHOUT palette (magenta fallback)",
                             otrPath, texType);
                }
            }
            break;
        }
        case OTR_TEX_I4: {
            rgba8 = TextureManager::ConvertN64Texture((const uint8_t*)rawData,
                4 /*G_IM_FMT_I*/, 0 /*G_IM_SIZ_4b*/, texWidth, texHeight);
            break;
        }
        case OTR_TEX_I8: {
            rgba8 = TextureManager::ConvertN64Texture((const uint8_t*)rawData,
                4 /*G_IM_FMT_I*/, 1 /*G_IM_SIZ_8b*/, texWidth, texHeight);
            break;
        }
        case OTR_TEX_IA4: {
            rgba8 = TextureManager::ConvertN64Texture((const uint8_t*)rawData,
                3 /*G_IM_FMT_IA*/, 0 /*G_IM_SIZ_4b*/, texWidth, texHeight);
            break;
        }
        case OTR_TEX_IA8: {
            rgba8 = TextureManager::ConvertN64Texture((const uint8_t*)rawData,
                3 /*G_IM_FMT_IA*/, 1 /*G_IM_SIZ_8b*/, texWidth, texHeight);
            break;
        }
        case OTR_TEX_IA16: {
            rgba8 = TextureManager::ConvertN64Texture((const uint8_t*)rawData,
                3 /*G_IM_FMT_IA*/, 2 /*G_IM_SIZ_16b*/, texWidth, texHeight);
            break;
        }
        default: {
            // Unknown type — try heuristic decode based on data size.
            if (dataSize >= expectedRGBA8Size) {
                rgba8.assign((const uint8_t*)rawData, (const uint8_t*)rawData + expectedRGBA8Size);
                if (s_otrLoadAttempts <= 20) {
                    RTX_DIAG("TryLoadOTRTexture: unknown type %u, assuming RGBA32 (data=%zu, expected=%zu)",
                             texType, (size_t)dataSize, expectedRGBA8Size);
                }
            } else if (dataSize >= (size_t)texWidth * texHeight * 2) {
                // Try RGBA16 decode (2 bytes per pixel)
                rgba8 = TextureManager::ConvertN64Texture((const uint8_t*)rawData,
                    0 /*G_IM_FMT_RGBA*/, 2 /*G_IM_SIZ_16b*/, texWidth, texHeight);
                if (s_otrLoadAttempts <= 20) {
                    RTX_DIAG("TryLoadOTRTexture: unknown type %u, trying RGBA16 (data=%u, %ux%u)",
                             texType, dataSize, texWidth, texHeight);
                }
            } else if (dataSize >= (size_t)texWidth * texHeight) {
                // Try IA8 decode (1 byte per pixel)
                rgba8 = TextureManager::ConvertN64Texture((const uint8_t*)rawData,
                    3 /*G_IM_FMT_IA*/, 1 /*G_IM_SIZ_8b*/, texWidth, texHeight);
                if (s_otrLoadAttempts <= 20) {
                    RTX_DIAG("TryLoadOTRTexture: unknown type %u, trying IA8 (data=%u, %ux%u)",
                             texType, dataSize, texWidth, texHeight);
                }
            } else if (dataSize >= (size_t)texWidth * texHeight / 2) {
                // Try I4 decode (0.5 bytes per pixel)
                rgba8 = TextureManager::ConvertN64Texture((const uint8_t*)rawData,
                    4 /*G_IM_FMT_I*/, 0 /*G_IM_SIZ_4b*/, texWidth, texHeight);
                if (s_otrLoadAttempts <= 20) {
                    RTX_DIAG("TryLoadOTRTexture: unknown type %u, trying I4 (data=%u, %ux%u)",
                             texType, dataSize, texWidth, texHeight);
                }
            } else {
                s_otrLoadFailures++;
                if (s_otrLoadFailures <= 30) {
                    RTX_DIAG("TryLoadOTRTexture: unknown type %u, size too small %u for %ux%u (fail #%u)",
                             texType, dataSize, texWidth, texHeight, s_otrLoadFailures);
                }
                return 0;
            }
            break;
        }
    }

    if (rgba8.empty()) {
        s_otrLoadFailures++;
        if (s_otrLoadFailures <= 30) {
            RTX_DIAG("TryLoadOTRTexture: decode returned empty for '%s' type=%u %ux%u (fail #%u)",
                     otrPath, texType, texWidth, texHeight, s_otrLoadFailures);
        }
        return 0;
    }

    // DIAGNOSTIC: Log first pixel and a mid pixel to verify decoded color values.
    // If these show yellow-green values, the texture decode is the problem.
    // If they show correct colors, the problem is elsewhere (shader, display chain).
    {
        static uint32_t s_pixelDiagCount = 0;
        s_pixelDiagCount++;
        if (s_pixelDiagCount <= 30 || (s_pixelDiagCount % 100) == 0) {
            uint8_t r0 = rgba8[0], g0 = rgba8[1], b0 = rgba8[2], a0 = rgba8[3];
            size_t midIdx = (rgba8.size() / 2) & ~3u; // Align to 4 bytes
            uint8_t rm = rgba8[midIdx], gm = rgba8[midIdx+1], bm = rgba8[midIdx+2], am = rgba8[midIdx+3];
            printf("[RTX] TryLoadOTR #%u: '%.*s' type=%u %ux%u pixel[0]=(%u,%u,%u,%u) pixel[mid]=(%u,%u,%u,%u)\n",
                   s_pixelDiagCount, 80, otrPath, texType, texWidth, texHeight,
                   r0, g0, b0, a0, rm, gm, bm, am);
            // Also log to file for persistent record
            FILE* texLog = fopen("rtx_texture_diag.txt", "a");
            if (texLog) {
                fprintf(texLog, "Tex #%u: '%.80s' type=%u %ux%u dataSize=%u decoded=%zu "
                        "pixel[0]=(%u,%u,%u,%u) pixel[mid]=(%u,%u,%u,%u)\n",
                        s_pixelDiagCount, otrPath, texType, texWidth, texHeight, dataSize,
                        rgba8.size(), r0, g0, b0, a0, rm, gm, bm, am);
                fclose(texLog);
            }
        }
    }

    // Compute a hash for this texture based on the OTR path string
    constexpr uint64_t FNV_OFFSET = 0xcbf29ce484222325ULL;
    constexpr uint64_t FNV_PRIME  = 0x100000001b3ULL;
    uint64_t hash = FNV_OFFSET;
    for (const char* p = otrPath; *p; p++) {
        hash ^= (uint64_t)(uint8_t)*p;
        hash *= FNV_PRIME;
    }

    // Upload decoded RGBA8 data to TextureManager
    RTXTextureHandle handle = texMgr.GetOrUploadTexture(rgba8.data(), texWidth, texHeight, hash);

    if (handle.srvIndex > 0) {
        s_otrLoadSuccesses++;
        // Register alias hashes so future lookups by address or path-hash work
        uintptr_t addr = (uintptr_t)otrPath;
        uint64_t addrHash = FNV_OFFSET;
        for (size_t b = 0; b < sizeof(addr); b++) {
            addrHash ^= static_cast<uint64_t>((addr >> (b * 8)) & 0xFF);
            addrHash *= FNV_PRIME;
        }
        texMgr.RegisterHashAlias(addrHash, handle.srvIndex);
        texMgr.RegisterHashAlias(static_cast<uint64_t>(addr), handle.srvIndex);
        texMgr.RegisterHashAlias(hash, handle.srvIndex);

        if (s_otrLoadSuccesses <= 30 || (s_otrLoadSuccesses % 50) == 0) {
            RTX_DIAG("TryLoadOTRTexture: SUCCESS '%s' type=%u (%ux%u) -> SRV %u (success #%u/%u)",
                     otrPath, texType, texWidth, texHeight, handle.srvIndex,
                     s_otrLoadSuccesses, s_otrLoadAttempts);
        }
        SPDLOG_INFO("[RTX] Loaded OTR texture '{}' type={} ({}x{}) -> SRV index {}",
                    otrPath, texType, texWidth, texHeight, handle.srvIndex);
    } else {
        s_otrLoadFailures++;
        if (s_otrLoadFailures <= 30) {
            RTX_DIAG("TryLoadOTRTexture: upload failed for '%s' (%ux%u), srvIndex=0 (fail #%u)",
                     otrPath, texWidth, texHeight, s_otrLoadFailures);
        }
    }

    return handle.srvIndex;
}

void RTXRenderer::ResolveMaterialTextures(RoomGeometry& geometry) {
    auto& texMgr = TextureManager::GetInstance();

    RTX_DIAG("ResolveMaterialTextures: texMgr initialized=%s, heap=%p, nextSRV=%u",
             texMgr.GetSRVHeap() ? "yes" : "no",
             (void*)texMgr.GetSRVHeap(),
             texMgr.GetNextSRVIndex());

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
    auto resolveMesh = [&](ExtractedMesh& mesh, const char* meshName) {
        uint32_t resolved = 0;
        uint32_t loadedFromOTR = 0;
        uint32_t unresolved = 0;
        uint32_t noAddr = 0;

        // Log material path availability for debugging
        {
            uint32_t withPaths = 0, withTlut = 0, withInfo = 0;
            for (size_t j = 0; j < mesh.materials.size(); j++) {
                if (j < mesh.materialTexturePaths.size() && !mesh.materialTexturePaths[j].empty()) withPaths++;
                if (j < mesh.materialTlutPaths.size() && !mesh.materialTlutPaths[j].empty()) withTlut++;
                if (j < mesh.materialTexInfos.size()) withInfo++;
            }
            RTX_DIAG("ResolveMaterialTextures [%s]: %zu materials, %u with OTR paths, %u with TLUT paths, %u with texInfo",
                     meshName, mesh.materials.size(), withPaths, withTlut, withInfo);
        }

        for (size_t i = 0; i < mesh.materials.size(); i++) {
            Material& mat = mesh.materials[i];

            // Skip materials that were already resolved during eager resolution
            // (in SceneGeometryExtractor::GetOrCreateMaterial). Re-resolving would
            // be wasteful and could incorrectly overwrite a valid textureIndex.
            if (mat.textureIndex > 0) {
                resolved++;
                if (i < 10) {
                    RTX_DIAG("ResolveMaterialTextures [%s] mat %zu: already resolved -> SRV %u (skipping)",
                             meshName, i, mat.textureIndex);
                }
                continue;
            }

            // Get the durable OTR path string first (preferred — survives extractor reuse).
            // Fall back to the raw texture address pointer if no path was stored.
            const char* otrPath = nullptr;
            if (i < mesh.materialTexturePaths.size() && !mesh.materialTexturePaths[i].empty()) {
                otrPath = mesh.materialTexturePaths[i].c_str();
            }

            // Get the TLUT path (for CI4/CI8 textures)
            const char* tlutPath = nullptr;
            if (i < mesh.materialTlutPaths.size() && !mesh.materialTlutPaths[i].empty()) {
                tlutPath = mesh.materialTlutPaths[i].c_str();
            }

            // Get the full texture address from the parallel vector.
            uintptr_t textureAddr = 0;
            if (i < mesh.materialTextureAddrs.size()) {
                textureAddr = mesh.materialTextureAddrs[i];
            }

            if (textureAddr == 0 && !otrPath) {
                // No texture set for this material; use default white (SRV index 0).
                mat.textureIndex = 0;
                noAddr++;
                continue;
            }

            // Log each material's texture address for debugging
            if (i < 10) {
                if (otrPath) {
                    RTX_DIAG("ResolveMaterialTextures [%s] mat %zu: path='%.120s' addr=0x%llX",
                             meshName, i, otrPath, (unsigned long long)textureAddr);
                } else {
                    RTX_DIAG("ResolveMaterialTextures [%s] mat %zu: addr=0x%llX (no path)",
                             meshName, i, (unsigned long long)textureAddr);
                }
            }

            // Strategy 1: Try looking up by the OTR path string hash in the cache
            if (otrPath) {
                constexpr uint64_t FNV_OFFSET = 0xcbf29ce484222325ULL;
                constexpr uint64_t FNV_PRIME  = 0x100000001b3ULL;
                uint64_t pathHash = FNV_OFFSET;
                for (const char* p = otrPath; *p; p++) {
                    pathHash ^= (uint64_t)(uint8_t)*p;
                    pathHash *= FNV_PRIME;
                }
                uint32_t srvIndex = texMgr.GetSRVIndexForHash(pathHash);
                if (srvIndex > 0) {
                    mat.textureIndex = srvIndex;
                    resolved++;
                    if (i < 10) {
                        RTX_DIAG("  -> resolved via pathHash=0x%llX -> SRV %u",
                                 (unsigned long long)pathHash, srvIndex);
                    }
                    continue;
                }
            }

            // Strategy 2: Try the address-based FNV-1a hash
            if (textureAddr != 0) {
                constexpr uint64_t FNV_OFFSET = 0xcbf29ce484222325ULL;
                constexpr uint64_t FNV_PRIME  = 0x100000001b3ULL;
                uint64_t hash = FNV_OFFSET;
                for (size_t b = 0; b < sizeof(textureAddr); b++) {
                    hash ^= static_cast<uint64_t>((textureAddr >> (b * 8)) & 0xFF);
                    hash *= FNV_PRIME;
                }

                uint32_t srvIndex = texMgr.GetSRVIndexForHash(hash);
                if (srvIndex > 0) {
                    mat.textureIndex = srvIndex;
                    resolved++;
                    if (i < 10) {
                        RTX_DIAG("  -> resolved via addrHash=0x%llX -> SRV %u",
                                 (unsigned long long)hash, srvIndex);
                    }
                    continue;
                }

                // Try the raw address value as a fallback hash key
                srvIndex = texMgr.GetSRVIndexForHash(static_cast<uint64_t>(textureAddr));
                if (srvIndex > 0) {
                    mat.textureIndex = srvIndex;
                    resolved++;
                    if (i < 10) {
                        RTX_DIAG("  -> resolved via rawAddr=0x%llX -> SRV %u",
                                 (unsigned long long)textureAddr, srvIndex);
                    }
                    continue;
                }
            }

            // Strategy 3: Load directly from OTR archive.
            // Use the durable path string if available, otherwise try the raw pointer.
            const char* loadPath = otrPath;
            if (!loadPath && textureAddr != 0 && LooksLikeStringPointer(textureAddr)) {
                loadPath = (const char*)textureAddr;
            }

            if (loadPath) {
                uint32_t srvIndex = TryLoadOTRTexture(loadPath, texMgr, tlutPath);

                // If the path failed, try with __OTR__ prefix if it doesn't have one
                if (srvIndex == 0 && loadPath[0] != '_') {
                    std::string prefixedPath = std::string("__OTR__") + loadPath;
                    srvIndex = TryLoadOTRTexture(prefixedPath.c_str(), texMgr, tlutPath);
                }
                // If path has __OTR__ prefix but failed, try without it
                if (srvIndex == 0 && loadPath[0] == '_' && loadPath[1] == '_' && strlen(loadPath) > 7) {
                    srvIndex = TryLoadOTRTexture(loadPath + 7, texMgr, tlutPath);
                }

                if (srvIndex > 0) {
                    mat.textureIndex = srvIndex;
                    loadedFromOTR++;
                    if (i < 10) {
                        RTX_DIAG("  -> loaded from OTR '%s' -> SRV %u", loadPath, srvIndex);
                    }
                    continue;
                }
                // Log the first few failed OTR loads for debugging
                static uint32_t s_otrFailLogCount = 0;
                s_otrFailLogCount++;
                if (s_otrFailLogCount <= 30) {
                    RTX_DIAG("ResolveMaterialTextures: OTR load FAILED for mat %zu path='%.100s' (fail #%u)",
                             i, loadPath, s_otrFailLogCount);
                }
            } else if (i < 10) {
                RTX_DIAG("  -> no path available, can't load from OTR (addr=0x%llX)",
                         (unsigned long long)textureAddr);
            }

            // Texture not available. Fall back to default white (SRV index 0).
            mat.textureIndex = 0;
            unresolved++;
        }

        RTX_DIAG("ResolveMaterialTextures [%s]: %u materials: %u cached, %u OTR loaded, %u unresolved(white), %u no-addr",
                 meshName, (uint32_t)mesh.materials.size(), resolved, loadedFromOTR, unresolved, noAddr);
        SPDLOG_INFO("[RTX] ResolveMaterialTextures [{}]: {} total — {} cached, {} OTR loaded, {} unresolved (white), {} no-addr",
                    meshName, (uint32_t)mesh.materials.size(), resolved, loadedFromOTR, unresolved, noAddr);
    };

    resolveMesh(geometry.opaqueMesh, "opaque");
    resolveMesh(geometry.alphaMesh, "alpha");

    // Log total texture count after resolution
    RTX_DIAG("ResolveMaterialTextures: done. TextureManager has %u textures, nextSRV=%u",
             texMgr.GetTextureCount(), texMgr.GetNextSRVIndex());
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

void RTXRenderer::UploadUIOverlay(const uint8_t* pixelData, uint32_t width, uint32_t height) {
    if (!m_uiCompositor || !m_uiCompositor->IsInitialized()) {
        return;
    }

    if (pixelData && width > 0 && height > 0) {
        size_t dataSize = static_cast<size_t>(width) * height * 4;
        m_pendingUIData.resize(dataSize);
        memcpy(m_pendingUIData.data(), pixelData, dataSize);
        m_pendingUIWidth = width;
        m_pendingUIHeight = height;
        m_hasPendingUI = true;

        static uint32_t s_uploadCount = 0;
        s_uploadCount++;
        if (s_uploadCount <= 5 || (s_uploadCount % 300) == 0) {
            RTX_DIAG("RTXRenderer::UploadUIOverlay() %ux%u (%zu bytes), frame #%u",
                     width, height, dataSize, s_uploadCount);
        }
    } else {
        m_hasPendingUI = false;
    }
}

} // namespace RTX

#endif // ENABLE_DX12_RTX
