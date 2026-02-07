#ifdef ENABLE_DX12_RTX

#include "RTXRenderer.h"
#include "RTXSceneConfig.h"
#include "TextureManager.h"
#include <spdlog/spdlog.h>
#include <cstring>
#include <cmath>

extern "C" {
#include "global.h"
#include "z64.h"
}

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
    , m_pipeline(std::make_unique<DXRPipeline>())
    , m_geometryExtractor(std::make_unique<SceneGeometryExtractor>())
    , m_accelerationStructure(std::make_unique<AccelerationStructure>())
    , m_giSystem(std::make_unique<GISystem>())
    , m_textureManager(std::make_unique<TextureManager>()) {
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

    if (!m_textureManager->Initialize(m_device.get())) {
        SPDLOG_ERROR("[RTX] Failed to initialize texture manager");
        m_pipeline->Shutdown();
        m_device->Shutdown();
        return false;
    }

    if (!m_accelerationStructure->Initialize(m_device.get())) {
        SPDLOG_ERROR("[RTX] Failed to initialize acceleration structures");
        m_textureManager->Shutdown();
        m_pipeline->Shutdown();
        m_device->Shutdown();
        return false;
    }

    if (!m_giSystem->Initialize(m_device.get())) {
        SPDLOG_ERROR("[RTX] Failed to initialize GI system");
        m_accelerationStructure->Shutdown();
        m_textureManager->Shutdown();
        m_pipeline->Shutdown();
        m_device->Shutdown();
        return false;
    }

    // Also initialize the TextureManager singleton via the Device5 path so that
    // other subsystems (e.g., RTXHooks texture interception) can use
    // TextureManager::GetInstance() without needing a pointer to RTXRenderer.
    if (!TextureManager::GetInstance().Initialize(m_device->GetDevice(), 4096)) {
        SPDLOG_WARN("[RTX] TextureManager singleton (Device5 path) init failed — "
                     "hooks will fall back to instance-based texture manager");
        // Non-fatal: the owned m_textureManager is already initialized above.
    }

    SPDLOG_INFO("[RTX] RTX renderer initialized successfully");
    return true;
}

void RTXRenderer::Shutdown() {
    // Shut down the TextureManager singleton (Device5 path) first
    TextureManager::GetInstance().Shutdown();

    // Shutdown in reverse initialization order:
    // Init:     DX12Device -> DXRPipeline -> TextureManager -> AccelStruct -> GISystem
    // Shutdown: GISystem -> AccelStruct -> TextureManager -> DXRPipeline -> DX12Device
    if (m_giSystem) m_giSystem->Shutdown();
    if (m_accelerationStructure) m_accelerationStructure->Shutdown();
    if (m_textureManager) m_textureManager->Shutdown();
    if (m_pipeline) m_pipeline->Shutdown();
    if (m_device) m_device->Shutdown();
    m_roomGeometry.clear();
    m_sceneLoaded = false;
    m_currentScene = -1;
    SPDLOG_INFO("[RTX] RTX renderer shut down");
}

void RTXRenderer::OnSceneLoaded(int sceneNum) {
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

    // Also release from the owned instance (for completeness)
    if (m_textureManager) {
        m_textureManager->ReleaseAllTextures();
    }

    if (m_giSystem) {
        m_giSystem->ResetAccumulation();
    }

    SPDLOG_INFO("[RTX] Scene unloaded");
}

void RTXRenderer::OnRoomLoaded(void* playPtr, int roomNum) {
    SPDLOG_INFO("[RTX] Room {} loaded", roomNum);

    if (!playPtr || !m_geometryExtractor || !m_accelerationStructure) {
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
    RoomGeometry geometry = m_geometryExtractor->ExtractRoomGeometry(room, (uint32_t)roomNum);

    bool hasGeometry = !geometry.opaqueMesh.vertices.empty() || !geometry.alphaMesh.vertices.empty();
    if (!hasGeometry) {
        SPDLOG_WARN("[RTX] OnRoomLoaded: no geometry extracted for room {}", roomNum);
        return;
    }

    // Phase 6: Resolve raw texture address hashes in Material::textureIndex
    // to actual SRV descriptor heap indices from the TextureManager.
    // Textures not yet intercepted get SRV index 0 (default white).
    ResolveMaterialTextures(geometry);

    // Phase 3: Build BLAS for this room
    if (!m_accelerationStructure->BuildBLAS(geometry)) {
        SPDLOG_ERROR("[RTX] OnRoomLoaded: failed to build BLAS for room {}", roomNum);
        return;
    }

    // Store for reference
    m_roomGeometry.push_back(std::move(geometry));

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

void RTXRenderer::DispatchAndPresent() {
    if (!m_sceneLoaded || !m_device->IsInitialized()) return;

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
        m_accelerationStructure->RebuildTLAS(cmdList);
    }

    // Dispatch rays (pass TLAS address and texture table handle if available)
    D3D12_GPU_VIRTUAL_ADDRESS tlasAddr = 0;
    if (m_accelerationStructure && m_accelerationStructure->HasTLAS()) {
        tlasAddr = m_accelerationStructure->GetTLASAddress();
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

    m_pipeline->DispatchRays(cmdList, m_device->GetWidth(), m_device->GetHeight(), tlasAddr, texTableGPU);

    // UAV barrier between ray tracing and denoise
    D3D12_RESOURCE_BARRIER uavBarrier = {};
    uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarrier.UAV.pResource = m_pipeline->GetOutputBuffer();
    if (uavBarrier.UAV.pResource) {
        cmdList->ResourceBarrier(1, &uavBarrier);
    }

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

        // UAV barrier between denoise passes
        if (uavBarrier.UAV.pResource) {
            cmdList->ResourceBarrier(1, &uavBarrier);
        }
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

void RTXRenderer::ResolveMaterialTextures(RoomGeometry& geometry) {
    auto& texMgr = TextureManager::GetInstance();

    // Helper lambda: resolve all materials in an ExtractedMesh.
    // Each material has a corresponding entry in materialTextureAddrs[]
    // containing the full N64/OTR texture address (uintptr_t).
    // We compute the same FNV-1a hash that RTX_InterceptTexture uses
    // (FNV-1a over the pointer value bytes) and look up the SRV index
    // from the TextureManager cache.
    //
    // Note: RTX_InterceptTexture also mixes in width/height/format, but
    // at geometry extraction time we don't reliably know the texture
    // dimensions (they come from G_SETTILESIZE which we don't fully
    // track). So we use only the address-based hash here. If the exact
    // hash doesn't match, we fall back to SRV index 0 (default white).
    // The texture will appear correctly once RTX_InterceptTexture fires
    // for that address during the game's normal texture decode path.
    auto resolveMesh = [&](ExtractedMesh& mesh) {
        uint32_t resolved = 0;
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

            // Compute the same FNV-1a hash that RTX_InterceptTexture uses,
            // but without the width/height/format mix-in (we don't have those).
            // This is a partial match — if the TextureManager stores by this hash,
            // we'll find it. Otherwise, fall back to default white.
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
            } else {
                // Also try the raw address value as a fallback hash key
                srvIndex = texMgr.GetSRVIndexForHash(static_cast<uint64_t>(textureAddr));
                if (srvIndex > 0) {
                    mat.textureIndex = srvIndex;
                    resolved++;
                } else {
                    // Texture not yet in cache. Fall back to default white (SRV index 0).
                    // This is expected for textures that haven't been decoded yet.
                    mat.textureIndex = 0;
                    unresolved++;
                }
            }
        }

        if (resolved > 0 || unresolved > 0) {
            SPDLOG_DEBUG("[RTX] ResolveMaterialTextures: {} resolved, {} unresolved (fallback to white)",
                         resolved, unresolved);
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
