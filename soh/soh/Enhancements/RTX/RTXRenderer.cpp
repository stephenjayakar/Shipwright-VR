#ifdef ENABLE_DX12_RTX

#include "RTXRenderer.h"
#include <spdlog/spdlog.h>
#include <cstring>
#include <cmath>

namespace RTX {

RTXRenderer* RTXRenderer::s_instance = nullptr;

RTXRenderer* RTXRenderer::Instance() {
    if (!s_instance) {
        s_instance = new RTXRenderer();
    }
    return s_instance;
}

bool RTXRenderer::IsActive() {
    return s_instance && s_instance->m_device && s_instance->m_device->IsInitialized() && s_instance->m_sceneLoaded;
}

RTXRenderer::RTXRenderer()
    : m_device(std::make_unique<DX12Device>())
    , m_pipeline(std::make_unique<DXRPipeline>()) {
}

RTXRenderer::~RTXRenderer() {
    Shutdown();
    if (s_instance == this) {
        s_instance = nullptr;
    }
}

bool RTXRenderer::Initialize(HWND hwnd, uint32_t width, uint32_t height) {
    SPDLOG_INFO("[RTX] Initializing RTX renderer ({}x{})", width, height);

    if (!m_device->Initialize(hwnd, width, height)) {
        SPDLOG_ERROR("[RTX] Failed to initialize DX12 device");
        return false;
    }

    if (!m_device->SupportsRaytracing()) {
        SPDLOG_ERROR("[RTX] Raytracing not supported - RTX renderer disabled");
        m_device->Shutdown();
        return false;
    }

    if (!m_pipeline->Initialize(m_device.get(), width, height)) {
        SPDLOG_ERROR("[RTX] Failed to initialize DXR pipeline");
        m_device->Shutdown();
        return false;
    }

    SPDLOG_INFO("[RTX] RTX renderer initialized successfully");
    return true;
}

void RTXRenderer::Shutdown() {
    if (m_pipeline) m_pipeline->Shutdown();
    if (m_device) m_device->Shutdown();
    m_sceneLoaded = false;
    m_currentScene = -1;
    SPDLOG_INFO("[RTX] RTX renderer shut down");
}

void RTXRenderer::OnSceneLoaded(int sceneNum) {
    m_currentScene = sceneNum;
    m_sceneLoaded = true;
    m_accumulationFrameCount = 0;
    memset(m_prevViewMatrix, 0, sizeof(m_prevViewMatrix));

    SPDLOG_INFO("[RTX] Scene loaded: 0x{:02X}", sceneNum);

    // TODO (Phase 2-3): Extract geometry from display lists and build BLAS
    // This will be triggered per-room via OnRoomLoaded()
}

void RTXRenderer::OnSceneUnload() {
    m_sceneLoaded = false;
    m_currentScene = -1;
    m_accumulationFrameCount = 0;

    // TODO (Phase 3): Release BLAS/TLAS
    // TODO (Phase 6): Release textures

    SPDLOG_INFO("[RTX] Scene unloaded");
}

void RTXRenderer::OnRoomLoaded(int roomNum) {
    SPDLOG_INFO("[RTX] Room {} loaded", roomNum);

    // TODO (Phase 2): Extract geometry from room display lists
    // TODO (Phase 3): Build BLAS for this room
    // TODO (Phase 6): Load textures referenced by this room
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
    // Detect camera movement for accumulation reset
    m_cameraMoved = (memcmp(viewMatrix, m_prevViewMatrix, sizeof(float) * 16) != 0);
    if (m_cameraMoved) {
        m_accumulationFrameCount = 0;
    } else {
        m_accumulationFrameCount++;
    }
    memcpy(m_prevViewMatrix, viewMatrix, sizeof(float) * 16);

    // Compute inverse matrices for ray generation
    InvertMatrix4x4(viewMatrix, m_sceneConstants.viewInverse);
    InvertMatrix4x4(projMatrix, m_sceneConstants.projInverse);

    // Camera
    m_sceneConstants.cameraPos[0] = cameraPos[0];
    m_sceneConstants.cameraPos[1] = cameraPos[1];
    m_sceneConstants.cameraPos[2] = cameraPos[2];
    m_sceneConstants.frameCount = m_accumulationFrameCount;

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

    // Time and per-frame values
    m_sceneConstants.time = static_cast<float>(gameplayFrames);
    m_sceneConstants.dekuTreeAlpha = vegetationAlpha;
    m_sceneConstants.fogBlendAlpha = actualFogNear * 0.1f / 255.0f;
    m_sceneConstants.waterScrollOffset = roomUnk74 * 0.02f;

    // Upload to GPU
    m_pipeline->UpdateSceneConstants(m_sceneConstants);
}

void RTXRenderer::DispatchAndPresent() {
    if (!m_sceneLoaded || !m_device->IsInitialized()) return;

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

    // Dispatch rays
    m_pipeline->DispatchRays(cmdList, m_device->GetWidth(), m_device->GetHeight());

    // UAV barrier between ray tracing and denoise
    D3D12_RESOURCE_BARRIER uavBarrier = {};
    uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarrier.UAV.pResource = m_pipeline->GetOutputBuffer();
    cmdList->ResourceBarrier(1, &uavBarrier);

    // Denoise passes (3 A-trous wavelet passes)
    for (int pass = 0; pass < 3; pass++) {
        m_pipeline->DispatchDenoise(cmdList, m_device->GetWidth(), m_device->GetHeight(), pass);

        // UAV barrier between denoise passes
        cmdList->ResourceBarrier(1, &uavBarrier);
    }

    // Copy output buffer to back buffer
    // First transition output from UAV to copy source
    D3D12_RESOURCE_BARRIER copyBarriers[2] = {};
    copyBarriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    copyBarriers[0].Transition.pResource = m_pipeline->GetOutputBuffer();
    copyBarriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    copyBarriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    copyBarriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    copyBarriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    copyBarriers[1].Transition.pResource = m_device->GetCurrentBackBuffer();
    copyBarriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    copyBarriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    copyBarriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    cmdList->ResourceBarrier(2, copyBarriers);
    cmdList->CopyResource(m_device->GetCurrentBackBuffer(), m_pipeline->GetOutputBuffer());

    // Transition back buffer to present, output back to UAV
    D3D12_RESOURCE_BARRIER presentBarriers[2] = {};
    presentBarriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    presentBarriers[0].Transition.pResource = m_device->GetCurrentBackBuffer();
    presentBarriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    presentBarriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    presentBarriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    presentBarriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    presentBarriers[1].Transition.pResource = m_pipeline->GetOutputBuffer();
    presentBarriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    presentBarriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    presentBarriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    cmdList->ResourceBarrier(2, presentBarriers);

    // Close and execute command list, then present
    cmdList->Close();
    ID3D12CommandList* ppCommandLists[] = { cmdList };
    m_device->GetCommandQueue()->ExecuteCommandLists(1, ppCommandLists);

    m_device->Present();
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
