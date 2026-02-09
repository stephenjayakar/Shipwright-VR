#ifdef ENABLE_DX12_RTX

#include "RTXHooks.h"
#include "RTXRenderer.h"
#include "RTXSceneConfig.h"
#include "TextureManager.h"
#include <libultraship/bridge/consolevariablebridge.h>
#include <spdlog/spdlog.h>
#include <cstdio>
#include <vector>
#include <utility>

#ifdef _WIN32
#include <Windows.h>
#endif

#include "RTXDiagLog.h"

// Include game headers for PlayState access.
// These headers have their own extern "C" guards internally;
// wrapping them in an additional extern "C" block causes C++
// templates from libultraship bridge headers to break.
#include "global.h"
#include "z64.h"

// CVar for runtime RTX toggle (requires ENABLE_DX12_RTX at compile time).
// When this CVar is 0, RTX hooks act as no-ops even if the system supports DXR.
// Default is 1 (enabled) so that RTX is active on DXR-capable hardware.
// Users can disable RTX at runtime via the developer console or settings.
#define CVAR_RTX_ENABLED "gEnhancements.RTX.Enabled"

// Track RTX initialization state
static bool s_rtxProbeAttempted = false;   // True after we've tried to probe DX12/DXR
static bool s_rtxProbeSucceeded = false;   // True if DX12 + DXR is available
static bool s_rtxInitAttempted = false;    // True after we've tried full initialization
static bool s_rtxInitSucceeded = false;    // True if DX12 swap chain is active

// Deferred room loading queue.
// If rooms finish loading before RTX initialization completes (race between
// async room loading and synchronous DX12 init), store the (play, roomNum)
// pairs here and process them once the renderer is active.
struct DeferredRoom {
    void* play;   // PlayState* (stored as void* for C-linkage header compatibility)
    int roomNum;
};
static std::vector<DeferredRoom> s_deferredRooms;

// Helper: process any deferred room loads that were queued before RTX was active.
static void ProcessDeferredRooms() {
    if (s_deferredRooms.empty()) return;

    auto* renderer = RTX::RTXRenderer::Instance();
    if (!renderer || !RTX::RTXRenderer::IsActive()) {
        RTX_DIAG("ProcessDeferredRooms: %zu rooms queued but renderer not active yet (renderer=%p, active=%s)",
                 s_deferredRooms.size(), (void*)renderer,
                 (renderer && RTX::RTXRenderer::IsActive()) ? "yes" : "no");
        return;
    }

    RTX_DIAG("ProcessDeferredRooms: processing %zu deferred room loads", s_deferredRooms.size());
    SPDLOG_INFO("[RTX] Processing {} deferred room loads", s_deferredRooms.size());
    for (const auto& dr : s_deferredRooms) {
        RTX_DIAG("ProcessDeferredRooms: loading deferred room %d (play=%p)", dr.roomNum, dr.play);
        renderer->OnRoomLoaded(dr.play, dr.roomNum);
    }
    s_deferredRooms.clear();
    RTX_DIAG("ProcessDeferredRooms: all deferred rooms processed");
}

// Helper: check if RTX is enabled via CVar. Returns true if the CVar is not set
// (default enabled) or explicitly set to non-zero.
static bool IsRTXEnabledByCVar() {
    // CVarGetInteger returns 0 if the CVar doesn't exist, so we default to 1 (enabled).
    // This means RTX is enabled by default on first run; users must explicitly disable it.
    int cvarVal = CVarGetInteger(CVAR_RTX_ENABLED, 1);
    static bool s_loggedOnce = false;
    if (!s_loggedOnce) {
        RTX_DIAG("IsRTXEnabledByCVar: CVar '%s' = %d (enabled=%s)", CVAR_RTX_ENABLED, cvarVal, cvarVal != 0 ? "YES" : "NO");
        s_loggedOnce = true;
    }
    return cvarVal != 0;
}

// Helper: get the HWND and dimensions from the DXGI backend (or fallback).
// Does NOT release the DX11 swap chain.
static HWND GetWindowHWND() {
#ifdef _WIN32
    RTX_DIAG("GetWindowHWND: attempting to acquire window handle...");
    // First try the DXGI backend's stored HWND
    void* rawHwnd = nullptr;
    __try {
        rawHwnd = RTX_GetWindowHWND();
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        RTX_DIAG("GetWindowHWND: SEH exception 0x%08X in RTX_GetWindowHWND, using Win32 fallback", GetExceptionCode());
        SPDLOG_WARN("[RTX] Exception in RTX_GetWindowHWND, using Win32 fallback");
        rawHwnd = nullptr;
    }

    if (rawHwnd) {
        RTX_DIAG("GetWindowHWND: got HWND=%p from RTX_GetWindowHWND", rawHwnd);
        return (HWND)rawHwnd;
    }

    // Fallback: use Win32 API
    HWND hwnd = GetActiveWindow();
    RTX_DIAG("GetWindowHWND: GetActiveWindow() returned %p", (void*)hwnd);
    if (!hwnd) {
        hwnd = GetForegroundWindow();
        RTX_DIAG("GetWindowHWND: GetForegroundWindow() returned %p", (void*)hwnd);
    }
    if (hwnd) {
        SPDLOG_INFO("[RTX] Using fallback HWND via Win32 API: {}", (void*)hwnd);
    } else {
        RTX_DIAG("GetWindowHWND: FAILED - no HWND available from any source!");
    }
    return hwnd;
#else
    return nullptr;
#endif
}

static void GetWindowDims(HWND hwnd, unsigned int& width, unsigned int& height) {
#ifdef _WIN32
    RTX_DIAG("GetWindowDims: querying dimensions for HWND=%p", (void*)hwnd);
    width = 0;
    height = 0;

    __try {
        RTX_GetWindowDimensions(&width, &height);
        RTX_DIAG("GetWindowDims: RTX_GetWindowDimensions returned %ux%u", width, height);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        RTX_DIAG("GetWindowDims: SEH exception 0x%08X in RTX_GetWindowDimensions", GetExceptionCode());
        SPDLOG_WARN("[RTX] Exception in RTX_GetWindowDimensions, using GetClientRect fallback");
        width = 0;
        height = 0;
    }

    if (width == 0 || height == 0) {
        RECT rect;
        if (hwnd && GetClientRect(hwnd, &rect)) {
            width = rect.right - rect.left;
            height = rect.bottom - rect.top;
            RTX_DIAG("GetWindowDims: GetClientRect fallback returned %ux%u", width, height);
        }
        if (width == 0 || height == 0) {
            width = 1280;
            height = 960;
            RTX_DIAG("GetWindowDims: using hardcoded fallback %ux%u", width, height);
        }
    }
#else
    width = 1280;
    height = 960;
#endif
}

// Attempt lazy RTX initialization when first entering an RTX-enabled scene.
// Uses a two-phase approach to avoid tearing down DX11 if DX12/DXR isn't available:
//   Phase 1 (Probe): Create DX12 device and check DXR support. No DX11 changes.
//   Phase 2 (Initialize): Tear down DX11 swap chain, create DX12 swap chain + pipeline.
static void TryLazyInitialize() {
    RTX_DIAG("RTX Hook: TryLazyInitialize entry (initSucceeded=%d, initAttempted=%d, probeAttempted=%d, probeSucceeded=%d)",
             (int)s_rtxInitSucceeded, (int)s_rtxInitAttempted, (int)s_rtxProbeAttempted, (int)s_rtxProbeSucceeded);
    if (s_rtxInitSucceeded) return;     // Already initialized
    if (s_rtxInitAttempted) return;      // Already tried and failed (DX11 was torn down)

#ifdef _WIN32
    // ========================================================================
    // Phase 1: Probe DX12/DXR support WITHOUT touching DX11
    // ========================================================================
    if (!s_rtxProbeAttempted) {
        s_rtxProbeAttempted = true;

        auto* renderer = RTX::RTXRenderer::Instance();
        if (!renderer) {
            SPDLOG_ERROR("[RTX] No RTXRenderer instance - RTX disabled");
            return;
        }

        s_rtxProbeSucceeded = renderer->ProbeRTXSupport();
        if (!s_rtxProbeSucceeded) {
            SPDLOG_WARN("[RTX] DX12/DXR not available on this system - RTX disabled");
            return;
        }

        SPDLOG_INFO("[RTX] Phase 1 complete: DX12 + DXR available");
    }

    if (!s_rtxProbeSucceeded) {
        // Probe failed earlier, don't retry
        return;
    }

    // ========================================================================
    // Phase 2: Tear down DX11 swap chain and complete DX12 initialization
    // ========================================================================
    s_rtxInitAttempted = true;

    HWND hwnd = nullptr;
    unsigned int width = 0, height = 0;

    // Get HWND first (before teardown, so we have fallback info)
    hwnd = GetWindowHWND();
    if (!hwnd) {
        SPDLOG_ERROR("[RTX] Failed to acquire HWND - RTX disabled");
        return;
    }

    // Get dimensions before teardown
    GetWindowDims(hwnd, width, height);

    // Now tear down the DX11 swap chain.
    // This is the point of no return — after this, DX11 cannot present.
    SPDLOG_INFO("[RTX] Phase 2: Tearing down DX11 swap chain for DX12 takeover...");

    __try {
        void* acquiredHwnd = RTX_AcquireWindowForDX12();
        if (acquiredHwnd) {
            // Prefer the HWND from the acquire function (it's verified by DXGI)
            hwnd = (HWND)acquiredHwnd;
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        SPDLOG_WARN("[RTX] Exception 0x{:08X} in RTX_AcquireWindowForDX12, continuing with existing HWND",
                     GetExceptionCode());
        // Continue with the HWND we already have — the DX11 swap chain may or
        // may not be released at this point, but we must proceed since we've
        // committed to the DX12 path.
    }

    // Complete DX12 initialization (creates swap chain on the now-free HWND)
    SPDLOG_INFO("[RTX] Completing DX12 initialization ({}x{}, HWND={})", width, height, (void*)hwnd);

    auto* renderer = RTX::RTXRenderer::Instance();
    if (renderer) {
        s_rtxInitSucceeded = renderer->CompleteInitialization(hwnd, width, height);
    }

    if (s_rtxInitSucceeded) {
        SPDLOG_INFO("[RTX] ✓ RTX initialization succeeded — DX12 swap chain active");
    } else {
        SPDLOG_ERROR("[RTX] ✗ RTX initialization FAILED after DX11 teardown");
        SPDLOG_ERROR("[RTX] The game may show a black screen. DX11 swap chain was already released.");
        // In this state: DX11 swap chain is gone, DX12 swap chain failed.
        // The game will check RTX_IsDX11SwapChainReleased() in OTRGlobals.cpp
        // to skip DX11 present and avoid a crash.
    }
#else
    SPDLOG_INFO("[RTX] RTX is only supported on Windows - disabled");
#endif
}

extern "C" {

int RTX_IsActive(void) {
    bool cvarEnabled = IsRTXEnabledByCVar();
    bool rendererActive = RTX::RTXRenderer::IsActive();
    int result = (cvarEnabled && rendererActive) ? 1 : 0;
    // Log only on transitions or first few calls to avoid flooding
    static int s_callCount = 0;
    static int s_lastResult = -1;
    s_callCount++;
    if (result != s_lastResult || s_callCount <= 5) {
        printf("[RTX] RTX_IsActive called (call #%d) -> returning: %d (CVar=%s, renderer=%s)\n",
               s_callCount, result, cvarEnabled ? "ON" : "OFF", rendererActive ? "active" : "inactive");
        RTX_DIAG("[DIAG] RTX_IsActive() = %d (CVar=%s, RendererActive=%s) [call #%d]",
                 result,
                 cvarEnabled ? "ON" : "OFF",
                 rendererActive ? "active" : "inactive",
                 s_callCount);
        s_lastResult = result;
    }
    return result;
}

int RTX_ProbeSupport(void) {
    printf("[RTX] RTX_ProbeSupport called\n");
    RTX_DIAG("RTX Hook: RTX_ProbeSupport called");
    auto* renderer = RTX::RTXRenderer::Instance();
    if (!renderer) {
        RTX_DIAG("RTX Hook: RTX_ProbeSupport - no renderer instance, returning 0");
        return 0;
    }
    bool result = renderer->ProbeRTXSupport();
    RTX_DIAG("RTX Hook: RTX_ProbeSupport result=%s", result ? "SUPPORTED" : "NOT SUPPORTED");
    return result ? 1 : 0;
}

int RTX_IsKokiriForest(void* playPtr) {
    PlayState* play = (PlayState*)playPtr;
    int result = RTX::IsRTXScene(static_cast<uint16_t>(play->sceneNum)) ? 1 : 0;
    static int s_lastScene = -1;
    if (play->sceneNum != s_lastScene) {
        RTX_DIAG("RTX Hook: RTX_IsKokiriForest scene=0x%02X (%d), isRTXScene=%s",
                 play->sceneNum, play->sceneNum, result ? "YES" : "NO");
        s_lastScene = play->sceneNum;
    }
    return result;
}

void RTX_UpdateSceneParams(void* playPtr) {
    static uint32_t s_updateCount = 0;
    s_updateCount++;
    if (s_updateCount <= 5 || (s_updateCount % 300) == 0) {
        printf("[RTX] RTX_UpdateSceneParams called (call #%u)\n", s_updateCount);
    }
    PlayState* play = (PlayState*)playPtr;
    if (!play) {
        if (s_updateCount <= 3) {
            RTX_DIAG("RTX Hook: RTX_UpdateSceneParams called with null PlayState (call #%u)", s_updateCount);
        }
        return;
    }

    auto* renderer = RTX::RTXRenderer::Instance();
    if (!renderer) {
        if (s_updateCount <= 3) {
            RTX_DIAG("RTX Hook: RTX_UpdateSceneParams - no renderer instance (call #%u)", s_updateCount);
        }
        return;
    }
    if (s_updateCount <= 5 || (s_updateCount % 300) == 0) {
        RTX_DIAG("RTX Hook: RTX_UpdateSceneParams called (call #%u, scene=%d, frame=%u, eye=%.1f,%.1f,%.1f)",
                 s_updateCount, play->sceneNum, play->gameplayFrames,
                 play->view.eye.x, play->view.eye.y, play->view.eye.z);
    }

    // Extract view/projection matrices from play->view.
    // play->view.viewing and play->view.projection are Mtx (fixed-point on N64).
    // Convert to MtxF (float) before extracting values.
    float viewMatrix[16];
    float projMatrix[16];
    float cameraPos[3];

    MtxF viewMtxF;
    MtxF projMtxF;
    Matrix_MtxToMtxF(&play->view.viewing, &viewMtxF);
    Matrix_MtxToMtxF(&play->view.projection, &projMtxF);

    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            viewMatrix[i * 4 + j] = viewMtxF.mf[i][j];
            projMatrix[i * 4 + j] = projMtxF.mf[i][j];
        }
    }

    // Camera position from play->view.eye
    cameraPos[0] = play->view.eye.x;
    cameraPos[1] = play->view.eye.y;
    cameraPos[2] = play->view.eye.z;

    // Extract lighting from environment context
    float ambientColor[3];
    float fogColor[3];

    // Ambient color from current light settings
    ambientColor[0] = play->envCtx.lightSettings.ambientColor[0] / 255.0f;
    ambientColor[1] = play->envCtx.lightSettings.ambientColor[1] / 255.0f;
    ambientColor[2] = play->envCtx.lightSettings.ambientColor[2] / 255.0f;

    // Fog color
    fogColor[0] = play->envCtx.lightSettings.fogColor[0] / 255.0f;
    fogColor[1] = play->envCtx.lightSettings.fogColor[1] / 255.0f;
    fogColor[2] = play->envCtx.lightSettings.fogColor[2] / 255.0f;

    float fogNear = (float)play->envCtx.lightSettings.fogNear;
    float fogFar = (float)play->envCtx.lightSettings.fogFar;

    // Directional light 1
    float sunDir1[3];
    sunDir1[0] = play->envCtx.lightSettings.light1Dir[0] / 127.0f;
    sunDir1[1] = play->envCtx.lightSettings.light1Dir[1] / 127.0f;
    sunDir1[2] = play->envCtx.lightSettings.light1Dir[2] / 127.0f;

    float sunColor1[3];
    sunColor1[0] = play->envCtx.lightSettings.light1Color[0] / 255.0f;
    sunColor1[1] = play->envCtx.lightSettings.light1Color[1] / 255.0f;
    sunColor1[2] = play->envCtx.lightSettings.light1Color[2] / 255.0f;

    // Directional light 2
    float sunDir2[3];
    sunDir2[0] = play->envCtx.lightSettings.light2Dir[0] / 127.0f;
    sunDir2[1] = play->envCtx.lightSettings.light2Dir[1] / 127.0f;
    sunDir2[2] = play->envCtx.lightSettings.light2Dir[2] / 127.0f;

    float sunColor2[3];
    sunColor2[0] = play->envCtx.lightSettings.light2Color[0] / 255.0f;
    sunColor2[1] = play->envCtx.lightSettings.light2Color[1] / 255.0f;
    sunColor2[2] = play->envCtx.lightSettings.light2Color[2] / 255.0f;

    // Scene setup info for Deku Tree death effects
    bool dekuTreeDead = Flags_GetEventChkInf(EVENTCHKINF_OBTAINED_KOKIRI_EMERALD_DEKU_TREE_DEAD);
    bool isAdultLink = LINK_IS_ADULT;

    renderer->UpdateSceneParams(
        play->gameplayFrames,
        gSaveContext.sceneSetupIndex,
        (float)play->roomCtx.unk_74[0],
        dekuTreeDead,
        isAdultLink,
        viewMatrix,
        projMatrix,
        cameraPos,
        ambientColor,
        fogColor,
        fogNear,
        fogFar,
        sunDir1,
        sunColor1,
        sunDir2,
        sunColor2
    );
}

void RTX_DispatchAndPresent(void) {
    // No CVar check needed here — RTX_IsActive() already checks CVar,
    // and the caller in OTRGlobals.cpp gates on RTX_IsActive().
    static uint32_t s_frameCounter = 0;
    s_frameCounter++;
    if (s_frameCounter <= 5 || (s_frameCounter % 300) == 0) {
        printf("[RTX] RTX_DispatchAndPresent called (frame #%u)\n", s_frameCounter);
    }
    auto* renderer = RTX::RTXRenderer::Instance();
    if (renderer) {
        if (s_frameCounter <= 5 || (s_frameCounter % 300) == 0) {
            RTX_DIAG("[DIAG] RTX_DispatchAndPresent HOOK FIRED: frame #%u, scene=%d, rendererActive=%s, RTX_IsActive=%d",
                     s_frameCounter,
                     renderer->IsRTXSceneActive() ? renderer->GetCurrentScene() : -1,
                     RTX::RTXRenderer::IsActive() ? "yes" : "no",
                     RTX_IsActive());
        }

        // Process any deferred rooms that were queued before initialization.
        // This catches the edge case where rooms loaded between scene load
        // and the first frame render.
        ProcessDeferredRooms();

        renderer->DispatchAndPresent();
    } else {
        if (s_frameCounter <= 3) {
            RTX_DIAG("RTX Hook: Frame begin but no RTXRenderer instance!");
        }
    }
}

void RTX_OnRoomLoaded(void* playPtr, int roomNum) {
    printf("[RTX] RTX_OnRoomLoaded called (room=%d)\n", roomNum);
    RTX_DIAG("[DIAG] RTX_OnRoomLoaded HOOK FIRED: room=%d, play=%p, CVar=%s, RTX_IsActive=%d",
             roomNum, playPtr, IsRTXEnabledByCVar() ? "ON" : "OFF", RTX_IsActive());
    if (!IsRTXEnabledByCVar()) return;

    auto* renderer = RTX::RTXRenderer::Instance();
    if (renderer && RTX::RTXRenderer::IsActive()) {
        renderer->OnRoomLoaded(playPtr, roomNum);
    } else {
        // RTX not active yet — this can happen if the room finishes loading
        // before the DX12 initialization completes. Queue the room for deferred
        // processing once the renderer is active.
        SPDLOG_INFO("[RTX] RTX_OnRoomLoaded: RTX not yet active, deferring room {} for later processing", roomNum);
        s_deferredRooms.push_back({ playPtr, roomNum });
    }
}

void RTX_OnSceneLoaded(int sceneNum) {
    printf("[RTX] RTX_OnSceneLoaded called\n");
    printf("[RTX] Scene changed to: %d\n", sceneNum);
    RTX_DIAG("[DIAG] RTX_OnSceneLoaded HOOK FIRED: sceneNum=0x%02X (dec=%d), isRTXScene=%s, CVar=%s, RTX_IsActive=%d",
             sceneNum, sceneNum,
             RTX::IsRTXScene(static_cast<uint16_t>(sceneNum)) ? "yes" : "no",
             IsRTXEnabledByCVar() ? "ON" : "OFF",
             RTX_IsActive());
    SPDLOG_INFO("[RTX] RTX_OnSceneLoaded: scene 0x{:02X} (RTX scene: {}, CVar enabled: {})",
                sceneNum,
                RTX::IsRTXScene(static_cast<uint16_t>(sceneNum)) ? "yes" : "no",
                IsRTXEnabledByCVar() ? "yes" : "no");

    if (!IsRTXEnabledByCVar()) return;

    // Clear any stale deferred rooms from a previous scene
    s_deferredRooms.clear();

    // If this is an RTX-enabled scene and we haven't initialized yet, do lazy init
    if (RTX::IsRTXScene(static_cast<uint16_t>(sceneNum))) {
        TryLazyInitialize();
    }

    auto* renderer = RTX::RTXRenderer::Instance();
    if (renderer && RTX::RTXRenderer::IsActive()) {
        renderer->OnSceneLoaded(sceneNum);

        // Process any rooms that were deferred during initialization.
        // This handles the case where rooms finish loading while DX12 was still
        // being initialized by TryLazyInitialize() above.
        ProcessDeferredRooms();
    }
}

void RTX_OnSceneUnload(void) {
    printf("[RTX] RTX_OnSceneUnload called\n");
    RTX_DIAG("[DIAG] RTX_OnSceneUnload HOOK FIRED");
    s_deferredRooms.clear();
    auto* renderer = RTX::RTXRenderer::Instance();
    if (renderer && RTX::RTXRenderer::IsActive()) {
        renderer->OnSceneUnload();
    }
}

int RTX_Initialize(void* hwnd, unsigned int width, unsigned int height) {
    printf("[RTX] RTX_Initialize called (hwnd=%p, %ux%u)\n", hwnd, width, height);
    RTX_DIAG("RTX Hook: RTX_Initialize called, hwnd=%p, %ux%u", hwnd, width, height);
    auto* renderer = RTX::RTXRenderer::Instance();
    if (!renderer) {
        RTX_DIAG("RTX Hook: RTX_Initialize FAILED - no renderer instance");
        return 0;
    }
    bool ok = renderer->Initialize((HWND)hwnd, width, height);
    RTX_DIAG("RTX Hook: RTX_Initialize result=%s", ok ? "SUCCESS" : "FAILED");
    return ok ? 1 : 0;
}

void RTX_Shutdown(void) {
    printf("[RTX] RTX_Shutdown called\n");
    RTX_DIAG("RTX Hook: RTX_Shutdown called (probeAttempted=%d, probeSucceeded=%d, initAttempted=%d, initSucceeded=%d)",
             (int)s_rtxProbeAttempted, (int)s_rtxProbeSucceeded, (int)s_rtxInitAttempted, (int)s_rtxInitSucceeded);
    auto* renderer = RTX::RTXRenderer::Instance();
    if (renderer) {
        RTX_DIAG("RTX Hook: RTX_Shutdown - shutting down renderer");
        renderer->Shutdown();
    } else {
        RTX_DIAG("RTX Hook: RTX_Shutdown - no renderer instance");
    }
    s_rtxProbeAttempted = false;
    s_rtxProbeSucceeded = false;
    s_rtxInitAttempted = false;
    s_rtxInitSucceeded = false;
    s_deferredRooms.clear();
    RTX_DIAG("RTX Hook: RTX_Shutdown complete - all state reset");
}

void RTX_InterceptTexture(const void* timgAddr, const unsigned char* rgbaData,
                          unsigned int width, unsigned int height, unsigned int format) {
    { static uint32_t s_interceptPrintCount = 0; s_interceptPrintCount++;
      if (s_interceptPrintCount <= 10 || (s_interceptPrintCount % 100) == 0)
        printf("[RTX] RTX_InterceptTexture called (%ux%u, fmt=%u, call #%u)\n", width, height, format, s_interceptPrintCount);
    }
    if (!RTX::RTXRenderer::IsActive()) {
        static uint32_t s_inactiveCount = 0;
        s_inactiveCount++;
        if (s_inactiveCount <= 3) {
            RTX_DIAG("RTX Hook: RTX_InterceptTexture skipped - renderer not active (skip #%u)", s_inactiveCount);
        }
        return;
    }
    if (!rgbaData || width == 0 || height == 0) {
        static uint32_t s_invalidCount = 0;
        s_invalidCount++;
        if (s_invalidCount <= 5) {
            RTX_DIAG("RTX Hook: RTX_InterceptTexture skipped - invalid data (rgbaData=%p, %ux%u, skip #%u)",
                     (const void*)rgbaData, width, height, s_invalidCount);
        }
        return;
    }

    static uint32_t s_texInterceptCount = 0;
    s_texInterceptCount++;
    if (s_texInterceptCount <= 10 || (s_texInterceptCount % 100) == 0) {
        RTX_DIAG("RTX Hook: RTX_InterceptTexture #%u, addr=%p, %ux%u, fmt=%u",
                 s_texInterceptCount, timgAddr, width, height, format);
    }

    // Compute a hash from the timg address (pointer value) and texture dimensions.
    // This gives a stable key for cache lookup as long as the same ROM address
    // maps to the same texture data across frames.
    uint64_t hash;
    {
        uintptr_t addr = reinterpret_cast<uintptr_t>(timgAddr);
        // FNV-1a seeded with the address, mixed with dimensions and format
        constexpr uint64_t FNV_OFFSET = 0xcbf29ce484222325ULL;
        constexpr uint64_t FNV_PRIME  = 0x100000001b3ULL;
        hash = FNV_OFFSET;
        for (size_t i = 0; i < sizeof(addr); i++) {
            hash ^= static_cast<uint64_t>((addr >> (i * 8)) & 0xFF);
            hash *= FNV_PRIME;
        }
        // Mix in dimensions so identical addresses with different sizes get unique keys
        hash ^= static_cast<uint64_t>(width);
        hash *= FNV_PRIME;
        hash ^= static_cast<uint64_t>(height);
        hash *= FNV_PRIME;
        hash ^= static_cast<uint64_t>(format);
        hash *= FNV_PRIME;
    }

    // Upload the decoded RGBA data to the GPU via the TextureManager singleton.
    RTX::RTXTextureHandle handle = RTX::TextureManager::GetInstance().GetOrUploadTexture(rgbaData, width, height, hash);

    // Also register an address-only hash as an alias so that
    // RTXRenderer::ResolveMaterialTextures (which only knows the texture address
    // at geometry extraction time, without width/height/format) can find this
    // texture in the cache.
    if (handle.srvIndex > 0) {
        uintptr_t addr = reinterpret_cast<uintptr_t>(timgAddr);
        constexpr uint64_t FNV_OFFSET = 0xcbf29ce484222325ULL;
        constexpr uint64_t FNV_PRIME  = 0x100000001b3ULL;
        uint64_t addrOnlyHash = FNV_OFFSET;
        for (size_t i = 0; i < sizeof(addr); i++) {
            addrOnlyHash ^= static_cast<uint64_t>((addr >> (i * 8)) & 0xFF);
            addrOnlyHash *= FNV_PRIME;
        }
        // Only register the alias if it differs from the full hash
        if (addrOnlyHash != hash) {
            RTX::TextureManager::GetInstance().RegisterHashAlias(addrOnlyHash, handle.srvIndex);
        }
        // Also register the raw address value as an alias (for the fallback path
        // in ResolveMaterialTextures that tries the raw address as a hash key)
        RTX::TextureManager::GetInstance().RegisterHashAlias(static_cast<uint64_t>(addr), handle.srvIndex);
    }
}

} // extern "C"

#endif // ENABLE_DX12_RTX
