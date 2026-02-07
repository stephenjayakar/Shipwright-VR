#ifdef ENABLE_DX12_RTX

#include "RTXHooks.h"
#include "RTXRenderer.h"
#include "RTXSceneConfig.h"
#include "TextureManager.h"
#include <spdlog/spdlog.h>

#ifdef _WIN32
#include <Windows.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>
#endif

// Include game headers for PlayState access
extern "C" {
#include "global.h"
#include "z64.h"
}

// Track whether we've attempted RTX initialization
static bool s_rtxInitAttempted = false;
static bool s_rtxInitSucceeded = false;

// Attempt lazy RTX initialization using the game's SDL window.
// Called once on first RTX scene load. Retrieves the HWND from SDL
// and initializes the DX12 device, DXR pipeline, etc.
static void TryLazyInitialize() {
    if (s_rtxInitAttempted) return;
    s_rtxInitAttempted = true;

#ifdef _WIN32
    // Get the game's SDL window to extract the native HWND
    SDL_Window* sdlWindow = nullptr;

    // The game window is managed by libultraship; enumerate SDL windows
    // to find the main game window.
    sdlWindow = SDL_GetKeyboardFocus();
    if (!sdlWindow) {
        // Fallback: try to get any SDL window
        sdlWindow = SDL_GetGrabbedWindow();
    }

    if (!sdlWindow) {
        SPDLOG_WARN("[RTX] No SDL window found for RTX initialization - RTX disabled");
        return;
    }

    SDL_SysWMinfo wmInfo;
    SDL_VERSION(&wmInfo.version);
    if (!SDL_GetWindowWMInfo(sdlWindow, &wmInfo)) {
        SPDLOG_WARN("[RTX] Failed to get window WM info: {} - RTX disabled", SDL_GetError());
        return;
    }

    HWND hwnd = wmInfo.info.win.window;
    if (!hwnd) {
        SPDLOG_WARN("[RTX] Null HWND from SDL window - RTX disabled");
        return;
    }

    // Get window dimensions
    int width = 0, height = 0;
    SDL_GetWindowSize(sdlWindow, &width, &height);
    if (width <= 0 || height <= 0) {
        width = 1280;
        height = 960;
    }

    SPDLOG_INFO("[RTX] Attempting lazy RTX initialization ({}x{})", width, height);
    s_rtxInitSucceeded = RTX_Initialize(hwnd, (unsigned int)width, (unsigned int)height) != 0;

    if (s_rtxInitSucceeded) {
        SPDLOG_INFO("[RTX] Lazy initialization succeeded");
    } else {
        SPDLOG_WARN("[RTX] Lazy initialization failed - RTX disabled for this session");
    }
#else
    SPDLOG_INFO("[RTX] RTX is only supported on Windows - disabled");
#endif
}

extern "C" {

int RTX_IsActive(void) {
    return RTX::RTXRenderer::IsActive() ? 1 : 0;
}

int RTX_IsKokiriForest(void* playPtr) {
    PlayState* play = (PlayState*)playPtr;
    return RTX::IsRTXScene(static_cast<uint16_t>(play->sceneNum)) ? 1 : 0;
}

void RTX_UpdateSceneParams(void* playPtr) {
    PlayState* play = (PlayState*)playPtr;
    if (!play) return;

    auto* renderer = RTX::RTXRenderer::Instance();
    if (!renderer) return;

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
    auto* renderer = RTX::RTXRenderer::Instance();
    if (renderer) {
        renderer->DispatchAndPresent();
    }
}

void RTX_OnRoomLoaded(void* playPtr, int roomNum) {
    auto* renderer = RTX::RTXRenderer::Instance();
    if (renderer) {
        renderer->OnRoomLoaded(playPtr, roomNum);
    }
}

void RTX_OnSceneLoaded(int sceneNum) {
    // If this is an RTX-enabled scene and we haven't initialized yet, do lazy init
    if (RTX::IsRTXScene(static_cast<uint16_t>(sceneNum))) {
        TryLazyInitialize();
    }

    auto* renderer = RTX::RTXRenderer::Instance();
    if (renderer) {
        renderer->OnSceneLoaded(sceneNum);
    }
}

void RTX_OnSceneUnload(void) {
    auto* renderer = RTX::RTXRenderer::Instance();
    if (renderer) {
        renderer->OnSceneUnload();
    }
}

int RTX_Initialize(void* hwnd, unsigned int width, unsigned int height) {
    auto* renderer = RTX::RTXRenderer::Instance();
    if (!renderer) return 0;
    return renderer->Initialize((HWND)hwnd, width, height) ? 1 : 0;
}

void RTX_Shutdown(void) {
    auto* renderer = RTX::RTXRenderer::Instance();
    if (renderer) {
        renderer->Shutdown();
    }
    s_rtxInitAttempted = false;
    s_rtxInitSucceeded = false;
}

void RTX_InterceptTexture(const void* timgAddr, const unsigned char* rgbaData,
                          unsigned int width, unsigned int height, unsigned int format) {
    if (!RTX::RTXRenderer::IsActive()) return;
    if (!rgbaData || width == 0 || height == 0) return;

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
    RTX::TextureManager::GetInstance().GetOrUploadTexture(rgbaData, width, height, hash);
}

} // extern "C"

#endif // ENABLE_DX12_RTX
