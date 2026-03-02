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

// DX12 Bridge header from libultraship
#include <fast/backends/gfx_dx12_bridge.h>

// Declared in gfx_dxgi.cpp — block/unblock DX11 Present calls during DX12 transition.
extern "C" volatile bool g_rtxPresentBlocked;

// Callback registration function defined in gfx_dxgi.cpp (libultraship).
// We register a callback so the DXGI backend can query RTX state even if
// the g_rtxPresentBlocked global somehow isn't shared correctly.
extern "C" void gfx_dxgi_set_rtx_block_callback(bool (*callback)());

// Static callback that gfx_dxgi.cpp will call to check if DX11 should be blocked.
static bool RTXBlockQueryCallback() {
    return RTX::RTXRenderer::IsActive();
}

// ============================================================================
// Free-Fly Camera for RTX Renderer
// Toggle: F5
// Controls: WASD move, Mouse look, Q/E down/up, Shift = fast, Ctrl = slow
// Completely overrides the game camera when active.
// ============================================================================
#ifdef _WIN32
#include <cmath>

struct FreeFlyCamera {
    bool active = false;
    bool wasF5Pressed = false;
    float pos[3] = { 0, 0, 0 };
    float yaw = 0.0f;     // radians, 0 = looking down +Z
    float pitch = 0.0f;   // radians, clamped to [-89, 89] degrees
    bool initialized = false;
    POINT lastMousePos = { 0, 0 };
    bool hasLastMouse = false;

    float moveSpeed = 1500.0f;    // units per second (N64 units are roughly cm, 1500 = ~15m/s)
    float mouseSensitivity = 0.003f;

    void InitFromGameCamera(const float* camPos, const float* viewMtx) {
        pos[0] = camPos[0];
        pos[1] = camPos[1];
        pos[2] = camPos[2];
        // Extract forward from view matrix (row 2, negated because camera looks down -Z)
        float fwd[3] = { -viewMtx[8], -viewMtx[9], -viewMtx[10] };
        float fwdLen = sqrtf(fwd[0]*fwd[0] + fwd[1]*fwd[1] + fwd[2]*fwd[2]);
        if (fwdLen > 0.001f) {
            fwd[0] /= fwdLen; fwd[1] /= fwdLen; fwd[2] /= fwdLen;
        }
        yaw = atan2f(fwd[0], fwd[2]);
        pitch = asinf(fmaxf(-1.0f, fminf(1.0f, fwd[1])));
        initialized = true;
        hasLastMouse = false;
    }

    void GetForwardRight(float* fwd, float* right) {
        float cy = cosf(yaw), sy = sinf(yaw);
        float cp = cosf(pitch), sp = sinf(pitch);
        fwd[0] = sy * cp;
        fwd[1] = sp;
        fwd[2] = cy * cp;
        right[0] = cy;
        right[1] = 0.0f;
        right[2] = -sy;
    }

    // Build a row-major 4x4 view matrix matching the game's guLookAtF format.
    // RTXRenderer extracts: row0=right, row1=up, row2=-forward from viewMatrix[row*4+col]
    void BuildViewMatrix(float* out) {
        float fwd[3], right[3];
        GetForwardRight(fwd, right);
        // up = cross(fwd, right) — right-handed coordinate system
        float up[3] = {
            fwd[1]*right[2] - fwd[2]*right[1],
            fwd[2]*right[0] - fwd[0]*right[2],
            fwd[0]*right[1] - fwd[1]*right[0]
        };
        // Row 0 = right (RTXRenderer reads viewMatrix[0..2] as camRight)
        out[0] = right[0]; out[1] = right[1]; out[2] = right[2];
        out[3] = -(right[0]*pos[0] + right[1]*pos[1] + right[2]*pos[2]);
        // Row 1 = up (RTXRenderer reads viewMatrix[4..6] as camUp)
        out[4] = up[0]; out[5] = up[1]; out[6] = up[2];
        out[7] = -(up[0]*pos[0] + up[1]*pos[1] + up[2]*pos[2]);
        // Row 2 = -forward (RTXRenderer reads -viewMatrix[8..10] as camForward)
        out[8] = -fwd[0]; out[9] = -fwd[1]; out[10] = -fwd[2];
        out[11] = (fwd[0]*pos[0] + fwd[1]*pos[1] + fwd[2]*pos[2]);
        // Row 3
        out[12] = 0; out[13] = 0; out[14] = 0; out[15] = 1;
    }

    // Returns true if the camera is active and overriding the game camera
    bool Update(float dt, float* camPos, float* viewMtx) {
        // Toggle on F5
        bool f5Down = (GetAsyncKeyState(VK_F5) & 0x8000) != 0;
        if (f5Down && !wasF5Pressed) {
            active = !active;
            if (active && !initialized) {
                InitFromGameCamera(camPos, viewMtx);
            } else if (active) {
                // Re-sync position from game camera on re-enable
                InitFromGameCamera(camPos, viewMtx);
            }
            if (!active) {
                hasLastMouse = false;
            }
        }
        wasF5Pressed = f5Down;

        if (!active) return false;

        // --- Mouse look ---
        POINT cursorPos;
        GetCursorPos(&cursorPos);
        if (hasLastMouse) {
            int dx = cursorPos.x - lastMousePos.x;
            int dy = cursorPos.y - lastMousePos.y;
            yaw += dx * mouseSensitivity;
            pitch -= dy * mouseSensitivity;
            // Clamp pitch to avoid gimbal lock
            float maxPitch = 89.0f * 3.14159265f / 180.0f;
            if (pitch > maxPitch) pitch = maxPitch;
            if (pitch < -maxPitch) pitch = -maxPitch;
        }
        lastMousePos = cursorPos;
        hasLastMouse = true;

        // --- Keyboard movement ---
        float speed = moveSpeed;
        // Use GetAsyncKeyState for modifiers (these are not letter keys, less likely to conflict)
        if (GetAsyncKeyState(VK_SHIFT) & 0x8000) speed *= 5.0f;
        if (GetAsyncKeyState(VK_CONTROL) & 0x8000) speed *= 0.2f;

        float fwd[3], right[3];
        GetForwardRight(fwd, right);

        // Use GetKeyboardState which is more reliable than GetAsyncKeyState
        // when SDL/Raw Input may be intercepting keyboard events.
        BYTE keyState[256] = {};
        GetKeyboardState(keyState);
        bool wDown = (keyState['W'] & 0x80) != 0;
        bool sDown = (keyState['S'] & 0x80) != 0;
        bool aDown = (keyState['A'] & 0x80) != 0;
        bool dDown = (keyState['D'] & 0x80) != 0;
        bool qDown = (keyState['Q'] & 0x80) != 0;
        bool eDown = (keyState['E'] & 0x80) != 0;

        // Also try GetAsyncKeyState as a fallback
        if (!wDown) wDown = (GetAsyncKeyState('W') & 0x8000) != 0;
        if (!sDown) sDown = (GetAsyncKeyState('S') & 0x8000) != 0;
        if (!aDown) aDown = (GetAsyncKeyState('A') & 0x8000) != 0;
        if (!dDown) dDown = (GetAsyncKeyState('D') & 0x8000) != 0;
        if (!qDown) qDown = (GetAsyncKeyState('Q') & 0x8000) != 0;
        if (!eDown) eDown = (GetAsyncKeyState('E') & 0x8000) != 0;

        float moveX = 0, moveY = 0, moveZ = 0;
        if (wDown) { moveX += fwd[0]; moveY += fwd[1]; moveZ += fwd[2]; }
        if (sDown) { moveX -= fwd[0]; moveY -= fwd[1]; moveZ -= fwd[2]; }
        if (dDown) { moveX += right[0]; moveY += right[1]; moveZ += right[2]; }
        if (aDown) { moveX -= right[0]; moveY -= right[1]; moveZ -= right[2]; }
        if (eDown) { moveY += 1.0f; }
        if (qDown) { moveY -= 1.0f; }

        // Debug: log key states and position periodically or when any key is pressed
        {
            static uint32_t s_flyCamLogCount = 0;
            s_flyCamLogCount++;
            bool anyKeyDown = wDown || sDown || aDown || dDown || qDown || eDown;
            if (s_flyCamLogCount <= 5 || anyKeyDown || (s_flyCamLogCount % 120) == 0) {
                // Also read raw GetAsyncKeyState for diagnostics
                SHORT rawW = GetAsyncKeyState('W');
                RTX_DIAG("FlyCamera #%u: dt=%.4f pos=(%.1f,%.1f,%.1f) WASDQE=%d%d%d%d%d%d kbState[W]=%02X async[W]=%04X fwd=(%.2f,%.2f,%.2f) spd=%.0f",
                       s_flyCamLogCount, dt, pos[0], pos[1], pos[2],
                       (int)wDown, (int)aDown, (int)sDown, (int)dDown, (int)qDown, (int)eDown,
                       (unsigned)keyState['W'], (unsigned)rawW & 0xFFFF,
                       fwd[0], fwd[1], fwd[2], speed);
            }
        }

        float moveLen = sqrtf(moveX*moveX + moveY*moveY + moveZ*moveZ);
        if (moveLen > 0.001f) {
            float scale = speed * dt / moveLen;
            pos[0] += moveX * scale;
            pos[1] += moveY * scale;
            pos[2] += moveZ * scale;
        }

        // --- Write overridden camera data ---
        camPos[0] = pos[0];
        camPos[1] = pos[1];
        camPos[2] = pos[2];
        BuildViewMatrix(viewMtx);

        return true;
    }
};

static FreeFlyCamera s_flyCamera;

// ============================================================================
// Scene Warp System for RTX Renderer
// Toggle: F6 opens input mode, type scene number, Enter to warp, Escape to cancel
// Also supports F7=prev RTX scene, F8=next RTX scene for quick cycling
// ============================================================================

struct SceneWarpEntry {
    int sceneId;
    int entranceId;
    const char* name;
};

// All RTX-enabled scenes with their primary entrance IDs
static const SceneWarpEntry s_rtxScenes[] = {
    { 0x00, 0x000, "Deku Tree" },
    { 0x34, 0x0BB, "Link's House" },
    { 0x2D, 0x0C1, "Kokiri Shop" },
    { 0x43, 0x053, "Temple of Time" },
    { 0x51, 0x0CD, "Hyrule Field" },
    { 0x54, 0x0EA, "Zora's River" },
    { 0x55, 0x0EE, "Kokiri Forest" },
    { 0x56, 0x0FC, "Sacred Forest Meadow" },
    { 0x58, 0x108, "Zora's Domain" },
    { 0x5B, 0x11E, "Lost Woods" },
};
static constexpr int NUM_RTX_SCENES = sizeof(s_rtxScenes) / sizeof(s_rtxScenes[0]);

struct SceneWarpInput {
    bool active = false;
    bool wasF6Pressed = false;
    bool wasF7Pressed = false;
    bool wasF8Pressed = false;
    bool wasEnterPressed = false;
    bool wasEscPressed = false;
    bool prevDigitState[10] = {};
    char inputBuffer[8] = {};  // up to 3 digits + null
    int inputLen = 0;

    // Find the index in s_rtxScenes for a given sceneId, or -1
    static int FindRTXSceneIndex(int sceneId) {
        for (int i = 0; i < NUM_RTX_SCENES; i++) {
            if (s_rtxScenes[i].sceneId == sceneId) return i;
        }
        return -1;
    }

    // Get the name for a scene number (from the RTX table or "Unknown")
    static const char* GetSceneName(int sceneId) {
        int idx = FindRTXSceneIndex(sceneId);
        if (idx >= 0) return s_rtxScenes[idx].name;
        return "Unknown (not RTX-enabled)";
    }

    // Warp to a scene by entrance ID
    static void DoWarp(void* playPtr, int entranceId, const char* sceneName) {
        PlayState* play = (PlayState*)playPtr;
        if (!play) return;
        play->nextEntranceIndex = entranceId;
        play->transitionTrigger = TRANS_TRIGGER_START;
        play->transitionType = TRANS_TYPE_INSTANT;
        printf("[RTX] WARP -> %s (entrance 0x%03X)\n", sceneName, entranceId);
        RTX_DIAG("[RTX] Scene warp: %s (entrance 0x%03X)", sceneName, entranceId);
    }

    // Update the warp input system. Called every frame from RTX_UpdateSceneParams.
    void Update(void* playPtr, int currentScene) {
        // --- F7 = previous RTX scene, F8 = next RTX scene ---
        bool f7Down = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
        if (f7Down && !wasF7Pressed) {
            int idx = FindRTXSceneIndex(currentScene);
            if (idx < 0) idx = 0; // not on an RTX scene, go to first
            else idx = (idx - 1 + NUM_RTX_SCENES) % NUM_RTX_SCENES;
            DoWarp(playPtr, s_rtxScenes[idx].entranceId, s_rtxScenes[idx].name);
        }
        wasF7Pressed = f7Down;

        bool f8Down = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
        if (f8Down && !wasF8Pressed) {
            int idx = FindRTXSceneIndex(currentScene);
            if (idx < 0) idx = NUM_RTX_SCENES - 1; // not on an RTX scene, go to last
            else idx = (idx + 1) % NUM_RTX_SCENES;
            DoWarp(playPtr, s_rtxScenes[idx].entranceId, s_rtxScenes[idx].name);
        }
        wasF8Pressed = f8Down;

        // --- F6 = toggle scene number input mode ---
        bool f6Down = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
        if (f6Down && !wasF6Pressed) {
            active = !active;
            if (active) {
                inputLen = 0;
                inputBuffer[0] = '\0';
                printf("[RTX] === SCENE WARP (F6) ===\n");
                printf("[RTX] Type scene number (decimal), then Enter. Escape to cancel.\n");
                printf("[RTX] F7/F8 = prev/next RTX scene. Current scene: 0x%02X (%d) = %s\n",
                       currentScene, currentScene, GetSceneName(currentScene));
                printf("[RTX] RTX-enabled scenes:\n");
                for (int i = 0; i < NUM_RTX_SCENES; i++) {
                    printf("[RTX]   %3d (0x%02X) = %s%s\n",
                           s_rtxScenes[i].sceneId, s_rtxScenes[i].sceneId,
                           s_rtxScenes[i].name,
                           (s_rtxScenes[i].sceneId == currentScene) ? " <-- YOU ARE HERE" : "");
                }
                printf("[RTX] Enter scene number> ");
            } else {
                printf("\n[RTX] Scene warp cancelled.\n");
            }
        }
        wasF6Pressed = f6Down;

        if (!active) return;

        // --- Digit input (0-9) ---
        for (int d = 0; d < 10; d++) {
            bool keyDown = (GetAsyncKeyState('0' + d) & 0x8000) != 0;
            if (keyDown && !prevDigitState[d] && inputLen < 3) {
                inputBuffer[inputLen++] = '0' + d;
                inputBuffer[inputLen] = '\0';
                int val = atoi(inputBuffer);
                printf("%c  (=%d, %s)\n[RTX] Enter scene number> %s",
                       '0' + d, val, GetSceneName(val), inputBuffer);
            }
            prevDigitState[d] = keyDown;
        }

        // --- Backspace ---
        bool bkspDown = (GetAsyncKeyState(VK_BACK) & 0x8000) != 0;
        static bool s_wasBksp = false;
        if (bkspDown && !s_wasBksp && inputLen > 0) {
            inputLen--;
            inputBuffer[inputLen] = '\0';
            printf("\r[RTX] Enter scene number> %s   ", inputBuffer);
        }
        s_wasBksp = bkspDown;

        // --- Enter = confirm warp ---
        bool enterDown = (GetAsyncKeyState(VK_RETURN) & 0x8000) != 0;
        if (enterDown && !wasEnterPressed && inputLen > 0) {
            int sceneId = atoi(inputBuffer);
            // Look up entrance ID
            int idx = FindRTXSceneIndex(sceneId);
            if (idx >= 0) {
                printf("\n");
                DoWarp(playPtr, s_rtxScenes[idx].entranceId, s_rtxScenes[idx].name);
            } else {
                // For non-RTX scenes, try to construct a basic entrance.
                // The entrance table uses sceneId-based offsets but it's not a simple formula.
                // Just warn the user.
                printf("\n[RTX] WARNING: Scene %d (0x%02X) is NOT in the RTX scene table.\n", sceneId, sceneId);
                printf("[RTX] Warp may crash or show no RTX rendering. Use F7/F8 for safe warping.\n");
            }
            active = false;
        }
        wasEnterPressed = enterDown;

        // --- Escape = cancel ---
        bool escDown = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
        if (escDown && !wasEscPressed) {
            printf("\n[RTX] Scene warp cancelled.\n");
            active = false;
        }
        wasEscPressed = escDown;
    }
};

static SceneWarpInput s_sceneWarp;
#endif // _WIN32

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
    // Try getting HWND from the DX12 bridge (set by gfx_dxgi.cpp)
    HWND hwnd = GfxDX12Bridge_GetHWND();
    if (hwnd) return hwnd;
    // Fallback: Win32 API
    hwnd = GetActiveWindow();
    if (!hwnd) hwnd = GetForegroundWindow();
    return hwnd;
#else
    return nullptr;
#endif
}

static void GetWindowDims(HWND hwnd, unsigned int& width, unsigned int& height) {
#ifdef _WIN32
    width = 0;
    height = 0;
    if (hwnd) {
        RECT rect;
        if (GetClientRect(hwnd, &rect)) {
            width = rect.right - rect.left;
            height = rect.bottom - rect.top;
        }
    }
    if (width == 0 || height == 0) {
        width = 1280;
        height = 960;
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
    RTX_DIAG("RTX Hook: TryLazyInitialize entry (initSucceeded=%d, initAttempted=%d)",
             (int)s_rtxInitSucceeded, (int)s_rtxInitAttempted);

    if (s_rtxInitSucceeded) return;
    if (s_rtxInitAttempted) return;

#ifdef _WIN32
    s_rtxInitAttempted = true;
    s_rtxProbeAttempted = true;

    auto* renderer = RTX::RTXRenderer::Instance();
    if (!renderer) {
        SPDLOG_ERROR("[RTX] No RTXRenderer instance - RTX disabled");
        return;
    }

    // Check if the DX12 bridge is active (RTX_EarlyInit succeeded and gfx_dxgi
    // created the swap chain with our DX12 command queue)
    if (RTX_IsBridgeActive()) {
        printf("[RTX] TryLazyInitialize: Bridge is active, completing initialization...\n");
        RTX_DIAG("RTX Hook: Bridge is active, completing initialization from bridge");
        
        s_rtxProbeSucceeded = true;
        s_rtxInitSucceeded = renderer->CompleteInitializationFromBridge();
        
        if (s_rtxInitSucceeded) {
            g_rtxPresentBlocked = true;
            gfx_dxgi_set_rtx_block_callback(RTXBlockQueryCallback);
            
            printf("[RTX] ============================================================\n");
            printf("[RTX] RTX BRIDGE INIT SUCCEEDED - DX12 OWNS THE SWAP CHAIN\n");
            printf("[RTX] ============================================================\n");
            
            FILE* f = fopen("rtx_bridge_debug.log", "a");
            if (f) {
                fprintf(f, "=== TryLazyInitialize: Bridge init SUCCESS ===\n");
                auto* dev = renderer->GetDevice();
                if (dev) {
                    fprintf(f, "  device=%p cmdQueue=%p swapChain=%p %ux%u\n",
                            (void*)dev->GetDevice(), (void*)dev->GetCommandQueue(),
                            (void*)dev->GetSwapChain(), dev->GetWidth(), dev->GetHeight());
                }
                fflush(f);
                fclose(f);
            }
        } else {
            printf("[RTX] TryLazyInitialize: Bridge init FAILED\n");
            RTX_DIAG("RTX Hook: Bridge init FAILED");
        }
    } else {
        // Bridge not active - probe may not have succeeded
        printf("[RTX] TryLazyInitialize: Bridge NOT active, trying probe...\n");
        
        s_rtxProbeSucceeded = renderer->ProbeRTXSupport();
        if (!s_rtxProbeSucceeded) {
            printf("[RTX] TryLazyInitialize: DX12/DXR probe FAILED\n");
            return;
        }
        
        printf("[RTX] TryLazyInitialize: DXR supported but bridge not active (swap chain is DX11-owned)\n");
        printf("[RTX] RTX_EarlyInit() was not called before InitWindow. Cannot switch to DX12.\n");
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

void* RTX_GetGameHWND(void) {
    // Get the game window HWND from the DXGI backend.
    // The DXGI backend stores h_wnd as a private member, accessed via GetWindowHandle().
    // This function wraps RTX_GetWindowHWND() from OTRGlobals.cpp, which reads
    // the HWND from the GfxWindowBackendDXGI instance.
    //
    // IMPLEMENTATION NOTE for Worker 2 (DX12Device.cpp):
    //   The HWND is originally created in GfxWindowBackendDXGI::Init() using
    //   CreateWindowW(WINCLASS_NAME, ...) and stored as 'h_wnd' (private member).
    //   It is exposed via GfxWindowBackendDXGI::GetWindowHandle().
    //   The OTRGlobals.cpp function RTX_GetWindowHWND() calls that method.
    //   Use RTX_GetGameHWND() to obtain the same HWND for DX12 swap chain creation.
    HWND hwnd = GetWindowHWND();  // Uses our internal helper (line ~402 in this file)
    return (void*)hwnd;
}

void RTX_BlockDX11(int block) {
    // Set or clear the global flag that blocks ALL DX11 rendering and Present calls.
    // This flag is checked by:
    //   - gfx_dxgi.cpp: SwapBuffersBegin, SwapBuffersEnd, IsFrameReady, ApplyMaxFrameLatency
    //   - gfx_direct3d11.cpp: DrawTriangles, ClearFramebuffer, StartFrame, EndFrame,
    //     StartDrawToFramebuffer, UpdateFramebufferParameters, SetViewport, SetScissor,
    //     UploadTexture, ResolveMSAAColorBuffer, CopyFramebuffer, ReadFramebufferToCPU,
    //     GetPixelDepth
    bool newValue = (block != 0);
    bool oldValue = g_rtxPresentBlocked;
    g_rtxPresentBlocked = newValue;
    if (newValue != oldValue) {
        printf("[RTX] RTX_BlockDX11: g_rtxPresentBlocked changed %d -> %d\n",
               (int)oldValue, (int)newValue);
    }
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

    // N64/OoT guLookAtF/guPerspectiveF store matrices in column-major order:
    //   mf[row][col] has axis vectors in columns (e.g. mf[0]={R.x, U.x, L.x, 0}).
    // Our HLSL declares 'row_major float4x4', so we must transpose on copy
    // so that HLSL row 0 = {R.x, R.y, R.z, -dot(E,R)} instead of {R.x, U.x, L.x, 0}.
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            viewMatrix[i * 4 + j] = viewMtxF.mf[j][i];  // transpose: swap i,j
            projMatrix[i * 4 + j] = projMtxF.mf[j][i];  // transpose: swap i,j
        }
    }

    // Camera position from play->view.eye
    cameraPos[0] = play->view.eye.x;
    cameraPos[1] = play->view.eye.y;
    cameraPos[2] = play->view.eye.z;

    // --- Free-fly camera override (F5 to toggle) ---
#ifdef _WIN32
    {
        // Estimate dt from frame timing (~20fps game tick = 50ms, but RTX runs at display rate)
        static LARGE_INTEGER s_lastTime = {};
        static LARGE_INTEGER s_freq = {};
        if (s_freq.QuadPart == 0) QueryPerformanceFrequency(&s_freq);
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        float dt = (s_lastTime.QuadPart > 0)
            ? (float)(now.QuadPart - s_lastTime.QuadPart) / (float)s_freq.QuadPart
            : 0.016f;
        s_lastTime = now;
        if (dt > 0.1f) dt = 0.1f;  // Clamp to avoid huge jumps

        s_flyCamera.Update(dt, cameraPos, viewMatrix);
    }

    // --- Scene warp input (F6=type number, F7=prev, F8=next) ---
    s_sceneWarp.Update(playPtr, play->sceneNum);
#endif

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

    // Log fog and lighting params for first few frames.
    // NOTE: sunDir1/sunColor1 logged here are the GAME-PROVIDED values from OoT's LightContext.
    // These are IGNORED by RTXRenderer::UpdateSceneParams() which hardcodes pure white sun.
    // The game's light colors are typically non-white (e.g., warm tinted) — this is expected.
    {
        static uint32_t s_fogLogCount = 0;
        s_fogLogCount++;
        if (s_fogLogCount <= 5 || (s_fogLogCount % 300) == 0) {
            RTX_DIAG("Scene params #%u: fogNear=%.1f fogFar=%.1f fogColor=(%.3f,%.3f,%.3f) ambient=(%.3f,%.3f,%.3f) cam=(%.1f,%.1f,%.1f)",
                     s_fogLogCount, fogNear, fogFar,
                     fogColor[0], fogColor[1], fogColor[2],
                     ambientColor[0], ambientColor[1], ambientColor[2],
                     cameraPos[0], cameraPos[1], cameraPos[2]);
            RTX_DIAG("  GAME-PROVIDED (IGNORED by RTX): sunDir1=(%.3f,%.3f,%.3f) sunColor1=(%.3f,%.3f,%.3f) sunDir2=(%.3f,%.3f,%.3f) sunColor2=(%.3f,%.3f,%.3f)",
                     sunDir1[0], sunDir1[1], sunDir1[2], sunColor1[0], sunColor1[1], sunColor1[2],
                     sunDir2[0], sunDir2[1], sunDir2[2], sunColor2[0], sunColor2[1], sunColor2[2]);
            RTX_DIAG("  RTX uses PURE WHITE sun from SceneConfig (no warm bias). Ambient is COOL BLUE.");
        }
    }

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

        // CYCLE 12: File-based logging of RTX dispatch from the hook side
        if (s_frameCounter == 1 || s_frameCounter == 10 || s_frameCounter == 60 ||
            s_frameCounter == 120 || s_frameCounter == 300) {
            FILE* f = fopen("rtx_hooks_dispatch.log", (s_frameCounter == 1) ? "w" : "a");
            if (f) {
                fprintf(f, "RTX_DispatchAndPresent frame #%u: renderer=%p, rendererActive=%s, scene=%d, g_rtxPresentBlocked=%d\n",
                        s_frameCounter,
                        (void*)renderer,
                        RTX::RTXRenderer::IsActive() ? "yes" : "no",
                        renderer->IsRTXSceneActive() ? renderer->GetCurrentScene() : -1,
                        (int)g_rtxPresentBlocked);
                fflush(f);
                fclose(f);
            }
        }

        // Process any deferred rooms that were queued before initialization.
        // This catches the edge case where rooms loaded between scene load
        // and the first frame render.
        ProcessDeferredRooms();

        renderer->DispatchAndPresent();

        // CYCLE 12: Log AFTER dispatch to confirm it returned successfully
        if (s_frameCounter == 1 || s_frameCounter == 60) {
            FILE* f = fopen("rtx_hooks_dispatch.log", "a");
            if (f) {
                fprintf(f, "  -> DispatchAndPresent RETURNED successfully at frame #%u\n", s_frameCounter);
                fflush(f);
                fclose(f);
            }
        }
    } else {
        if (s_frameCounter <= 3) {
            RTX_DIAG("RTX Hook: Frame begin but no RTXRenderer instance!");
        }
        // CYCLE 12: Log the case where renderer is null
        if (s_frameCounter == 1) {
            FILE* f = fopen("rtx_hooks_dispatch.log", "w");
            if (f) {
                fprintf(f, "RTX_DispatchAndPresent frame #%u: NO RENDERER INSTANCE!\n", s_frameCounter);
                fflush(f);
                fclose(f);
            }
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

    // CYCLE 12: File-based log of scene load
    {
        FILE* f = fopen("rtx_init_debug.log", "a");
        if (f) {
            fprintf(f, "\nRTX_OnSceneLoaded: sceneNum=0x%02X (%d), isRTXScene=%s, CVar=%s\n",
                    sceneNum, sceneNum,
                    RTX::IsRTXScene(static_cast<uint16_t>(sceneNum)) ? "YES" : "NO",
                    IsRTXEnabledByCVar() ? "ON" : "OFF");
            fflush(f);
            fclose(f);
        }
    }
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

#ifdef _WIN32
    // Reset the free-fly camera so it re-initializes from the new scene's
    // camera position instead of keeping stale position/orientation from
    // the previous scene. Without this, flying into a new scene starts
    // at the old scene's camera location (potentially underground or in void).
    s_flyCamera.active = false;
    s_flyCamera.initialized = false;
    s_flyCamera.hasLastMouse = false;
    RTX_DIAG("[DIAG] RTX_OnSceneUnload: reset s_flyCamera (active=false, initialized=false)");
#endif

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
    // Unblock DX11 present so it can resume if the game continues without RTX
    g_rtxPresentBlocked = false;
    s_rtxProbeAttempted = false;
    s_rtxProbeSucceeded = false;
    s_rtxInitAttempted = false;
    s_rtxInitSucceeded = false;
    s_deferredRooms.clear();
    RTX_DIAG("RTX Hook: RTX_Shutdown complete - all state reset, g_rtxPresentBlocked=false");
}

void RTX_InterceptTexture(const void* timgAddr, const unsigned char* rgbaData,
                          unsigned int width, unsigned int height, unsigned int format) {
    static uint32_t s_interceptPrintCount = 0;
    s_interceptPrintCount++;
    if (s_interceptPrintCount <= 10 || (s_interceptPrintCount % 100) == 0) {
        printf("[RTX] RTX_InterceptTexture called (%ux%u, fmt=%u, addr=%p, call #%u)\n",
               width, height, format, timgAddr, s_interceptPrintCount);
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

    // Clamp unreasonable dimensions
    if (width > 4096 || height > 4096) {
        static uint32_t s_oversizeCount = 0;
        s_oversizeCount++;
        if (s_oversizeCount <= 5) {
            RTX_DIAG("RTX Hook: RTX_InterceptTexture skipped - oversized %ux%u (skip #%u)",
                     width, height, s_oversizeCount);
        }
        return;
    }

    // Verify the RGBA data isn't all-white or all-zero (common data corruption indicators)
    {
        static uint32_t s_dataCheckCount = 0;
        s_dataCheckCount++;
        if (s_dataCheckCount <= 30) {
            uint32_t pixelCount = width * height;
            uint32_t sampleStride = (pixelCount > 64) ? (pixelCount / 16) : 1;
            uint32_t allWhite = 0, allBlack = 0, sampled = 0;
            for (uint32_t i = 0; i < pixelCount && sampled < 16; i += sampleStride, sampled++) {
                uint8_t r = rgbaData[i * 4 + 0];
                uint8_t g = rgbaData[i * 4 + 1];
                uint8_t b = rgbaData[i * 4 + 2];
                if (r == 255 && g == 255 && b == 255) allWhite++;
                if (r == 0 && g == 0 && b == 0) allBlack++;
            }
            RTX_DIAG("RTX Hook: texture data check #%u: %ux%u, %u sampled, %u white, %u black, pixel[0]=(%u,%u,%u,%u)",
                     s_dataCheckCount, width, height, sampled, allWhite, allBlack,
                     rgbaData[0], rgbaData[1], rgbaData[2], rgbaData[3]);
        }
    }

    static uint32_t s_texInterceptCount = 0;
    static uint32_t s_texUploadSuccess = 0;
    static uint32_t s_texUploadFail = 0;
    s_texInterceptCount++;
    if (s_texInterceptCount <= 20 || (s_texInterceptCount % 100) == 0) {
        RTX_DIAG("RTX Hook: RTX_InterceptTexture #%u, addr=%p, %ux%u, fmt=%u, totalUploadOK=%u, totalFail=%u",
                 s_texInterceptCount, timgAddr, width, height, format, s_texUploadSuccess, s_texUploadFail);
    }

    constexpr uint64_t FNV_OFFSET = 0xcbf29ce484222325ULL;
    constexpr uint64_t FNV_PRIME  = 0x100000001b3ULL;

    // Compute a hash from the timg address (pointer value) and texture dimensions.
    // This gives a stable key for cache lookup as long as the same ROM address
    // maps to the same texture data across frames.
    uint64_t hash;
    uintptr_t addr = reinterpret_cast<uintptr_t>(timgAddr);
    {
        // FNV-1a seeded with the address, mixed with dimensions and format
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

    // Check if the TextureManager singleton is initialized before uploading
    auto& texMgr = RTX::TextureManager::GetInstance();
    if (!texMgr.GetSRVHeap()) {
        static uint32_t s_noHeapCount = 0;
        s_noHeapCount++;
        if (s_noHeapCount <= 5) {
            RTX_DIAG("RTX Hook: RTX_InterceptTexture skipped - TextureManager SRV heap not ready (skip #%u)", s_noHeapCount);
        }
        return;
    }

    // Upload the decoded RGBA data to the GPU via the TextureManager singleton.
    RTX::RTXTextureHandle handle = texMgr.GetOrUploadTexture(rgbaData, width, height, hash);

    if (handle.srvIndex > 0) {
        s_texUploadSuccess++;
        // Register address-only hash as alias so ResolveMaterialTextures can find it
        uint64_t addrOnlyHash = FNV_OFFSET;
        for (size_t i = 0; i < sizeof(addr); i++) {
            addrOnlyHash ^= static_cast<uint64_t>((addr >> (i * 8)) & 0xFF);
            addrOnlyHash *= FNV_PRIME;
        }
        // Only register the alias if it differs from the full hash
        if (addrOnlyHash != hash) {
            texMgr.RegisterHashAlias(addrOnlyHash, handle.srvIndex);
        }
        // Also register the raw address value as an alias (for the fallback path
        // in ResolveMaterialTextures that tries the raw address as a hash key)
        texMgr.RegisterHashAlias(static_cast<uint64_t>(addr), handle.srvIndex);

        // Additionally, check if the timgAddr looks like a string pointer (OTR path).
        // If so, register a hash of the string itself as an alias. This bridges
        // the gap between RTX_InterceptTexture (which receives the data pointer)
        // and ResolveMaterialTextures (which has the OTR path string pointer).
        if (addr > 0x10000) {
            bool isOTRPath = false;
            const char* pathStr = nullptr;
#ifdef _WIN32
            __try {
                const char* s = (const char*)addr;
                isOTRPath = (s[0] == '_' && s[1] == '_');
                if (isOTRPath) pathStr = s;
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                isOTRPath = false;
            }
#endif
            if (isOTRPath && pathStr) {
                // Compute FNV-1a hash of the OTR path string content
                uint64_t pathHash = FNV_OFFSET;
                for (const char* p = pathStr; *p; p++) {
                    pathHash ^= (uint64_t)(uint8_t)*p;
                    pathHash *= FNV_PRIME;
                }
                texMgr.RegisterHashAlias(pathHash, handle.srvIndex);

                // Also register hash of the path WITHOUT the __OTR__ prefix.
                // ResolveMaterialTextures may store paths with or without the prefix.
                if (pathStr[0] == '_' && pathStr[1] == '_' && pathStr[2] == 'O'
                    && pathStr[3] == 'T' && pathStr[4] == 'R' && pathStr[5] == '_'
                    && pathStr[6] == '_') {
                    const char* strippedPath = pathStr + 7;
                    uint64_t strippedHash = FNV_OFFSET;
                    for (const char* p = strippedPath; *p; p++) {
                        strippedHash ^= (uint64_t)(uint8_t)*p;
                        strippedHash *= FNV_PRIME;
                    }
                    texMgr.RegisterHashAlias(strippedHash, handle.srvIndex);
                }

                if (s_texUploadSuccess <= 20) {
                    RTX_DIAG("RTX Hook: registered OTR path alias for '%s' -> SRV %u (pathHash=0x%llX)",
                             pathStr, handle.srvIndex, (unsigned long long)pathHash);
                }
            }
        }

        if (s_texUploadSuccess <= 20 || (s_texUploadSuccess % 50) == 0) {
            RTX_DIAG("RTX Hook: texture upload SUCCESS #%u: addr=%p, %ux%u -> SRV %u (hash=0x%llX, addrHash=0x%llX)",
                     s_texUploadSuccess, timgAddr, width, height, handle.srvIndex,
                     (unsigned long long)hash, (unsigned long long)addrOnlyHash);
        }
    } else {
        s_texUploadFail++;
        if (s_texUploadFail <= 20) {
            RTX_DIAG("RTX Hook: texture upload FAILED #%u: addr=%p, %ux%u, fmt=%u (srvHeap=%p, nextSRV=%u)",
                     s_texUploadFail, timgAddr, width, height, format,
                     (void*)texMgr.GetSRVHeap(), texMgr.GetNextSRVIndex());
        }
    }
}

void RTX_UploadUIOverlay(const unsigned char* pixelData, unsigned int width, unsigned int height) {
    if (!RTX::RTXRenderer::IsActive()) return;

    auto* renderer = RTX::RTXRenderer::Instance();
    if (!renderer) return;

    renderer->UploadUIOverlay(pixelData, width, height);

    static uint32_t s_uiUploadCount = 0;
    s_uiUploadCount++;
    if (s_uiUploadCount <= 5 || (s_uiUploadCount % 300) == 0) {
        RTX_DIAG("RTX Hook: RTX_UploadUIOverlay %ux%u (data=%p, frame #%u)",
                 width, height, (const void*)pixelData, s_uiUploadCount);
    }
}

int RTX_GetDiagnosticState(char* buf, int bufSize) {
    if (!buf || bufSize <= 0) return 0;
    buf[0] = '\0';

    auto* renderer = RTX::RTXRenderer::Instance();
    bool rendererExists = (renderer != nullptr);
    bool isActive = RTX::RTXRenderer::IsActive();
    bool hasDevice = rendererExists && renderer->GetDevice() != nullptr;
    bool deviceInit = hasDevice && renderer->GetDevice()->IsInitialized();
    bool hasPipeline = rendererExists && renderer->GetPipeline() != nullptr;
    bool hasPSO = hasPipeline && renderer->GetPipeline()->GetStateObject() != nullptr;
    bool hasSwapChain = hasDevice && renderer->GetDevice()->GetSwapChain() != nullptr;
    bool sceneLoaded = rendererExists && renderer->IsSceneLoaded();
    int currentScene = rendererExists ? renderer->GetCurrentScene() : -1;

    int written = snprintf(buf, bufSize,
        "RTX_DIAG: renderer=%s active=%s device=%s init=%s pipeline=%s PSO=%s swapChain=%s scene=%d loaded=%s",
        rendererExists ? "YES" : "NO",
        isActive ? "YES" : "NO",
        hasDevice ? "YES" : "NO",
        deviceInit ? "YES" : "NO",
        hasPipeline ? "YES" : "NO",
        hasPSO ? "YES" : "NO",
        hasSwapChain ? "YES" : "NO",
        currentScene,
        sceneLoaded ? "YES" : "NO");
    return (written > 0 && written < bufSize) ? written : 0;
}

int RTX_DiagnosticClearBackBuffer(float r, float g, float b, float a) {
    auto* renderer = RTX::RTXRenderer::Instance();
    if (!renderer) return 0;
    auto* device = renderer->GetDevice();
    if (!device || !device->IsInitialized()) return 0;
    auto* swapChain = device->GetSwapChain();
    if (!swapChain) return 0;

    // This function performs a standalone BeginFrame/Clear/Present cycle
    // that bypasses ALL shaders. It clears the DX12 back buffer to the
    // specified color and presents it. If the screen color changes to
    // this color, it proves the DX12 swap chain is active and displaying.
    device->BeginFrame();
    auto* cmdList = device->GetCommandList();
    if (!cmdList) {
        device->Present();
        return 0;
    }

    ID3D12Resource* bb = device->GetCurrentBackBuffer();
    if (!bb) {
        cmdList->Close();
        ID3D12CommandList* ppEmpty[] = { cmdList };
        auto* q = device->GetCommandQueue();
        if (q) q->ExecuteCommandLists(1, ppEmpty);
        device->Present();
        return 0;
    }

    // Transition to render target
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = bb;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmdList->ResourceBarrier(1, &barrier);

    // Clear to the specified color
    float clearColor[4] = { r, g, b, a };
    cmdList->ClearRenderTargetView(device->GetCurrentRTVHandle(), clearColor, 0, nullptr);

    // Transition to present
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    cmdList->ResourceBarrier(1, &barrier);

    cmdList->Close();
    ID3D12CommandList* ppCmdLists[] = { cmdList };
    auto* cmdQueue = device->GetCommandQueue();
    if (cmdQueue) {
        cmdQueue->ExecuteCommandLists(1, ppCmdLists);
    }
    device->Present();

    static uint32_t s_diagClearCount = 0;
    s_diagClearCount++;
    if (s_diagClearCount <= 5 || (s_diagClearCount % 300) == 0) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "[RTX] DiagnosticClearBackBuffer: cleared to (%.2f,%.2f,%.2f,%.2f) — frame #%u\n",
                 r, g, b, a, s_diagClearCount);
        OutputDebugStringA(msg);
        printf("%s", msg);
    }
    return 1;
}

} // extern "C"

#endif // ENABLE_DX12_RTX
