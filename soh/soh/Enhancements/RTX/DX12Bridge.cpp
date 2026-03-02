#ifdef ENABLE_DX12_RTX

// =============================================================================
// DX12Bridge.cpp — Connects the RTX renderer to the gfx_dxgi swap chain
// =============================================================================
// This file implements the RTX side of the DX12 bridge. It registers override
// callbacks with the bridge infrastructure (in libultraship/gfx_dx12_bridge.cpp)
// so that gfx_dxgi.cpp creates the swap chain with the DX12 command queue
// instead of the DX11 device. This means there is ONE swap chain, owned by DX12.
//
// Flow:
//   1. RTX_EarlyInit() is called from OTRGlobals.cpp BEFORE gfx_dxgi Init()
//   2. We probe DX12/DXR, create device + command queue
//   3. We register bridge callbacks so GfxDX12Bridge_IsActive() returns true
//   4. gfx_dxgi.cpp::CreateSwapChain() sees bridge active, uses DX12 queue
//   5. gfx_dxgi calls GfxDX12Bridge_SetSwapChain() → we store it
//   6. gfx_dxgi calls GfxDX12Bridge_Present() → we render & present via DX12
// =============================================================================

#include "DX12Device.h"
#include "RTXRenderer.h"
#include "RTXDiagLog.h"

#include <cstdio>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <Windows.h>
#include <wrl/client.h>

// Bridge header from libultraship
#include <fast/backends/gfx_dx12_bridge.h>

using Microsoft::WRL::ComPtr;

// =============================================================================
// Module state
// =============================================================================
static bool s_bridgeInitialized = false;
static bool s_probeSucceeded = false;
static IDXGISwapChain1* s_sharedSwapChain = nullptr;
static HWND s_gameHWND = nullptr;

// The DX12Device used for the bridge. Owned by RTXRenderer, but we cache
// a pointer here for fast access from bridge callbacks.
static RTX::DX12Device* s_dx12Device = nullptr;

// =============================================================================
// Bridge callback implementations
// =============================================================================

// Returns true when we have a DX12 device with a command queue ready.
// gfx_dxgi.cpp checks this to decide whether to use DX12 for swap chain creation.
static bool BridgeIsActive() {
    return s_probeSucceeded && s_dx12Device != nullptr && 
           s_dx12Device->GetCommandQueue() != nullptr;
}

// Returns the DX12 command queue for swap chain creation.
static IUnknown* BridgeGetCommandQueue() {
    if (s_dx12Device) {
        return s_dx12Device->GetCommandQueue();
    }
    return nullptr;
}

// Called by gfx_dxgi.cpp after creating the swap chain.
// We store it and set up DX12 RTVs for the back buffers.
static void BridgeSetSwapChain(IDXGISwapChain1* swapChain) {
    s_sharedSwapChain = swapChain;
    
    printf("[DX12Bridge] SetSwapChain: received swap chain %p\n", (void*)swapChain);
    RTX_DIAG("DX12Bridge: SetSwapChain %p", (void*)swapChain);
    
    // Log to diagnostic file
    {
        FILE* f = fopen("rtx_bridge_debug.log", "a");
        if (f) {
            fprintf(f, "=== DX12Bridge SetSwapChain ===\n");
            fprintf(f, "  swapChain=%p device=%p cmdQueue=%p\n",
                    (void*)swapChain,
                    s_dx12Device ? (void*)s_dx12Device->GetDevice() : nullptr,
                    s_dx12Device ? (void*)s_dx12Device->GetCommandQueue() : nullptr);
            if (swapChain) {
                DXGI_SWAP_CHAIN_DESC desc = {};
                swapChain->GetDesc(&desc);
                fprintf(f, "  SC: HWND=%p %ux%u fmt=%u buffers=%u\n",
                        (void*)desc.OutputWindow, desc.BufferDesc.Width, desc.BufferDesc.Height,
                        (unsigned)desc.BufferDesc.Format, desc.BufferCount);
            }
            fflush(f);
            fclose(f);
        }
    }

    // Notify DX12Device about the shared swap chain so it can create RTVs
    if (s_dx12Device) {
        s_dx12Device->SetSharedSwapChain(swapChain);
    }
}

// Called by gfx_dxgi.cpp during SwapBuffersBegin when bridge is active.
// This is where DX12 renders and presents.
static void BridgePresent(IDXGISwapChain1* swapChain) {
    static uint32_t s_presentCallCount = 0;
    s_presentCallCount++;

    // Direct file logging for crash diagnosis (RTX_DIAG might not capture if crash is here)
    if (s_presentCallCount <= 5) {
        FILE* f = fopen("rtx_bridge_debug.log", "a");
        if (f) {
            fprintf(f, "BridgePresent #%u: swapChain=%p, renderer=%p\n",
                    s_presentCallCount, (void*)swapChain,
                    (void*)RTX::RTXRenderer::Instance());
            fflush(f);
            fclose(f);
        }
    }

    auto* renderer = RTX::RTXRenderer::Instance();
    if (renderer) {
        renderer->RenderAndPresentFrame();

        if (s_presentCallCount <= 5) {
            FILE* f = fopen("rtx_bridge_debug.log", "a");
            if (f) {
                fprintf(f, "BridgePresent #%u: RenderAndPresentFrame returned OK\n", s_presentCallCount);
                fflush(f);
                fclose(f);
            }
        }
    } else {
        // Fallback: just present whatever is in the back buffer
        if (swapChain) {
            swapChain->Present(1, 0);
        }
    }
}

// Called by gfx_dxgi.cpp to provide the HWND
static void BridgeSetHWND(HWND hwnd) {
    s_gameHWND = hwnd;
    printf("[DX12Bridge] SetHWND: %p\n", (void*)hwnd);
    RTX_DIAG("DX12Bridge: SetHWND %p", (void*)hwnd);
}

// =============================================================================
// Early initialization — called BEFORE gfx_dxgi Init()
// =============================================================================
// This creates the DX12 device and command queue, then registers bridge callbacks.
// After this returns, GfxDX12Bridge_IsActive() will return true, so when
// gfx_dxgi.cpp creates the swap chain, it will use our DX12 command queue.
extern "C" void RTX_EarlyInit() {
    printf("[DX12Bridge] RTX_EarlyInit() starting...\n");
    RTX_DIAG("DX12Bridge: RTX_EarlyInit() called");
    
    // Log to file
    {
        FILE* f = fopen("rtx_bridge_debug.log", "w");
        if (f) {
            fprintf(f, "=== DX12Bridge RTX_EarlyInit ===\n");
            fprintf(f, "  compiled at %s %s\n", __DATE__, __TIME__);
            fflush(f);
            fclose(f);
        }
    }
    
    if (s_bridgeInitialized) {
        printf("[DX12Bridge] Already initialized, skipping\n");
        return;
    }
    
    // Get the RTXRenderer instance (creates singleton if needed)
    auto* renderer = RTX::RTXRenderer::Instance();
    if (!renderer) {
        printf("[DX12Bridge] ERROR: Could not create RTXRenderer instance\n");
        return;
    }
    
    // Probe DX12/DXR support (creates DX12 device, checks raytracing)
    // This also creates the command queue via our modified ProbeDevice path
    s_probeSucceeded = renderer->ProbeRTXSupport();
    
    if (!s_probeSucceeded) {
        printf("[DX12Bridge] DX12/DXR probe failed — bridge will NOT be active\n");
        FILE* f = fopen("rtx_bridge_debug.log", "a");
        if (f) {
            fprintf(f, "  Probe FAILED — DX12/DXR not available\n");
            fflush(f);
            fclose(f);
        }
        return;
    }
    
    // Cache the DX12Device pointer for bridge callbacks
    s_dx12Device = renderer->GetDevice();
    
    if (!s_dx12Device || !s_dx12Device->GetCommandQueue()) {
        printf("[DX12Bridge] ERROR: Probe succeeded but command queue is null!\n");
        s_probeSucceeded = false;
        FILE* f = fopen("rtx_bridge_debug.log", "a");
        if (f) {
            fprintf(f, "  Probe succeeded but command queue is NULL!\n");
            fprintf(f, "  device=%p, cmdQueue=%p\n",
                    s_dx12Device ? (void*)s_dx12Device->GetDevice() : nullptr,
                    s_dx12Device ? (void*)s_dx12Device->GetCommandQueue() : nullptr);
            fflush(f);
            fclose(f);
        }
        return;
    }
    
    printf("[DX12Bridge] Probe succeeded! DX12 device=%p, cmdQueue=%p\n",
           (void*)s_dx12Device->GetDevice(), (void*)s_dx12Device->GetCommandQueue());
    
    // Register bridge callbacks — this is the critical step that makes
    // GfxDX12Bridge_IsActive() return true so gfx_dxgi uses our DX12 queue
    GfxDX12Bridge_RegisterIsActive(BridgeIsActive);
    GfxDX12Bridge_RegisterGetCommandQueue(BridgeGetCommandQueue);
    GfxDX12Bridge_RegisterSetSwapChain(BridgeSetSwapChain);
    GfxDX12Bridge_RegisterPresent(BridgePresent);
    GfxDX12Bridge_RegisterSetHWND(BridgeSetHWND);
    
    s_bridgeInitialized = true;
    
    printf("[DX12Bridge] Bridge callbacks registered — DX12 will own the swap chain\n");
    
    // Log success
    {
        FILE* f = fopen("rtx_bridge_debug.log", "a");
        if (f) {
            fprintf(f, "  Probe SUCCEEDED\n");
            fprintf(f, "  DX12 device=%p\n", (void*)s_dx12Device->GetDevice());
            fprintf(f, "  DX12 cmdQueue=%p\n", (void*)s_dx12Device->GetCommandQueue());
            fprintf(f, "  Bridge callbacks registered\n");
            fprintf(f, "  GfxDX12Bridge_IsActive() should now return true\n");
            fflush(f);
            fclose(f);
        }
    }
}

// Check if bridge-based DX12 is active (for external queries)
extern "C" int RTX_IsBridgeActive() {
    return s_bridgeInitialized && s_probeSucceeded && s_sharedSwapChain != nullptr;
}

// Get the shared swap chain (for external queries)
extern "C" void* RTX_GetSharedSwapChain() {
    return s_sharedSwapChain;
}

#else // !ENABLE_DX12_RTX

extern "C" void RTX_EarlyInit() {}
extern "C" int RTX_IsBridgeActive() { return 0; }
extern "C" void* RTX_GetSharedSwapChain() { return nullptr; }

#endif // ENABLE_DX12_RTX
