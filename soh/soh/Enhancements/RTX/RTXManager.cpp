#ifdef ENABLE_DX12_RTX

#include "RTXManager.h"
#include <spdlog/spdlog.h>

namespace RTX {

RTXManager& RTXManager::Get() {
    static RTXManager instance;
    return instance;
}

RTXManager::~RTXManager() {
    Shutdown();
}

bool RTXManager::Initialize(void* hwnd, uint32_t width, uint32_t height) {
    if (m_initialized) {
        SPDLOG_WARN("[RTX] RTXManager already initialized");
        return true;
    }

    SPDLOG_INFO("[RTX] RTXManager::Initialize ({}x{})", width, height);

    m_renderer = std::make_unique<RTXRenderer>();
    if (!m_renderer->Initialize(static_cast<HWND>(hwnd), width, height)) {
        SPDLOG_ERROR("[RTX] RTXManager: renderer initialization failed");
        m_renderer.reset();
        return false;
    }

    m_initialized = true;
    SPDLOG_INFO("[RTX] RTXManager initialized successfully");
    return true;
}

void RTXManager::Shutdown() {
    if (!m_initialized) return;

    SPDLOG_INFO("[RTX] RTXManager::Shutdown");
    if (m_renderer) {
        m_renderer->Shutdown();
        m_renderer.reset();
    }
    m_initialized = false;
}

bool RTXManager::IsInitialized() const {
    return m_initialized && m_renderer != nullptr;
}

bool RTXManager::IsRTXSupported() const {
    if (!m_initialized || !m_renderer) return false;
    auto* device = m_renderer->GetDevice();
    return device && device->SupportsRaytracing();
}

void RTXManager::OnSceneLoaded(int sceneNum) {
    if (m_renderer) {
        m_renderer->OnSceneLoaded(sceneNum);
    }
}

void RTXManager::OnSceneUnload() {
    if (m_renderer) {
        m_renderer->OnSceneUnload();
    }
}

void RTXManager::OnRoomLoaded(void* play, int roomNum) {
    if (m_renderer) {
        m_renderer->OnRoomLoaded(play, roomNum);
    }
}

void RTXManager::BeginFrame() {
    // Reserved for future per-frame setup (e.g., profiling, debug overlays)
}

void RTXManager::EndFrame() {
    if (m_renderer && RTXRenderer::IsActive()) {
        m_renderer->DispatchAndPresent();
    }
}

} // namespace RTX

#endif // ENABLE_DX12_RTX
