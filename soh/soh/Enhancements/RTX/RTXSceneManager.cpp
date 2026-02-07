#ifdef ENABLE_DX12_RTX

#include "RTXSceneManager.h"
#include "DX12Device.h"
#include <spdlog/spdlog.h>
#include <cstring>

extern "C" {
#include "global.h"
#include "z64.h"
}

namespace RTX {

RTXSceneManager::RTXSceneManager()
    : m_geometryExtractor(std::make_unique<SceneGeometryExtractor>())
    , m_accelerationStructure(std::make_unique<AccelerationStructure>()) {
}

RTXSceneManager::~RTXSceneManager() {
    Shutdown();
}

bool RTXSceneManager::Initialize(DX12Device* device) {
    if (!device) {
        SPDLOG_ERROR("[RTX] RTXSceneManager::Initialize: null device");
        return false;
    }

    m_device = device;

    if (!m_accelerationStructure->Initialize(device)) {
        SPDLOG_ERROR("[RTX] RTXSceneManager: failed to initialize acceleration structures");
        return false;
    }

    SPDLOG_INFO("[RTX] RTXSceneManager initialized");
    return true;
}

void RTXSceneManager::Shutdown() {
    if (m_accelerationStructure) {
        m_accelerationStructure->ReleaseAll();
    }
    m_roomGeometry.clear();
    m_sceneLoaded = false;
    m_currentScene = -1;
    m_currentSceneConfig = GetDefaultConfig();
    m_device = nullptr;
    SPDLOG_INFO("[RTX] RTXSceneManager shut down");
}

void RTXSceneManager::OnSceneLoaded(int sceneNum) {
    m_currentScene = sceneNum;
    m_sceneLoaded = true;
    m_roomGeometry.clear();

    m_currentSceneConfig = LoadSceneConfig(sceneNum);

    SPDLOG_INFO("[RTX] RTXSceneManager: scene loaded 0x{:02X} (enabled: {}, GI: {:.2f}, bounces: {})",
                sceneNum, m_currentSceneConfig.enabled,
                m_currentSceneConfig.giIntensity, m_currentSceneConfig.maxBounces);
}

void RTXSceneManager::OnSceneUnload() {
    m_sceneLoaded = false;
    m_currentScene = -1;
    m_currentSceneConfig = GetDefaultConfig();
    ClearSceneOverride();

    if (m_accelerationStructure) {
        m_accelerationStructure->ReleaseAll();
    }
    m_roomGeometry.clear();

    TextureManager::GetInstance().ReleaseAllTextures();

    SPDLOG_INFO("[RTX] RTXSceneManager: scene unloaded");
}

void RTXSceneManager::OnRoomLoaded(void* playPtr, int roomNum) {
    SPDLOG_INFO("[RTX] RTXSceneManager: room {} loaded", roomNum);

    if (!playPtr || !m_geometryExtractor || !m_accelerationStructure) {
        SPDLOG_ERROR("[RTX] RTXSceneManager::OnRoomLoaded: missing dependencies");
        return;
    }

    PlayState* play = (PlayState*)playPtr;
    Room* room = nullptr;

    if (play->roomCtx.curRoom.num == roomNum) {
        room = &play->roomCtx.curRoom;
    } else if (play->roomCtx.prevRoom.num == roomNum) {
        room = &play->roomCtx.prevRoom;
    }

    if (!room || !room->segment) {
        SPDLOG_WARN("[RTX] RTXSceneManager: could not find Room struct for room {}", roomNum);
        return;
    }

    RoomGeometry geometry = m_geometryExtractor->ExtractRoomGeometry(room, (uint32_t)roomNum);

    bool hasGeometry = !geometry.opaqueMesh.vertices.empty() || !geometry.alphaMesh.vertices.empty();
    if (!hasGeometry) {
        SPDLOG_WARN("[RTX] RTXSceneManager: no geometry extracted for room {}", roomNum);
        return;
    }

    ResolveMaterialTextures(geometry);

    if (!m_accelerationStructure->BuildBLAS(geometry)) {
        SPDLOG_ERROR("[RTX] RTXSceneManager: failed to build BLAS for room {}", roomNum);
        return;
    }

    m_roomGeometry.push_back(std::move(geometry));

    SPDLOG_INFO("[RTX] RTXSceneManager: room {} ready, {} instances total",
                roomNum, m_accelerationStructure->GetInstanceCount());
}

void RTXSceneManager::ResolveMaterialTextures(RoomGeometry& geometry) {
    auto& texMgr = TextureManager::GetInstance();

    auto resolveMesh = [&](ExtractedMesh& mesh) {
        for (size_t i = 0; i < mesh.materials.size(); i++) {
            Material& mat = mesh.materials[i];

            uintptr_t textureAddr = 0;
            if (i < mesh.materialTextureAddrs.size()) {
                textureAddr = mesh.materialTextureAddrs[i];
            }

            if (textureAddr == 0) {
                mat.textureIndex = 0;
                continue;
            }

            // FNV-1a hash matching RTX_InterceptTexture
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
            } else {
                srvIndex = texMgr.GetSRVIndexForHash(static_cast<uint64_t>(textureAddr));
                mat.textureIndex = (srvIndex > 0) ? srvIndex : 0;
            }
        }
    };

    resolveMesh(geometry.opaqueMesh);
    resolveMesh(geometry.alphaMesh);
}

} // namespace RTX

#endif // ENABLE_DX12_RTX
