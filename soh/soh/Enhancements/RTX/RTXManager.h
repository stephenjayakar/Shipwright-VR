#pragma once
#ifndef RTX_MANAGER_H
#define RTX_MANAGER_H

#ifdef ENABLE_DX12_RTX

#include "RTXRenderer.h"
#include <memory>
#include <cstdint>

namespace RTX {

/**
 * RTXManager - High-level manager for the RTX rendering subsystem.
 *
 * Provides a simplified singleton facade for initializing, querying, and
 * shutting down the RTX renderer.  Game-side code (hooks, UI) should
 * interact with RTXManager rather than RTXRenderer directly so that
 * future refactoring of the renderer internals doesn't ripple outward.
 */
class RTXManager {
public:
    static RTXManager& Get();

    // Lifecycle
    bool Initialize(void* hwnd, uint32_t width, uint32_t height);
    void Shutdown();
    bool IsInitialized() const;
    bool IsRTXSupported() const;

    // Accessors
    RTXRenderer* GetRenderer() { return m_renderer.get(); }

    // Scene management delegation
    void OnSceneLoaded(int sceneNum);
    void OnSceneUnload();
    void OnRoomLoaded(void* play, int roomNum);

    // Frame lifecycle
    void BeginFrame();
    void EndFrame();

private:
    RTXManager() = default;
    ~RTXManager();
    RTXManager(const RTXManager&) = delete;
    RTXManager& operator=(const RTXManager&) = delete;

    std::unique_ptr<RTXRenderer> m_renderer;
    bool m_initialized = false;
};

} // namespace RTX

#endif // ENABLE_DX12_RTX
#endif // RTX_MANAGER_H
