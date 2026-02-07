#ifdef ENABLE_DX12_RTX

#include "RTXHooks.h"
#include "RTXRenderer.h"
#include <spdlog/spdlog.h>

// Include game headers for PlayState access
extern "C" {
#include "global.h"
#include "z64.h"
}

// Scene ID for Kokiri Forest
#define SCENE_KOKIRI_FOREST_ID 0x55

extern "C" {

int RTX_IsActive(void) {
    return RTX::RTXRenderer::IsActive() ? 1 : 0;
}

int RTX_IsKokiriForest(void* playPtr) {
    PlayState* play = (PlayState*)playPtr;
    return (play->sceneNum == SCENE_KOKIRI_FOREST_ID) ? 1 : 0;
}

void RTX_UpdateSceneParams(void* playPtr) {
    PlayState* play = (PlayState*)playPtr;
    if (!play) return;

    auto* renderer = RTX::RTXRenderer::Instance();
    if (!renderer) return;

    // Extract view/projection matrices from play->view
    // The game uses column-major MtxF (4x4 float matrix)
    float viewMatrix[16];
    float projMatrix[16];
    float cameraPos[3];

    // Copy view matrix from play->view.viewing (MtxF)
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            viewMatrix[i * 4 + j] = play->view.viewing.mf[i][j];
            projMatrix[i * 4 + j] = play->view.projection.mf[i][j];
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
    sunDir1[0] = play->envCtx.lightSettings.diffuseDir1[0] / 127.0f;
    sunDir1[1] = play->envCtx.lightSettings.diffuseDir1[1] / 127.0f;
    sunDir1[2] = play->envCtx.lightSettings.diffuseDir1[2] / 127.0f;

    float sunColor1[3];
    sunColor1[0] = play->envCtx.lightSettings.diffuseColor1[0] / 255.0f;
    sunColor1[1] = play->envCtx.lightSettings.diffuseColor1[1] / 255.0f;
    sunColor1[2] = play->envCtx.lightSettings.diffuseColor1[2] / 255.0f;

    // Directional light 2
    float sunDir2[3];
    sunDir2[0] = play->envCtx.lightSettings.diffuseDir2[0] / 127.0f;
    sunDir2[1] = play->envCtx.lightSettings.diffuseDir2[1] / 127.0f;
    sunDir2[2] = play->envCtx.lightSettings.diffuseDir2[2] / 127.0f;

    float sunColor2[3];
    sunColor2[0] = play->envCtx.lightSettings.diffuseColor2[0] / 255.0f;
    sunColor2[1] = play->envCtx.lightSettings.diffuseColor2[1] / 255.0f;
    sunColor2[2] = play->envCtx.lightSettings.diffuseColor2[2] / 255.0f;

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
        renderer->OnRoomLoaded(roomNum);
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
}

} // extern "C"

#endif // ENABLE_DX12_RTX
