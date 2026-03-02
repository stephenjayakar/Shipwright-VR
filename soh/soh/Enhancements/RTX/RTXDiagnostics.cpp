#ifdef ENABLE_DX12_RTX

// RTXDiagnostics.cpp — Implementation of comprehensive RTX pipeline diagnostics.
//
// Reads state from existing RTX singletons (RTXRenderer, TextureManager, etc.)
// and outputs detailed status information through the RTX_DIAG() logging system.

#include "RTXDiagnostics.h"
#include "RTXDiagLog.h"

// Full RTX subsystem headers — needed to query state
#include "RTXRenderer.h"
#include "RTXManager.h"
#include "DX12Device.h"
#include "DXRPipeline.h"
#include "AccelerationStructure.h"
#include "TextureManager.h"
#include "GISystem.h"
#include "RTXSceneConfig.h"
#include "RTXTypes.h"

#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <chrono>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#endif

// Internal frame counter for RTX_LogFrameStats auto-increment
static uint32_t s_diagFrameCounter = 0;

// Static buffer for RTX_GetStatusLine
static char s_statusLineBuffer[512] = { 0 };

// Helper: write diagnostics to a FILE* (used by both DumpDiagnostics and DumpDiagnosticsToFile)
static void DumpDiagnosticsImpl(FILE* extraFile);

// Helper: log to both RTX_DIAG and an optional extra file
static void DiagLog(FILE* extraFile, const char* fmt, ...) {
    char buf[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    RTX_DIAG("%s", buf);

    if (extraFile) {
        fprintf(extraFile, "[RTX-DIAG] %s\n", buf);
        fflush(extraFile);
    }
}

// ============================================================================
// RTX_DumpDiagnostics
// ============================================================================

void RTX_DumpDiagnostics() {
    DumpDiagnosticsImpl(nullptr);
}

void RTX_DumpDiagnosticsToFile(const char* filePath) {
    FILE* f = fopen(filePath, "w");
    if (!f) {
        RTX_DIAG("RTX_DumpDiagnosticsToFile: Failed to open '%s' for writing", filePath);
        DumpDiagnosticsImpl(nullptr);
        return;
    }

    // Write header
    fprintf(f, "=== RTX Diagnostics Dump ===\n");
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    char timeBuf[64];
    struct tm tmBuf;
#ifdef _WIN32
    localtime_s(&tmBuf, &time_t_now);
#else
    localtime_r(&time_t_now, &tmBuf);
#endif
    strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", &tmBuf);
    fprintf(f, "Timestamp: %s\n\n", timeBuf);

    DumpDiagnosticsImpl(f);

    fprintf(f, "\n=== End RTX Diagnostics ===\n");
    fclose(f);
    RTX_DIAG("RTX_DumpDiagnosticsToFile: Written to '%s'", filePath);
}

static void DumpDiagnosticsImpl(FILE* extraFile) {
    DiagLog(extraFile, "========================================================");
    DiagLog(extraFile, "  RTX PIPELINE DIAGNOSTICS DUMP");
    DiagLog(extraFile, "========================================================");

    // ---- 1. RTXRenderer Singleton ----
    DiagLog(extraFile, "");
    DiagLog(extraFile, "--- RTXRenderer ---");

    RTX::RTXRenderer* renderer = RTX::RTXRenderer::Instance();
    if (!renderer) {
        DiagLog(extraFile, "  RTXRenderer::Instance() = NULL (not created)");
        DiagLog(extraFile, "  => RTX pipeline is NOT active. No further state to query.");
        DiagLog(extraFile, "========================================================");
        return;
    }

    DiagLog(extraFile, "  RTXRenderer::Instance() = %p", (void*)renderer);
    DiagLog(extraFile, "  RTXRenderer::IsActive()  = %s", RTX::RTXRenderer::IsActive() ? "YES" : "NO");
    DiagLog(extraFile, "  IsSceneLoaded            = %s", renderer->IsSceneLoaded() ? "YES" : "NO");
    DiagLog(extraFile, "  CurrentScene             = %d", renderer->GetCurrentScene());
    DiagLog(extraFile, "  IsRTXSceneActive         = %s", renderer->IsRTXSceneActive() ? "YES" : "NO");

    // ---- 2. DX12 Device ----
    DiagLog(extraFile, "");
    DiagLog(extraFile, "--- DX12Device ---");

    RTX::DX12Device* device = renderer->GetDevice();
    if (!device) {
        DiagLog(extraFile, "  DX12Device = NULL (not created)");
    } else {
        DiagLog(extraFile, "  DX12Device ptr            = %p", (void*)device);
        DiagLog(extraFile, "  IsInitialized             = %s", device->IsInitialized() ? "YES" : "NO");
        DiagLog(extraFile, "  SupportsRaytracing        = %s", device->SupportsRaytracing() ? "YES" : "NO");

        if (device->IsInitialized()) {
            DiagLog(extraFile, "  ID3D12Device5*            = %p", (void*)device->GetDevice());
            DiagLog(extraFile, "  CommandQueue              = %p", (void*)device->GetCommandQueue());
            DiagLog(extraFile, "  CommandList               = %p", (void*)device->GetCommandList());
            DiagLog(extraFile, "  SwapChain                 = %p", (void*)device->GetSwapChain());
            DiagLog(extraFile, "  BackBuffer                = %p", (void*)device->GetCurrentBackBuffer());
            DiagLog(extraFile, "  Resolution                = %u x %u", device->GetWidth(), device->GetHeight());
            DiagLog(extraFile, "  FrameIndex                = %u", device->GetFrameIndex());

            // Descriptor heaps
            DiagLog(extraFile, "  SRV Heap                  = %p", (void*)device->GetSRVHeap());
            DiagLog(extraFile, "  UAV Heap                  = %p", (void*)device->GetUAVHeap());
            DiagLog(extraFile, "  RTV Heap                  = %p", (void*)device->GetRTVHeap());
            DiagLog(extraFile, "  SRV Descriptor Size       = %u", device->GetSRVDescriptorSize());
            DiagLog(extraFile, "  RTV Descriptor Size       = %u", device->GetRTVDescriptorSize());
        } else {
            DiagLog(extraFile, "  (Device not initialized — no further DX12 state available)");
        }
    }

    // ---- 3. DXR Pipeline ----
    DiagLog(extraFile, "");
    DiagLog(extraFile, "--- DXRPipeline ---");

    RTX::DXRPipeline* pipeline = renderer->GetPipeline();
    if (!pipeline) {
        DiagLog(extraFile, "  DXRPipeline = NULL (not created)");
    } else {
        DiagLog(extraFile, "  DXRPipeline ptr           = %p", (void*)pipeline);

        ID3D12StateObject* stateObj = pipeline->GetStateObject();
        DiagLog(extraFile, "  StateObject (PSO)         = %p  [%s]",
                (void*)stateObj,
                stateObj ? "VALID" : "NULL — shaders not loaded!");

        ID3D12RootSignature* rootSig = pipeline->GetGlobalRootSignature();
        DiagLog(extraFile, "  GlobalRootSignature       = %p  [%s]",
                (void*)rootSig,
                rootSig ? "VALID" : "NULL — root sig not created!");

        // Output buffers
        ID3D12Resource* outputBuf = pipeline->GetOutputBuffer();
        ID3D12Resource* accumBuf = pipeline->GetAccumulationBuffer();
        ID3D12Resource* denoiseBuf = pipeline->GetDenoiseTempBuffer();
        ID3D12Resource* constBuf = pipeline->GetConstantBuffer();

        DiagLog(extraFile, "  OutputBuffer              = %p  [%s]",
                (void*)outputBuf, outputBuf ? "VALID" : "NULL");
        DiagLog(extraFile, "  AccumulationBuffer        = %p  [%s]",
                (void*)accumBuf, accumBuf ? "VALID" : "NULL");
        DiagLog(extraFile, "  DenoiseTempBuffer         = %p  [%s]",
                (void*)denoiseBuf, denoiseBuf ? "VALID" : "NULL");
        DiagLog(extraFile, "  ConstantBuffer            = %p  [%s]",
                (void*)constBuf, constBuf ? "VALID" : "NULL");

        if (outputBuf) {
            D3D12_GPU_DESCRIPTOR_HANDLE uavHandle = pipeline->GetOutputUAV();
            DiagLog(extraFile, "  OutputUAV GPU Handle      = 0x%llX  [%s]",
                    (unsigned long long)uavHandle.ptr,
                    uavHandle.ptr != 0 ? "VALID" : "NULL — UAV descriptors not created!");
        }
    }

    // ---- 4. Texture Manager ----
    DiagLog(extraFile, "");
    DiagLog(extraFile, "--- TextureManager ---");

    // TextureManager is a singleton — safe to query even if renderer has a different instance
    RTX::TextureManager& texMgr = RTX::TextureManager::GetInstance();
    DiagLog(extraFile, "  TextureManager singleton  = %p", (void*)&texMgr);

    // Check initialization by probing for the SRV heap (no public IsInitialized method)
    ID3D12DescriptorHeap* texSrvHeap = texMgr.GetSRVHeap();
    bool texMgrInitialized = (texSrvHeap != nullptr);
    DiagLog(extraFile, "  Initialized (SRV Heap)    = %s", texMgrInitialized ? "YES" : "NO");

    if (texMgrInitialized) {
        DiagLog(extraFile, "  SRV Heap (owned)          = %p", (void*)texSrvHeap);
        DiagLog(extraFile, "  Max Cached Textures       = %u", texMgr.GetMaxCachedTextures());
        DiagLog(extraFile, "  Cached Texture Count      = %u", texMgr.GetTextureCount());
        DiagLog(extraFile, "  Next SRV Index            = %u", texMgr.GetNextSRVIndex());

        D3D12_GPU_DESCRIPTOR_HANDLE whiteTex = texMgr.GetDefaultWhiteTextureSRV();
        DiagLog(extraFile, "  DefaultWhiteTexture SRV   = 0x%llX  [%s]",
                (unsigned long long)whiteTex.ptr,
                whiteTex.ptr != 0 ? "VALID" : "NULL");

        D3D12_GPU_DESCRIPTOR_HANDLE tableStart = texMgr.GetSRVTableStart();
        DiagLog(extraFile, "  SRV Table GPU Start       = 0x%llX  [%s]",
                (unsigned long long)tableStart.ptr,
                tableStart.ptr != 0 ? "VALID" : "NULL");
    }

    // ---- 5. RTXManager ----
    DiagLog(extraFile, "");
    DiagLog(extraFile, "--- RTXManager ---");

    RTX::RTXManager& mgr = RTX::RTXManager::Get();
    DiagLog(extraFile, "  RTXManager singleton      = %p", (void*)&mgr);
    DiagLog(extraFile, "  IsInitialized             = %s", mgr.IsInitialized() ? "YES" : "NO");
    DiagLog(extraFile, "  IsRTXSupported            = %s", mgr.IsRTXSupported() ? "YES" : "NO");

    // ---- 6. Summary / Health Check ----
    DiagLog(extraFile, "");
    DiagLog(extraFile, "--- HEALTH CHECK ---");

    bool healthy = true;
    auto check = [&](const char* component, bool ok) {
        DiagLog(extraFile, "  [%s] %s", ok ? " OK " : "FAIL", component);
        if (!ok) healthy = false;
    };

    check("DX12 Device initialized",       device && device->IsInitialized());
    check("DX12 Raytracing supported",      device && device->SupportsRaytracing());
    check("DX12 SwapChain created",         device && device->GetSwapChain() != nullptr);
    check("DXR State Object (PSO) created", pipeline && pipeline->GetStateObject() != nullptr);
    check("Global Root Signature created",  pipeline && pipeline->GetGlobalRootSignature() != nullptr);
    check("Output Buffer allocated",        pipeline && pipeline->GetOutputBuffer() != nullptr);
    check("Accumulation Buffer allocated",  pipeline && pipeline->GetAccumulationBuffer() != nullptr);
    check("Constant Buffer allocated",      pipeline && pipeline->GetConstantBuffer() != nullptr);
    check("TextureManager initialized",     texMgrInitialized);
    check("TextureManager SRV Heap valid",  texSrvHeap != nullptr);

    DiagLog(extraFile, "");
    if (healthy) {
        DiagLog(extraFile, "  >>> RTX PIPELINE: ALL CHECKS PASSED <<<");
    } else {
        DiagLog(extraFile, "  >>> RTX PIPELINE: ONE OR MORE CHECKS FAILED <<<");
        DiagLog(extraFile, "  >>> Review the FAIL items above. <<<");
    }

    DiagLog(extraFile, "========================================================");
}

// ============================================================================
// RTX_LogFrameStats
// ============================================================================

void RTX_LogFrameStats(uint32_t frameNumber) {
    if (frameNumber == 0) {
        frameNumber = ++s_diagFrameCounter;
    } else {
        s_diagFrameCounter = frameNumber;
    }

    RTX::RTXRenderer* renderer = RTX::RTXRenderer::Instance();
    if (!renderer) {
        RTX_DIAG("[Frame %u] RTXRenderer not active (Instance=NULL)", frameNumber);
        return;
    }

    if (!RTX::RTXRenderer::IsActive()) {
        RTX_DIAG("[Frame %u] RTXRenderer exists but not active (DX12 not initialized)", frameNumber);
        return;
    }

    RTX::DX12Device* device = renderer->GetDevice();
    RTX::DXRPipeline* pipeline = renderer->GetPipeline();

    // Basic frame info
    uint32_t width = device ? device->GetWidth() : 0;
    uint32_t height = device ? device->GetHeight() : 0;
    uint32_t frameIdx = device ? device->GetFrameIndex() : 0;

    // Pipeline state
    bool hasPSO = pipeline && pipeline->GetStateObject() != nullptr;
    bool hasOutput = pipeline && pipeline->GetOutputBuffer() != nullptr;

    // Scene state
    bool sceneLoaded = renderer->IsSceneLoaded();
    bool rtxSceneActive = renderer->IsRTXSceneActive();
    int currentScene = renderer->GetCurrentScene();

    // Texture stats
    RTX::TextureManager& texMgr = RTX::TextureManager::GetInstance();
    bool texInit = (texMgr.GetSRVHeap() != nullptr);
    uint32_t texCount = texInit ? texMgr.GetTextureCount() : 0;
    uint32_t texCapacity = texInit ? texMgr.GetMaxCachedTextures() : 0;

    // Determine dispatch status
    const char* dispatchStatus = "UNKNOWN";
    if (!hasPSO) {
        dispatchStatus = "NO (no PSO)";
    } else if (!sceneLoaded) {
        dispatchStatus = "NO (no scene)";
    } else if (!rtxSceneActive) {
        dispatchStatus = "NO (non-RTX scene)";
    } else {
        dispatchStatus = "YES (dispatching)";
    }

    RTX_DIAG("[Frame %u] res=%ux%u bufIdx=%u scene=%d loaded=%s rtxActive=%s pso=%s dispatch=%s textures=%u/%u",
             frameNumber,
             width, height,
             frameIdx,
             currentScene,
             sceneLoaded ? "Y" : "N",
             rtxSceneActive ? "Y" : "N",
             hasPSO ? "Y" : "N",
             dispatchStatus,
             texCount, texCapacity);
}

// ============================================================================
// RTX_GetStatusLine
// ============================================================================

const char* RTX_GetStatusLine() {
    RTX::RTXRenderer* renderer = RTX::RTXRenderer::Instance();

    if (!renderer) {
        snprintf(s_statusLineBuffer, sizeof(s_statusLineBuffer),
                 "RTX ERR: RTXRenderer not created (Instance=NULL)");
        return s_statusLineBuffer;
    }

    RTX::DX12Device* device = renderer->GetDevice();
    if (!device || !device->IsInitialized()) {
        snprintf(s_statusLineBuffer, sizeof(s_statusLineBuffer),
                 "RTX ERR: DX12 device not initialized (device=%p, init=%s)",
                 (void*)device,
                 device ? (device->IsInitialized() ? "yes" : "no") : "N/A");
        return s_statusLineBuffer;
    }

    if (!device->SupportsRaytracing()) {
        snprintf(s_statusLineBuffer, sizeof(s_statusLineBuffer),
                 "RTX ERR: DX12 active but raytracing NOT supported on this GPU");
        return s_statusLineBuffer;
    }

    RTX::DXRPipeline* pipeline = renderer->GetPipeline();
    if (!pipeline || !pipeline->GetStateObject()) {
        snprintf(s_statusLineBuffer, sizeof(s_statusLineBuffer),
                 "RTX WARN: DX12+DXR ready but PSO not created (pipeline=%p, pso=%p)",
                 (void*)pipeline,
                 pipeline ? (void*)pipeline->GetStateObject() : nullptr);
        return s_statusLineBuffer;
    }

    // Check texture manager
    RTX::TextureManager& texMgrS = RTX::TextureManager::GetInstance();
    bool texInitS = (texMgrS.GetSRVHeap() != nullptr);
    uint32_t texCount = texInitS ? texMgrS.GetTextureCount() : 0;

    if (!renderer->IsSceneLoaded()) {
        snprintf(s_statusLineBuffer, sizeof(s_statusLineBuffer),
                 "RTX OK: DX12+DXR active, PSO ready, %u textures cached, no scene loaded",
                 texCount);
        return s_statusLineBuffer;
    }

    bool rtxScene = renderer->IsRTXSceneActive();
    int scene = renderer->GetCurrentScene();

    snprintf(s_statusLineBuffer, sizeof(s_statusLineBuffer),
             "RTX %s: DX12+DXR active, scene=%d, rtxScene=%s, %u textures, %ux%u",
             rtxScene ? "OK" : "IDLE",
             scene,
             rtxScene ? "YES (dispatching)" : "NO (non-RTX scene)",
             texCount,
             device->GetWidth(), device->GetHeight());

    return s_statusLineBuffer;
}

#endif // ENABLE_DX12_RTX
