#ifdef ENABLE_DX12_RTX

#include "DX12Device.h"
#include "RTXDiagLog.h"
#include <cassert>
#include <cstdio>
#include <spdlog/spdlog.h>

// Link libraries
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

// Window class name for the DX12 overlay child window
static const wchar_t* RTX_OVERLAY_CLASS = L"RTX_DX12_Overlay";
static bool s_overlayClassRegistered = false;

namespace RTX {

DX12Device::DX12Device() = default;

DX12Device::~DX12Device() {
    Shutdown();
}

bool DX12Device::ProbeDevice() {
    RTX_DIAG("DX12Device::ProbeDevice() called (already probed=%s)", m_probed ? "yes" : "no");
    if (m_probed) {
        RTX_DIAG("DX12Device::ProbeDevice() already probed, result=%s", m_raytracingSupported ? "DXR supported" : "DXR NOT supported");
        return m_raytracingSupported;
    }

    SPDLOG_INFO("[RTX] Probing DX12 device and raytracing support...");
    RTX_DIAG("DX12Device: Attempting DX12 device creation...");

    if (!CreateDevice()) {
        RTX_DIAG("DX12Device: Device creation FAILED");
        SPDLOG_ERROR("[RTX] Probe failed: no DX12 device");
        return false;
    }
    RTX_DIAG("DX12Device: Device creation SUCCEEDED");

    if (!CheckRaytracingSupport()) {
        RTX_DIAG("DX12Device: DXR support check FAILED");
        SPDLOG_ERROR("[RTX] Probe failed: no DXR support");
        // Release device resources created during probe
        m_device.Reset();
        m_adapter.Reset();
        m_factory.Reset();
        return false;
    }
    RTX_DIAG("DX12Device: DXR support check PASSED");

    // Create command queue during probe so the DX12 Bridge can use it
    // for swap chain creation BEFORE CompleteInitialization is called.
    // This is critical for the shared swap chain approach.
    if (!m_commandQueue) {
        RTX_DIAG("DX12Device: Creating command queue during probe (for DX12 Bridge)...");
        if (!CreateCommandQueue()) {
            RTX_DIAG("DX12Device: Command queue creation during probe FAILED");
            SPDLOG_ERROR("[RTX] Probe: failed to create command queue");
            m_device.Reset();
            m_adapter.Reset();
            m_factory.Reset();
            return false;
        }
        RTX_DIAG("DX12Device: Command queue created during probe: %p", (void*)m_commandQueue.Get());
    }

    m_probed = true;
    SPDLOG_INFO("[RTX] Probe succeeded: DX12 + DXR available (command queue ready)");
    RTX_DIAG("DX12Device::ProbeDevice() SUCCESS - DX12 + DXR available, cmdQueue=%p", (void*)m_commandQueue.Get());
    return true;
}

bool DX12Device::CompleteInitialization(HWND hwnd, uint32_t width, uint32_t height) {
    printf("[RTX C15] DX12Device::CompleteInitialization() called (%ux%u, hwnd=%p)\n", width, height, (void*)hwnd);
    OutputDebugStringA("[RTX C15] CompleteInitialization() ENTRY\n");
    RTX_DIAG("DX12Device::CompleteInitialization() hwnd=%p, %ux%u", (void*)hwnd, width, height);

    // CYCLE 15: Log to diagnostic file
    {
        FILE* f = fopen("rtx_nuclear_test.log", "a");
        if (f) {
            fprintf(f, "\n=== CYCLE 15: DX12Device::CompleteInitialization ===\n");
            fprintf(f, "  HWND=%p %ux%u probed=%s dxr=%s initialized=%s\n",
                    (void*)hwnd, width, height,
                    m_probed ? "YES" : "NO",
                    m_raytracingSupported ? "YES" : "NO",
                    m_initialized ? "YES" : "NO");
            if (hwnd) {
                fprintf(f, "  HWND.visible=%s HWND.isWindow=%s\n",
                        IsWindowVisible(hwnd) ? "YES" : "NO",
                        IsWindow(hwnd) ? "YES" : "NO");
                RECT r = {};
                GetClientRect(hwnd, &r);
                fprintf(f, "  HWND.clientRect=%ldx%ld\n", r.right - r.left, r.bottom - r.top);
            }
            fflush(f);
            fclose(f);
        }
    }

    if (m_initialized) {
        RTX_DIAG("DX12Device: Already initialized, returning true");
        return true;
    }
    if (!m_probed || !m_raytracingSupported) {
        RTX_DIAG("DX12Device: CompleteInitialization FAILED - not probed or DXR not supported");
        SPDLOG_ERROR("[RTX] CompleteInitialization called without successful ProbeDevice()");
        return false;
    }

    m_hwnd = hwnd;
    m_width = width;
    m_height = height;

    if (!m_commandQueue) {
        RTX_DIAG("DX12Device: Creating command queue...");
        if (!CreateCommandQueue()) { RTX_DIAG("DX12Device: Command queue creation FAILED"); return false; }
    } else {
        RTX_DIAG("DX12Device: Command queue already exists from probe: %p", (void*)m_commandQueue.Get());
    }
    RTX_DIAG("DX12Device: Creating swap chain...");
    if (!CreateSwapChain(hwnd)) { RTX_DIAG("DX12Device: Swap chain creation FAILED"); return false; }
    RTX_DIAG("DX12Device: Creating descriptor heaps...");
    if (!CreateDescriptorHeaps()) { RTX_DIAG("DX12Device: Descriptor heap creation FAILED"); return false; }
    RTX_DIAG("DX12Device: Creating render target views...");
    if (!CreateRenderTargetViews()) { RTX_DIAG("DX12Device: RTV creation FAILED"); return false; }
    RTX_DIAG("DX12Device: Creating command allocators and list...");
    if (!CreateCommandAllocatorsAndList()) { RTX_DIAG("DX12Device: Cmd allocator creation FAILED"); return false; }
    RTX_DIAG("DX12Device: Creating fence...");
    if (!CreateFence()) { RTX_DIAG("DX12Device: Fence creation FAILED"); return false; }

    m_initialized = true;
    printf("[RTX C15] DX12 Device created successfully (two-phase, %ux%u)\n", width, height);
    OutputDebugStringA("[RTX C15] CompleteInitialization SUCCESS!\n");
    RTX_DIAG("[DIAG] DX12Device::CompleteInitialization SUCCESS - Device fully initialized (%ux%u), DXR=%s",
             width, height, m_raytracingSupported ? "supported" : "NOT supported");
    SPDLOG_INFO("[RTX] DX12 device fully initialized ({}x{})", width, height);

    // CYCLE 15: Log success details to file for verification
    {
        FILE* f = fopen("rtx_nuclear_test.log", "a");
        if (f) {
            fprintf(f, "\n=== CYCLE 15: DX12 INIT SUCCESS ===\n");
            fprintf(f, "  swapChain=%p cmdQueue=%p cmdList=%p device=%p\n",
                    (void*)m_swapChain.Get(), (void*)m_commandQueue.Get(),
                    (void*)m_commandList.Get(), (void*)m_device.Get());
            fprintf(f, "  rtvHeap=%p rtvDescSize=%u\n",
                    (void*)m_rtvHeap.Get(), m_rtvDescriptorSize);
            fprintf(f, "  renderTarget[0]=%p renderTarget[1]=%p\n",
                    (void*)m_renderTargets[0].Get(), (void*)m_renderTargets[1].Get());
            fprintf(f, "  frameIndex=%u %ux%u\n", m_frameIndex, m_width, m_height);
            fprintf(f, "  usingOverlay=%s overlayHwnd=%p parentHwnd=%p\n",
                    m_usingOverlayHwnd ? "YES" : "NO", (void*)m_overlayHwnd, (void*)m_hwnd);
            if (m_swapChain) {
                DXGI_SWAP_CHAIN_DESC scDesc = {};
                m_swapChain->GetDesc(&scDesc);
                fprintf(f, "  SC.HWND=%p SC.fmt=%u SC.buffers=%u SC.%ux%u\n",
                        (void*)scDesc.OutputWindow, (unsigned)scDesc.BufferDesc.Format,
                        scDesc.BufferCount, scDesc.BufferDesc.Width, scDesc.BufferDesc.Height);
            }
            HRESULT healthHr = m_device->GetDeviceRemovedReason();
            fprintf(f, "  DeviceHealth=0x%08X (%s)\n", (unsigned)healthHr,
                    healthHr == S_OK ? "HEALTHY" : "REMOVED!");
            fprintf(f, "=== READY TO PRESENT ===\n\n");
            fflush(f);
            fclose(f);
        }
    }

    return true;
}

bool DX12Device::Initialize(HWND hwnd, uint32_t width, uint32_t height) {
    printf("[RTX] DX12Device::Initialize() called (%ux%u, hwnd=%p)\n", width, height, (void*)hwnd);
    if (m_initialized) {
        return true;
    }

    // If not yet probed, do a full single-phase init (legacy path)
    if (!m_probed) {
        m_hwnd = hwnd;
        m_width = width;
        m_height = height;

        if (!CreateDevice()) return false;
        if (!CheckRaytracingSupport()) return false;
        m_probed = true;
        if (!CreateCommandQueue()) return false;
        if (!CreateSwapChain(hwnd)) return false;
        if (!CreateDescriptorHeaps()) return false;
        if (!CreateRenderTargetViews()) return false;
        if (!CreateCommandAllocatorsAndList()) return false;
        if (!CreateFence()) return false;

        m_initialized = true;
        printf("[RTX] DX12 Device created successfully (single-phase, %ux%u)\n", width, height);
        RTX_DIAG("[DIAG] DX12Device::Initialize SUCCESS (single-phase) - Device initialized (%ux%u)", width, height);
        SPDLOG_INFO("[RTX] DX12 device initialized successfully ({}x{})", width, height);
        return true;
    }

    // Already probed — use the two-phase path
    return CompleteInitialization(hwnd, width, height);
}

void DX12Device::SetSharedSwapChain(IDXGISwapChain1* swapChain) {
    RTX_DIAG("DX12Device::SetSharedSwapChain() swapChain=%p", (void*)swapChain);
    printf("[RTX] DX12Device::SetSharedSwapChain(%p)\n", (void*)swapChain);
    
    if (!swapChain) {
        RTX_DIAG("DX12Device::SetSharedSwapChain() WARNING: null swap chain!");
        return;
    }
    
    // Query for IDXGISwapChain3 interface (needed for GetCurrentBackBufferIndex)
    ComPtr<IDXGISwapChain3> swapChain3;
    HRESULT hr = swapChain->QueryInterface(IID_PPV_ARGS(&swapChain3));
    if (FAILED(hr)) {
        RTX_DIAG("DX12Device::SetSharedSwapChain() IDXGISwapChain3 query FAILED hr=0x%08X", (uint32_t)hr);
        SPDLOG_ERROR("[RTX] SetSharedSwapChain: IDXGISwapChain3 query failed: 0x{:08X}", (uint32_t)hr);
        return;
    }
    
    m_swapChain = swapChain3;
    m_hasSharedSwapChain = true;
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
    
    // Get dimensions from the swap chain
    DXGI_SWAP_CHAIN_DESC1 desc = {};
    m_swapChain->GetDesc1(&desc);
    if (desc.Width > 0 && desc.Height > 0) {
        m_width = desc.Width;
        m_height = desc.Height;
    }
    
    // Get the HWND from the swap chain
    {
        DXGI_SWAP_CHAIN_DESC fullDesc = {};
        m_swapChain->GetDesc(&fullDesc);
        if (fullDesc.OutputWindow) {
            m_hwnd = fullDesc.OutputWindow;
        }
    }
    
    RTX_DIAG("DX12Device::SetSharedSwapChain() OK: swapChain3=%p, %ux%u, frameIndex=%u, hwnd=%p, bufferCount=%u",
             (void*)m_swapChain.Get(), m_width, m_height, m_frameIndex, (void*)m_hwnd, desc.BufferCount);
    
    // Log to diagnostic file
    {
        FILE* f = fopen("rtx_bridge_debug.log", "a");
        if (f) {
            fprintf(f, "=== DX12Device SetSharedSwapChain ===\n");
            fprintf(f, "  swapChain3=%p %ux%u frameIndex=%u hwnd=%p buffers=%u\n",
                    (void*)m_swapChain.Get(), m_width, m_height, m_frameIndex, (void*)m_hwnd, desc.BufferCount);
            fflush(f);
            fclose(f);
        }
    }
}

bool DX12Device::CompleteInitializationWithSharedSwapChain(uint32_t width, uint32_t height) {
    printf("[RTX] DX12Device::CompleteInitializationWithSharedSwapChain() %ux%u\n", width, height);
    RTX_DIAG("DX12Device::CompleteInitializationWithSharedSwapChain() %ux%u, hasSharedSC=%s",
             width, height, m_hasSharedSwapChain ? "yes" : "no");
    
    if (m_initialized) {
        RTX_DIAG("DX12Device: Already initialized");
        return true;
    }
    if (!m_probed || !m_raytracingSupported) {
        RTX_DIAG("DX12Device: CompleteInitializationWithSharedSwapChain FAILED - not probed");
        return false;
    }
    if (!m_swapChain) {
        RTX_DIAG("DX12Device: CompleteInitializationWithSharedSwapChain FAILED - no shared swap chain set");
        return false;
    }
    
    if (width > 0) m_width = width;
    if (height > 0) m_height = height;
    
    // Command queue should already exist from ProbeDevice
    if (!m_commandQueue) {
        RTX_DIAG("DX12Device: Creating command queue...");
        if (!CreateCommandQueue()) {
            RTX_DIAG("DX12Device: Command queue creation FAILED");
            return false;
        }
    }
    
    // Create descriptor heaps (needed for RTVs)
    RTX_DIAG("DX12Device: Creating descriptor heaps...");
    if (!CreateDescriptorHeaps()) {
        RTX_DIAG("DX12Device: Descriptor heap creation FAILED");
        return false;
    }
    
    // Create RTVs for the shared swap chain's back buffers
    RTX_DIAG("DX12Device: Creating render target views for shared swap chain...");
    if (!CreateRenderTargetViews()) {
        RTX_DIAG("DX12Device: RTV creation FAILED");
        return false;
    }
    
    // Create command allocators and list
    RTX_DIAG("DX12Device: Creating command allocators and list...");
    if (!CreateCommandAllocatorsAndList()) {
        RTX_DIAG("DX12Device: Command allocator creation FAILED");
        return false;
    }
    
    // Create fence
    RTX_DIAG("DX12Device: Creating fence...");
    if (!CreateFence()) {
        RTX_DIAG("DX12Device: Fence creation FAILED");
        return false;
    }
    
    m_initialized = true;
    
    printf("[RTX] DX12Device::CompleteInitializationWithSharedSwapChain() SUCCESS (%ux%u)\n", m_width, m_height);
    RTX_DIAG("DX12Device::CompleteInitializationWithSharedSwapChain() SUCCESS - fully initialized (%ux%u)", m_width, m_height);
    SPDLOG_INFO("[RTX] DX12 device initialized with shared swap chain ({}x{})", m_width, m_height);
    
    // Log success
    {
        FILE* f = fopen("rtx_bridge_debug.log", "a");
        if (f) {
            fprintf(f, "=== DX12Device Initialized with Shared SwapChain ===\n");
            fprintf(f, "  device=%p cmdQueue=%p swapChain=%p\n",
                    (void*)m_device.Get(), (void*)m_commandQueue.Get(), (void*)m_swapChain.Get());
            fprintf(f, "  %ux%u frameIndex=%u rtvHeap=%p\n",
                    m_width, m_height, m_frameIndex, (void*)m_rtvHeap.Get());
            for (uint32_t i = 0; i < BACK_BUFFER_COUNT; i++) {
                fprintf(f, "  renderTarget[%u]=%p\n", i, (void*)m_renderTargets[i].Get());
            }
            fflush(f);
            fclose(f);
        }
    }
    
    return true;
}

void DX12Device::Shutdown() {
    RTX_DIAG("DX12Device::Shutdown() called (initialized=%s, probed=%s)", m_initialized ? "yes" : "no", m_probed ? "yes" : "no");
    if (!m_initialized && !m_probed) return;

    if (m_initialized) {
        WaitForGPU();
    }

    if (m_fenceEvent) {
        CloseHandle(m_fenceEvent);
        m_fenceEvent = nullptr;
    }

    // Release all COM objects
    m_commandList.Reset();
    for (UINT i = 0; i < BACK_BUFFER_COUNT; i++) {
        m_commandAllocators[i].Reset();
        m_renderTargets[i].Reset();
        m_fenceValues[i] = 0;
    }
    m_fence.Reset();
    m_uavHeap.Reset();
    m_srvHeap.Reset();
    m_rtvHeap.Reset();
    // If the swap chain is shared (from DX12 bridge), don't release it — gfx_dxgi owns it
    if (!m_hasSharedSwapChain) {
        m_swapChain.Reset();
    } else {
        m_swapChain.Reset(); // Release our reference but gfx_dxgi still holds theirs
    }
    m_hasSharedSwapChain = false;
    m_commandQueue.Reset();
    m_device.Reset();
    m_adapter.Reset();
    m_factory.Reset();

    // Destroy overlay child window if we created one
    if (m_overlayHwnd && IsWindow(m_overlayHwnd)) {
        DestroyWindow(m_overlayHwnd);
        m_overlayHwnd = nullptr;
    }
    m_usingOverlayHwnd = false;

    m_initialized = false;
    m_probed = false;
    m_raytracingSupported = false;
    SPDLOG_INFO("[RTX] DX12 device shut down");
}

bool DX12Device::CreateDevice() {
    RTX_DIAG("DX12Device::CreateDevice() starting...");
    UINT dxgiFactoryFlags = 0;

#ifdef _DEBUG
    // Enable debug layer in debug builds
    ComPtr<ID3D12Debug> debugController;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
        debugController->EnableDebugLayer();
        dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
        RTX_DIAG("DX12Device: Debug layer enabled");
        SPDLOG_INFO("[RTX] DX12 debug layer enabled");
    }
#endif

    HRESULT hr = CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(&m_factory));
    if (FAILED(hr)) { printf("[RTX] DX12 FAILED: 0x%08X at line %d\n", (unsigned)hr, __LINE__); }
    if (FAILED(hr)) {
        RTX_DIAG("DX12Device: CreateDXGIFactory2 FAILED hr=0x%08X", (uint32_t)hr);
        SPDLOG_ERROR("[RTX] Failed to create DXGI factory: 0x{:08X}", (uint32_t)hr);
        return false;
    }
    RTX_DIAG("DX12Device: DXGI factory created successfully");

    // Enumerate adapters, prefer discrete GPU with DXR support
    ComPtr<IDXGIAdapter1> adapter;
    bool adapterFound = false;

    for (UINT i = 0; m_factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);

        // Log all adapters found
        {
            char adapterNameNarrow[256] = {};
            wcstombs(adapterNameNarrow, desc.Description, sizeof(adapterNameNarrow) - 1);
            RTX_DIAG("DX12Device: Adapter[%u] = '%s' (VRAM: %llu MB, flags=0x%X)",
                     i, adapterNameNarrow,
                     (unsigned long long)(desc.DedicatedVideoMemory / (1024 * 1024)),
                     desc.Flags);
        }

        // Skip software adapters
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
            RTX_DIAG("DX12Device: Adapter[%u] skipped (software adapter)", i);
            continue;
        }

        // Check if adapter supports D3D12 with feature level 12.1 (required for robust DXR support).
        // DXR technically only requires tier 1_0 on FL 12.0, but FL 12.1 ensures broader
        // compatibility with DXR features and avoids edge cases on some drivers.
        hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&m_device));
        if (FAILED(hr)) {
            printf("[RTX] DX12 FAILED: 0x%08X at line %d (FL12.1)\n", (unsigned)hr, __LINE__);
            RTX_DIAG("DX12Device: Adapter[%u] FL12.1 failed (hr=0x%08X), trying FL12.0...", i, (uint32_t)hr);
            // Fall back to 12.0 if 12.1 is not supported — DXR support is still checked
            // separately via D3D12_FEATURE_DATA_D3D12_OPTIONS5::RaytracingTier.
            hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&m_device));
            if (FAILED(hr)) { printf("[RTX] DX12 FAILED: 0x%08X at line %d (FL12.0)\n", (unsigned)hr, __LINE__); }
        }
        if (SUCCEEDED(hr)) {
            m_adapter = adapter;
            adapterFound = true;
            char adapterNameNarrow[256] = {};
            wcstombs(adapterNameNarrow, desc.Description, sizeof(adapterNameNarrow) - 1);
            RTX_DIAG("DX12Device: SELECTED adapter '%s' (VRAM: %llu MB)",
                     adapterNameNarrow,
                     (unsigned long long)(desc.DedicatedVideoMemory / (1024 * 1024)));
            SPDLOG_INFO("[RTX] Using adapter: {} (VRAM: {} MB)",
                        std::string(desc.Description, desc.Description + wcslen(desc.Description)),
                        desc.DedicatedVideoMemory / (1024 * 1024));
            break;
        } else {
            RTX_DIAG("DX12Device: Adapter[%u] FL12.0 also failed (hr=0x%08X)", i, (uint32_t)hr);
        }
    }

    if (!adapterFound) {
        RTX_DIAG("[DIAG] DX12Device::CreateDevice FAILED - No DX12-capable adapter found!");
        SPDLOG_ERROR("[RTX] No DX12-capable adapter found");
        return false;
    }

    RTX_DIAG("[DIAG] DX12Device::CreateDevice SUCCESS - DX12 device created, adapter found");
    return true;
}

bool DX12Device::CheckRaytracingSupport() {
    RTX_DIAG("DX12Device::CheckRaytracingSupport() checking DXR tier...");
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5 = {};
    HRESULT hr = m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5));
    if (FAILED(hr)) { printf("[RTX] DX12 FAILED: 0x%08X at line %d\n", (unsigned)hr, __LINE__); }

    if (FAILED(hr)) {
        RTX_DIAG("DX12Device: CheckFeatureSupport FAILED hr=0x%08X", (uint32_t)hr);
    }

    RTX_DIAG("DX12Device: DXR tier = %d (need >= %d for TIER_1_0)",
             (int)options5.RaytracingTier, (int)D3D12_RAYTRACING_TIER_1_0);

    if (FAILED(hr) || options5.RaytracingTier < D3D12_RAYTRACING_TIER_1_0) {
        RTX_DIAG("DX12Device: DXR NOT supported on this device (tier=%d)", (int)options5.RaytracingTier);
        SPDLOG_ERROR("[RTX] Raytracing not supported on this device (tier: {})",
                     (int)options5.RaytracingTier);
        m_raytracingSupported = false;
        return false;
    }

    m_raytracingSupported = true;
    RTX_DIAG("DX12Device: DXR IS supported (tier=%d)", (int)options5.RaytracingTier);
    SPDLOG_INFO("[RTX] Raytracing tier: {}", (int)options5.RaytracingTier);
    return true;
}

bool DX12Device::CreateCommandQueue() {
    RTX_DIAG("DX12Device::CreateCommandQueue() starting...");
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;

    HRESULT hr = m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_commandQueue));
    if (FAILED(hr)) { printf("[RTX] DX12 FAILED: 0x%08X at line %d\n", (unsigned)hr, __LINE__); }
    if (FAILED(hr)) {
        RTX_DIAG("DX12Device::CreateCommandQueue() FAILED hr=0x%08X", (uint32_t)hr);
        SPDLOG_ERROR("[RTX] Failed to create command queue: 0x{:08X}", (uint32_t)hr);
        return false;
    }

    RTX_DIAG("DX12Device::CreateCommandQueue() SUCCESS");
    return true;
}

// Window procedure for the DX12 overlay child window.
// Passes all messages to the parent (the game window) so input still works.
static LRESULT CALLBACK RTXOverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // For input messages, pass to the parent window
    switch (msg) {
        case WM_ERASEBKGND:
            return 1; // Prevent background erase (DX12 handles all rendering)
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
            return 0;
        }
        // Forward all input to parent so game input still works
        case WM_KEYDOWN:
        case WM_KEYUP:
        case WM_SYSKEYDOWN:
        case WM_SYSKEYUP:
        case WM_CHAR:
        case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
        case WM_MOUSEWHEEL:
        case WM_INPUT: {
            HWND parent = GetParent(hwnd);
            if (parent) {
                return SendMessage(parent, msg, wParam, lParam);
            }
            break;
        }
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

HWND DX12Device::GetOrCreateOverlayHwnd(HWND parentHwnd) {
    if (m_overlayHwnd && IsWindow(m_overlayHwnd)) {
        return m_overlayHwnd;
    }

    RTX_DIAG("DX12Device::GetOrCreateOverlayHwnd() creating child overlay for parent=%p", (void*)parentHwnd);

    // Register window class if needed
    if (!s_overlayClassRegistered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = RTXOverlayWndProc;
        wc.hInstance = GetModuleHandle(nullptr);
        wc.lpszClassName = RTX_OVERLAY_CLASS;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);

        if (RegisterClassExW(&wc)) {
            s_overlayClassRegistered = true;
            RTX_DIAG("DX12Device: Overlay window class registered");
        } else {
            DWORD err = GetLastError();
            if (err == ERROR_CLASS_ALREADY_EXISTS) {
                s_overlayClassRegistered = true;
            } else {
                RTX_DIAG("DX12Device: RegisterClassExW FAILED (err=%lu)", err);
                return nullptr;
            }
        }
    }

    // Get parent client area dimensions
    RECT clientRect = {};
    GetClientRect(parentHwnd, &clientRect);
    int w = clientRect.right - clientRect.left;
    int h = clientRect.bottom - clientRect.top;
    if (w <= 0) w = (int)m_width;
    if (h <= 0) h = (int)m_height;

    // Create child window that exactly covers the parent's client area.
    // WS_CHILD means it's clipped to the parent. WS_VISIBLE makes it immediately shown.
    // The child window sits ON TOP of the DX11 rendering area, so DX12 output
    // will naturally cover DX11 output without needing to release the DX11 swap chain.
    m_overlayHwnd = CreateWindowExW(
        0,                          // No extended style
        RTX_OVERLAY_CLASS,
        L"RTX DX12 Overlay",
        WS_CHILD | WS_VISIBLE,     // Child window, immediately visible
        0, 0, w, h,                // Cover entire client area
        parentHwnd,                 // Parent window
        nullptr,
        GetModuleHandle(nullptr),
        nullptr
    );

    if (!m_overlayHwnd) {
        DWORD err = GetLastError();
        RTX_DIAG("DX12Device: CreateWindowExW FAILED (err=%lu)", err);
        SPDLOG_ERROR("[RTX] Failed to create overlay child window: error {}", err);
        return nullptr;
    }

    // Ensure overlay is topmost among siblings
    SetWindowPos(m_overlayHwnd, HWND_TOP, 0, 0, w, h, SWP_SHOWWINDOW);

    RTX_DIAG("DX12Device: Overlay child HWND created: %p (%dx%d)", (void*)m_overlayHwnd, w, h);
    SPDLOG_INFO("[RTX] Created DX12 overlay child window: {} ({}x{})", (void*)m_overlayHwnd, w, h);

    // Log to diagnostic file
    {
        FILE* f = fopen("rtx_nuclear_test.log", "a");
        if (f) {
            fprintf(f, "\n=== DX12 OVERLAY CHILD WINDOW CREATED ===\n");
            fprintf(f, "  Parent HWND: %p\n", (void*)parentHwnd);
            fprintf(f, "  Overlay HWND: %p\n", (void*)m_overlayHwnd);
            fprintf(f, "  Size: %dx%d\n", w, h);
            fprintf(f, "  Visible: %s\n", IsWindowVisible(m_overlayHwnd) ? "YES" : "NO");
            fflush(f);
            fclose(f);
        }
    }

    m_usingOverlayHwnd = true;
    return m_overlayHwnd;
}

bool DX12Device::CreateSwapChain(HWND hwnd) {
    RTX_DIAG("DX12Device::CreateSwapChain() hwnd=%p, %ux%u, buffers=%u", (void*)hwnd, m_width, m_height, BACK_BUFFER_COUNT);
    OutputDebugStringA("[RTX C15] CreateSwapChain() ENTRY\n");

    // CYCLE 15: Comprehensive logging of swap chain creation
    {
        FILE* nf = fopen("rtx_nuclear_test.log", "a");
        if (nf) {
            fprintf(nf, "\n=== CYCLE 15: DX12 SWAP CHAIN CREATION ===\n");
            fprintf(nf, "  Input HWND=%p %ux%u bufferCount=%u\n", (void*)hwnd, m_width, m_height, BACK_BUFFER_COUNT);
            fprintf(nf, "  cmdQueue=%p factory=%p device=%p\n",
                    (void*)m_commandQueue.Get(), (void*)m_factory.Get(), (void*)m_device.Get());
            if (hwnd) {
                fprintf(nf, "  HWND.visible=%s HWND.enabled=%s HWND.isWindow=%s\n",
                        IsWindowVisible(hwnd) ? "YES" : "NO",
                        IsWindowEnabled(hwnd) ? "YES" : "NO",
                        IsWindow(hwnd) ? "YES" : "NO");
                RECT r = {};
                GetWindowRect(hwnd, &r);
                fprintf(nf, "  HWND.rect=%ld,%ld,%ld,%ld (%ldx%ld)\n",
                        r.left, r.top, r.right, r.bottom, r.right-r.left, r.bottom-r.top);
                RECT cr = {};
                GetClientRect(hwnd, &cr);
                fprintf(nf, "  HWND.clientRect=%ld,%ld,%ld,%ld (%ldx%ld)\n",
                        cr.left, cr.top, cr.right, cr.bottom, cr.right-cr.left, cr.bottom-cr.top);
                // Check window class
                wchar_t className[256] = {};
                GetClassNameW(hwnd, className, 256);
                char classNameA[256] = {};
                wcstombs(classNameA, className, sizeof(classNameA) - 1);
                fprintf(nf, "  HWND.className='%s'\n", classNameA);
                // Check parent
                HWND parent = GetParent(hwnd);
                fprintf(nf, "  HWND.parent=%p\n", (void*)parent);
            } else {
                fprintf(nf, "  *** WARNING: NULL HWND! ***\n");
            }
            fflush(nf);
            fclose(nf);
        }
    }

    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
    swapChainDesc.BufferCount = BACK_BUFFER_COUNT;
    swapChainDesc.Width = m_width;
    swapChainDesc.Height = m_height;
    swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;  // NOT sRGB — shaders output linear, gamma is in PostProcess
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.SampleDesc.Count = 1;

    RTX_DIAG("DX12Device::CreateSwapChain() desc: format=DXGI_FORMAT_R8G8B8A8_UNORM(%u), swap=FLIP_DISCARD, buffers=%u, %ux%u",
             (unsigned)swapChainDesc.Format, BACK_BUFFER_COUNT, m_width, m_height);

    ComPtr<IDXGISwapChain1> swapChain1;
    HWND swapChainHwnd = hwnd; // The HWND we'll actually use for the swap chain
    HRESULT hr = m_factory->CreateSwapChainForHwnd(
        m_commandQueue.Get(),
        hwnd,
        &swapChainDesc,
        nullptr,
        nullptr,
        &swapChain1
    );

    // CYCLE 15: Log creation result to file
    {
        FILE* nf = fopen("rtx_nuclear_test.log", "a");
        if (nf) {
            fprintf(nf, "  CreateSwapChainForHwnd(parent) hr=0x%08X (%s)\n", (unsigned)hr, SUCCEEDED(hr) ? "SUCCESS" : "FAILED");
            if (FAILED(hr)) {
                fprintf(nf, "  *** SWAP CHAIN ON PARENT FAILED! ***\n");
                if (hr == DXGI_ERROR_INVALID_CALL)
                    fprintf(nf, "  DXGI_ERROR_INVALID_CALL — DX11 swap chain still on this HWND!\n");
                else if (hr == E_INVALIDARG)
                    fprintf(nf, "  E_INVALIDARG — invalid parameters\n");
                else if (hr == DXGI_ERROR_DEVICE_REMOVED)
                    fprintf(nf, "  DXGI_ERROR_DEVICE_REMOVED — device gone\n");
                fprintf(nf, "  >>> FALLING BACK TO CHILD OVERLAY WINDOW <<<\n");
            }
            fflush(nf);
            fclose(nf);
        }
    }

    if (FAILED(hr)) {
        printf("[RTX] DX12 CreateSwapChainForHwnd on parent FAILED: 0x%08X — trying overlay child window\n", (unsigned)hr);
        RTX_DIAG("DX12Device::CreateSwapChain() parent HWND failed hr=0x%08X, creating overlay child window...", (uint32_t)hr);
        SPDLOG_WARN("[RTX] Swap chain on parent HWND failed (0x{:08X}), creating overlay child window...", (uint32_t)hr);

        // === FALLBACK: Create a child overlay window and bind swap chain to it ===
        // This avoids the DX11 swap chain conflict entirely.
        HWND overlayHwnd = GetOrCreateOverlayHwnd(hwnd);
        if (!overlayHwnd) {
            RTX_DIAG("DX12Device::CreateSwapChain() FAILED - could not create overlay child window");
            SPDLOG_ERROR("[RTX] Failed to create overlay child window");
            return false;
        }

        // Try creating swap chain on the child overlay window
        hr = m_factory->CreateSwapChainForHwnd(
            m_commandQueue.Get(),
            overlayHwnd,
            &swapChainDesc,
            nullptr,
            nullptr,
            &swapChain1
        );

        // Log overlay attempt result
        {
            FILE* nf = fopen("rtx_nuclear_test.log", "a");
            if (nf) {
                fprintf(nf, "  CreateSwapChainForHwnd(overlay) hr=0x%08X (%s)\n", (unsigned)hr, SUCCEEDED(hr) ? "SUCCESS" : "FAILED");
                fprintf(nf, "  Overlay HWND: %p (parent: %p)\n", (void*)overlayHwnd, (void*)hwnd);
                if (SUCCEEDED(hr)) {
                    fprintf(nf, "  >>> OVERLAY SWAP CHAIN CREATED SUCCESSFULLY! <<<\n");
                }
                fflush(nf);
                fclose(nf);
            }
        }

        if (FAILED(hr)) {
            printf("[RTX] DX12 CreateSwapChainForHwnd on overlay ALSO FAILED: 0x%08X\n", (unsigned)hr);
            RTX_DIAG("DX12Device::CreateSwapChain() overlay HWND also failed hr=0x%08X", (uint32_t)hr);
            SPDLOG_ERROR("[RTX] Failed to create swap chain even on overlay: 0x{:08X}", (uint32_t)hr);
            return false;
        }

        swapChainHwnd = overlayHwnd;
        printf("[RTX] DX12 swap chain created on OVERLAY child window %p (parent %p)\n", (void*)overlayHwnd, (void*)hwnd);
        RTX_DIAG("DX12Device::CreateSwapChain() SUCCESS via overlay child window %p", (void*)overlayHwnd);
        SPDLOG_INFO("[RTX] DX12 swap chain created on overlay child window: {}", (void*)overlayHwnd);
    } else {
        printf("[RTX] DX12 swap chain created directly on parent HWND %p\n", (void*)hwnd);
    }
    RTX_DIAG("DX12Device::CreateSwapChain() CreateSwapChainForHwnd OK (target=%p)", (void*)swapChainHwnd);

    // Disable ALT+ENTER fullscreen toggle (game handles its own fullscreen)
    m_factory->MakeWindowAssociation(swapChainHwnd, DXGI_MWA_NO_ALT_ENTER);

    hr = swapChain1.As(&m_swapChain);
    if (FAILED(hr)) {
        RTX_DIAG("DX12Device::CreateSwapChain() IDXGISwapChain3 query FAILED hr=0x%08X", (uint32_t)hr);
        SPDLOG_ERROR("[RTX] Failed to query IDXGISwapChain3: 0x{:08X}", (uint32_t)hr);
        return false;
    }

    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

    // CYCLE 12: Log final swap chain state
    {
        FILE* nf = fopen("rtx_nuclear_test.log", "a");
        if (nf) {
            fprintf(nf, "  SwapChain3 created at %p, initial frameIndex=%u\n", (void*)m_swapChain.Get(), m_frameIndex);
            // Verify the swap chain is associated with the correct HWND
            DXGI_SWAP_CHAIN_DESC verifyDesc = {};
            m_swapChain->GetDesc(&verifyDesc);
            fprintf(nf, "  Verify: HWND=%p %ux%u format=%u buffers=%u\n",
                    (void*)verifyDesc.OutputWindow, verifyDesc.BufferDesc.Width, verifyDesc.BufferDesc.Height,
                    (unsigned)verifyDesc.BufferDesc.Format, verifyDesc.BufferCount);
            fprintf(nf, "  Using overlay child window: %s\n", m_usingOverlayHwnd ? "YES" : "NO");
            if (m_usingOverlayHwnd) {
                fprintf(nf, "  Overlay HWND: %p (parent: %p)\n", (void*)m_overlayHwnd, (void*)hwnd);
            }
            fprintf(nf, "=== SWAP CHAIN CREATION COMPLETE ===\n\n");
            fflush(nf);
            fclose(nf);
        }
    }

    RTX_DIAG("DX12Device::CreateSwapChain() SUCCESS, initial frameIndex=%u", m_frameIndex);
    return true;
}

bool DX12Device::CreateDescriptorHeaps() {
    RTX_DIAG("DX12Device::CreateDescriptorHeaps() starting...");
    // RTV heap: 2 descriptors for double-buffered back buffers
    {
        D3D12_DESCRIPTOR_HEAP_DESC desc = {};
        desc.NumDescriptors = BACK_BUFFER_COUNT;
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

        HRESULT hr = m_device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_rtvHeap));
        if (FAILED(hr)) {
            RTX_DIAG("DX12Device: RTV heap creation FAILED hr=0x%08X", (uint32_t)hr);
            SPDLOG_ERROR("[RTX] Failed to create RTV heap: 0x{:08X}", (uint32_t)hr);
            return false;
        }

        m_rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        RTX_DIAG("DX12Device: RTV heap created OK (descriptorSize=%u)", m_rtvDescriptorSize);
    }

    // SRV heap: 1024 descriptors for textures (shader-visible)
    {
        D3D12_DESCRIPTOR_HEAP_DESC desc = {};
        desc.NumDescriptors = 1024;
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

        HRESULT hr = m_device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_srvHeap));
        if (FAILED(hr)) {
            RTX_DIAG("DX12Device: SRV heap creation FAILED hr=0x%08X", (uint32_t)hr);
            SPDLOG_ERROR("[RTX] Failed to create SRV heap: 0x{:08X}", (uint32_t)hr);
            return false;
        }

        m_srvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        RTX_DIAG("DX12Device: SRV heap created OK (1024 descriptors, descriptorSize=%u)", m_srvDescriptorSize);
    }

    // UAV heap: 16 descriptors for output buffers + GI accumulation (shader-visible)
    {
        D3D12_DESCRIPTOR_HEAP_DESC desc = {};
        desc.NumDescriptors = 16;
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

        HRESULT hr = m_device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_uavHeap));
        if (FAILED(hr)) {
            RTX_DIAG("DX12Device: UAV heap creation FAILED hr=0x%08X", (uint32_t)hr);
            SPDLOG_ERROR("[RTX] Failed to create UAV heap: 0x{:08X}", (uint32_t)hr);
            return false;
        }

        m_uavDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        RTX_DIAG("DX12Device: UAV heap created OK (16 descriptors, descriptorSize=%u)", m_uavDescriptorSize);
    }

    RTX_DIAG("DX12Device::CreateDescriptorHeaps() SUCCESS - all heaps created");
    return true;
}

bool DX12Device::CreateRenderTargetViews() {
    RTX_DIAG("DX12Device::CreateRenderTargetViews() starting (%u buffers)", BACK_BUFFER_COUNT);
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();

    for (UINT i = 0; i < BACK_BUFFER_COUNT; i++) {
        HRESULT hr = m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_renderTargets[i]));
        if (FAILED(hr)) {
            RTX_DIAG("DX12Device: GetBuffer(%u) FAILED hr=0x%08X", i, (uint32_t)hr);
            SPDLOG_ERROR("[RTX] Failed to get swap chain buffer {}: 0x{:08X}", i, (uint32_t)hr);
            return false;
        }
        RTX_DIAG("DX12Device: RTV[%u] created at %p", i, (void*)m_renderTargets[i].Get());

        m_device->CreateRenderTargetView(m_renderTargets[i].Get(), nullptr, rtvHandle);
        rtvHandle.ptr += m_rtvDescriptorSize;
    }

    RTX_DIAG("DX12Device::CreateRenderTargetViews() SUCCESS");
    return true;
}

bool DX12Device::CreateCommandAllocatorsAndList() {
    RTX_DIAG("DX12Device::CreateCommandAllocatorsAndList() starting (%u allocators)", BACK_BUFFER_COUNT);
    for (UINT i = 0; i < BACK_BUFFER_COUNT; i++) {
        HRESULT hr = m_device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&m_commandAllocators[i])
        );
        if (FAILED(hr)) {
            RTX_DIAG("DX12Device: Command allocator[%u] FAILED hr=0x%08X", i, (uint32_t)hr);
            SPDLOG_ERROR("[RTX] Failed to create command allocator {}: 0x{:08X}", i, (uint32_t)hr);
            return false;
        }
    }
    RTX_DIAG("DX12Device: %u command allocators created OK", BACK_BUFFER_COUNT);

    HRESULT hr = m_device->CreateCommandList(
        0,
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        m_commandAllocators[m_frameIndex].Get(),
        nullptr,
        IID_PPV_ARGS(&m_commandList)
    );
    if (FAILED(hr)) {
        RTX_DIAG("DX12Device: Command list creation FAILED hr=0x%08X", (uint32_t)hr);
        SPDLOG_ERROR("[RTX] Failed to create command list: 0x{:08X}", (uint32_t)hr);
        return false;
    }

    // Command list is created in recording state; close it so we can reset it per frame
    m_commandList->Close();
    RTX_DIAG("DX12Device::CreateCommandAllocatorsAndList() SUCCESS");
    return true;
}

bool DX12Device::CreateFence() {
    RTX_DIAG("DX12Device::CreateFence() starting...");
    HRESULT hr = m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence));
    if (FAILED(hr)) {
        RTX_DIAG("DX12Device: Fence creation FAILED hr=0x%08X", (uint32_t)hr);
        SPDLOG_ERROR("[RTX] Failed to create fence: 0x{:08X}", (uint32_t)hr);
        return false;
    }
    RTX_DIAG("DX12Device: Fence created OK");

    m_fenceValues[m_frameIndex] = 1;

    m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!m_fenceEvent) {
        RTX_DIAG("DX12Device: Fence event creation FAILED (GetLastError=%lu)", GetLastError());
        SPDLOG_ERROR("[RTX] Failed to create fence event");
        return false;
    }
    RTX_DIAG("DX12Device: Fence event created OK");

    // Wait for the GPU to complete any initial work
    RTX_DIAG("DX12Device: Initial GPU wait...");
    WaitForGPU();
    RTX_DIAG("DX12Device::CreateFence() SUCCESS");
    return true;
}

void DX12Device::BeginFrame() {
    static uint32_t s_beginFrameCount = 0;
    s_beginFrameCount++;
    if (s_beginFrameCount <= 5 || (s_beginFrameCount % 300) == 0) {
        RTX_DIAG("DX12Device::BeginFrame() #%u, frameIndex=%u", s_beginFrameCount, m_frameIndex);
    }

    // Validate state before proceeding — guard against use-after-shutdown
    if (!m_initialized || !m_commandAllocators[m_frameIndex] || !m_commandList) {
        RTX_DIAG("DX12Device::BeginFrame() SKIPPED — device not initialized or null allocator/cmdlist (frame #%u)", s_beginFrameCount);
        return;
    }

    // Periodically sync overlay child window size/visibility with parent
    if (m_usingOverlayHwnd && m_overlayHwnd && m_hwnd && (s_beginFrameCount % 60) == 1) {
        RECT parentRect = {};
        GetClientRect(m_hwnd, &parentRect);
        int pw = parentRect.right - parentRect.left;
        int ph = parentRect.bottom - parentRect.top;
        if (pw > 0 && ph > 0) {
            RECT overlayRect = {};
            GetClientRect(m_overlayHwnd, &overlayRect);
            int ow = overlayRect.right - overlayRect.left;
            int oh = overlayRect.bottom - overlayRect.top;
            if (ow != pw || oh != ph) {
                SetWindowPos(m_overlayHwnd, HWND_TOP, 0, 0, pw, ph, SWP_SHOWWINDOW);
                // Trigger swap chain resize
                OnResize((uint32_t)pw, (uint32_t)ph);
            } else {
                // Just ensure it's on top
                SetWindowPos(m_overlayHwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
            }
        }
    }

    // Reset command allocator and command list for this frame.
    // These can fail if the GPU hasn't finished with the allocator (shouldn't happen
    // after MoveToNextFrame's fence wait, but check defensively).
    HRESULT hr = m_commandAllocators[m_frameIndex]->Reset();
    if (FAILED(hr)) {
        RTX_DIAG("DX12Device::BeginFrame() command allocator Reset FAILED hr=0x%08X (frame #%u)", (uint32_t)hr, s_beginFrameCount);
        SPDLOG_ERROR("[RTX] BeginFrame: command allocator reset failed: 0x{:08X}", (uint32_t)hr);
        return;
    }
    hr = m_commandList->Reset(m_commandAllocators[m_frameIndex].Get(), nullptr);
    if (FAILED(hr)) {
        RTX_DIAG("DX12Device::BeginFrame() command list Reset FAILED hr=0x%08X (frame #%u)", (uint32_t)hr, s_beginFrameCount);
        SPDLOG_ERROR("[RTX] BeginFrame: command list reset failed: 0x{:08X}", (uint32_t)hr);
        return;
    }
}

void DX12Device::EndFrame() {
    // NOTE: This method is currently UNUSED — RTXRenderer::DispatchAndPresent()
    // manages its own back buffer state transitions and command list execution
    // directly. Retained for potential future use by non-RTX rendering paths.
    if (!m_initialized || !m_commandList || !m_renderTargets[m_frameIndex]) {
        RTX_DIAG("DX12Device::EndFrame() SKIPPED — device not initialized or null resources");
        return;
    }

    // Transition back buffer to present state
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = m_renderTargets[m_frameIndex].Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_commandList->ResourceBarrier(1, &barrier);

    // Close and execute the command list
    HRESULT hr = m_commandList->Close();
    if (FAILED(hr)) {
        RTX_DIAG("DX12Device::EndFrame() command list Close FAILED hr=0x%08X", (uint32_t)hr);
        SPDLOG_ERROR("[RTX] EndFrame: command list close failed: 0x{:08X}", (uint32_t)hr);
        return;
    }
    ID3D12CommandList* ppCommandLists[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(1, ppCommandLists);
}

bool DX12Device::Present() {
    static uint32_t s_presentCount = 0;
    s_presentCount++;

    if (!m_swapChain) {
        RTX_DIAG("DX12Device::Present() SKIPPED — null swap chain (frame #%u)", s_presentCount);
        if (s_presentCount <= 10) {
            FILE* nf = fopen("rtx_nuclear_test.log", "a");
            if (nf) {
                fprintf(nf, "DX12 Present #%u: NULL SWAP CHAIN!\n", s_presentCount);
                fflush(nf);
                fclose(nf);
            }
        }
        OutputDebugStringA("[RTX C15] Present: NULL swap chain!\n");
        return false;
    }

    // Output debug string every frame
    {
        char dbg[128];
        snprintf(dbg, sizeof(dbg), "[RTX C15] DX12 Present #%u (frameIdx=%u)\n", s_presentCount, m_frameIndex);
        OutputDebugStringA(dbg);
    }

    // Log detailed swap chain validation on first 10 presents and periodically
    if (s_presentCount <= 10 || (s_presentCount % 60) == 0) {
        DXGI_SWAP_CHAIN_DESC scDesc = {};
        m_swapChain->GetDesc(&scDesc);
        
        FILE* nf = fopen("rtx_nuclear_test.log", "a");
        if (nf) {
            fprintf(nf, "\n--- DX12 Present #%u PRE-VALIDATE ---\n", s_presentCount);
            fprintf(nf, "  swapChain=%p HWND=%p fmt=%u buffers=%u %ux%u\n",
                    (void*)m_swapChain.Get(), (void*)scDesc.OutputWindow,
                    (unsigned)scDesc.BufferDesc.Format, scDesc.BufferCount,
                    scDesc.BufferDesc.Width, scDesc.BufferDesc.Height);
            fprintf(nf, "  overlay=%s overlayHwnd=%p parentHwnd=%p\n",
                    m_usingOverlayHwnd ? "YES" : "NO", (void*)m_overlayHwnd, (void*)m_hwnd);
            if (scDesc.OutputWindow) {
                fprintf(nf, "  SC.HWND visible=%s enabled=%s isWindow=%s\n",
                        IsWindowVisible(scDesc.OutputWindow) ? "YES" : "NO",
                        IsWindowEnabled(scDesc.OutputWindow) ? "YES" : "NO",
                        IsWindow(scDesc.OutputWindow) ? "YES" : "NO");
                RECT r = {};
                GetWindowRect(scDesc.OutputWindow, &r);
                fprintf(nf, "  SC.HWND rect=%ld,%ld,%ld,%ld (%ldx%ld)\n",
                        r.left, r.top, r.right, r.bottom, r.right-r.left, r.bottom-r.top);
                HWND fgWnd = GetForegroundWindow();
                fprintf(nf, "  ForegroundWindow=%p (matches SC.HWND=%s, matches parent=%s)\n",
                        (void*)fgWnd,
                        (fgWnd == scDesc.OutputWindow) ? "YES" : "NO",
                        (fgWnd == m_hwnd) ? "YES" : "NO");
            }
            D3D12_RESOURCE_DESC bbDesc = m_renderTargets[m_frameIndex] ?
                m_renderTargets[m_frameIndex]->GetDesc() : D3D12_RESOURCE_DESC{};
            fprintf(nf, "  backBuffer[%u]=%p fmt=%u %llux%u\n",
                    m_frameIndex, (void*)m_renderTargets[m_frameIndex].Get(),
                    (unsigned)bbDesc.Format, (unsigned long long)bbDesc.Width, bbDesc.Height);
            // Device health check
            HRESULT healthHr = m_device->GetDeviceRemovedReason();
            fprintf(nf, "  DeviceHealth=0x%08X (%s)\n", (unsigned)healthHr,
                    healthHr == S_OK ? "HEALTHY" : "DEVICE_REMOVED");
            fflush(nf);
            fclose(nf);
        }

        char validateMsg[512];
        snprintf(validateMsg, sizeof(validateMsg),
                 "[RTX C15] Present #%u: sc=%p HWND=%p fmt=%u %ux%u overlay=%s bb[%u]=%p\n",
                 s_presentCount, (void*)m_swapChain.Get(), (void*)scDesc.OutputWindow,
                 (unsigned)scDesc.BufferDesc.Format, scDesc.BufferDesc.Width, scDesc.BufferDesc.Height,
                 m_usingOverlayHwnd ? "Y" : "N", m_frameIndex,
                 (void*)m_renderTargets[m_frameIndex].Get());
        OutputDebugStringA(validateMsg);
        printf("%s", validateMsg);
    }

    // Ensure overlay child window stays on top and covers parent (every frame)
    if (m_usingOverlayHwnd && m_overlayHwnd && IsWindow(m_overlayHwnd)) {
        // Bring overlay to top of sibling z-order every frame
        SetWindowPos(m_overlayHwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        
        // Every 30 frames, ensure overlay covers parent's full client area
        if ((s_presentCount % 30) == 1) {
            RECT parentClient = {};
            if (m_hwnd && GetClientRect(m_hwnd, &parentClient)) {
                int pw = parentClient.right - parentClient.left;
                int ph = parentClient.bottom - parentClient.top;
                if (pw > 0 && ph > 0) {
                    RECT overlayRect = {};
                    GetClientRect(m_overlayHwnd, &overlayRect);
                    int ow = overlayRect.right - overlayRect.left;
                    int oh = overlayRect.bottom - overlayRect.top;
                    if (ow != pw || oh != ph) {
                        SetWindowPos(m_overlayHwnd, HWND_TOP, 0, 0, pw, ph, SWP_SHOWWINDOW);
                    }
                }
            }
        }
    }

    HRESULT hr = m_swapChain->Present(1, 0); // VSync on

    // Log Present result
    if (s_presentCount <= 30 || (s_presentCount % 60) == 0) {
        FILE* nf = fopen("rtx_nuclear_test.log", "a");
        if (nf) {
            fprintf(nf, "  Present #%u result: hr=0x%08X (%s) frameIdx=%u %ux%u overlay=%s\n",
                    s_presentCount, (unsigned)hr, SUCCEEDED(hr) ? "OK" : "FAILED",
                    m_frameIndex, m_width, m_height,
                    m_usingOverlayHwnd ? "YES" : "NO");
            if (FAILED(hr) && (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)) {
                HRESULT reason = m_device->GetDeviceRemovedReason();
                fprintf(nf, "  DEVICE REMOVED reason: 0x%08X\n", (unsigned)reason);
            }
            fflush(nf);
            fclose(nf);
        }
    }

    if (hr == DXGI_STATUS_OCCLUDED) {
        // Window is occluded (minimized or covered). This is not an error.
        // Still move to next frame to keep sync.
        if (s_presentCount <= 10 || (s_presentCount % 300) == 0) {
            RTX_DIAG("DX12Device: Present #%u OCCLUDED (window hidden/minimized)", s_presentCount);
        }
        MoveToNextFrame();
        return true;
    }

    if (FAILED(hr)) {
        RTX_DIAG("DX12Device: Present #%u FAILED hr=0x%08X", s_presentCount, (uint32_t)hr);
        SPDLOG_ERROR("[RTX] Present failed: 0x{:08X}", (uint32_t)hr);

        // Provide detailed diagnostics for common DXR failure modes
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
            HRESULT reason = m_device->GetDeviceRemovedReason();
            RTX_DIAG("DX12Device: DEVICE REMOVED — reason hr=0x%08X", (uint32_t)reason);
            SPDLOG_ERROR("[RTX] Device removed reason: 0x{:08X}", (uint32_t)reason);

            const char* reasonStr = "Unknown";
            if (reason == DXGI_ERROR_DEVICE_HUNG) reasonStr = "DEVICE_HUNG (GPU took too long)";
            else if (reason == DXGI_ERROR_DEVICE_REMOVED) reasonStr = "DEVICE_REMOVED (GPU physically removed or driver crash)";
            else if (reason == DXGI_ERROR_DEVICE_RESET) reasonStr = "DEVICE_RESET (driver-level reset)";
            else if (reason == DXGI_ERROR_DRIVER_INTERNAL_ERROR) reasonStr = "DRIVER_INTERNAL_ERROR";
            else if (reason == DXGI_ERROR_INVALID_CALL) reasonStr = "INVALID_CALL (bad API usage)";
            else if (reason == S_OK) reasonStr = "S_OK (no specific reason — possible TDR)";

            RTX_DIAG("DX12Device: Device removed reason: %s", reasonStr);
            SPDLOG_ERROR("[RTX] Device removed reason: {}", reasonStr);
        }

        // Log to file for post-mortem diagnostics
        {
            FILE* nf = fopen("rtx_nuclear_test.log", "a");
            if (nf) {
                fprintf(nf, "DX12 Present #%u FAILED: hr=0x%08X\n", s_presentCount, (unsigned)hr);
                fflush(nf);
                fclose(nf);
            }
        }

        return false;
    }
    if (s_presentCount <= 10 || (s_presentCount % 300) == 0) {
        RTX_DIAG("DX12Device: Present #%u OK (frameIndex=%u, %ux%u, overlay=%s)", s_presentCount, m_frameIndex, m_width, m_height, m_usingOverlayHwnd ? "YES" : "NO");
        printf("[RTX] OUTPUT_DIAG: DX12 Present #%u OK (frameIndex=%u, %ux%u, swapChain=%p, overlay=%s)\n",
               s_presentCount, m_frameIndex, m_width, m_height, (void*)m_swapChain.Get(),
               m_usingOverlayHwnd ? "YES" : "NO");
    }

    MoveToNextFrame();
    return true;
}

void DX12Device::WaitForGPU() {
    if (!m_commandQueue || !m_fence || !m_fenceEvent) return;

    // Signal the fence with the current value
    const uint64_t fenceValue = m_fenceValues[m_frameIndex];
    HRESULT hr = m_commandQueue->Signal(m_fence.Get(), fenceValue);
    if (FAILED(hr)) {
        RTX_DIAG("DX12Device::WaitForGPU() Signal FAILED hr=0x%08X", (uint32_t)hr);
        return;
    }

    // Wait for the fence to be triggered
    hr = m_fence->SetEventOnCompletion(fenceValue, m_fenceEvent);
    if (FAILED(hr)) {
        RTX_DIAG("DX12Device::WaitForGPU() SetEventOnCompletion FAILED hr=0x%08X", (uint32_t)hr);
        return;
    }
    DWORD waitResult = WaitForSingleObjectEx(m_fenceEvent, 10000, FALSE); // 10s timeout instead of INFINITE
    if (waitResult == WAIT_TIMEOUT) {
        RTX_DIAG("DX12Device::WaitForGPU() TIMEOUT — GPU may be hung (waited 10s, fenceValue=%llu)", (unsigned long long)fenceValue);
        SPDLOG_ERROR("[RTX] WaitForGPU: 10-second timeout — GPU may be hung");
    } else if (waitResult == WAIT_FAILED) {
        RTX_DIAG("DX12Device::WaitForGPU() WaitForSingleObjectEx FAILED (GetLastError=%lu)", GetLastError());
    }

    m_fenceValues[m_frameIndex]++;
}

void DX12Device::WaitForPreviousFrame() {
    if (!m_commandQueue || !m_fence || !m_fenceEvent || !m_swapChain) return;

    // Signal the fence with the current fence value for this frame
    const uint64_t fenceVal = m_fenceValues[m_frameIndex];
    HRESULT hr = m_commandQueue->Signal(m_fence.Get(), fenceVal);
    if (FAILED(hr)) {
        RTX_DIAG("DX12Device::WaitForPreviousFrame() Signal FAILED hr=0x%08X", (uint32_t)hr);
        return;
    }

    // Increment for next use
    m_fenceValues[m_frameIndex]++;

    // Wait until the GPU has completed the previous frame's work
    if (m_fence->GetCompletedValue() < fenceVal) {
        hr = m_fence->SetEventOnCompletion(fenceVal, m_fenceEvent);
        if (FAILED(hr)) {
            RTX_DIAG("DX12Device::WaitForPreviousFrame() SetEventOnCompletion FAILED hr=0x%08X", (uint32_t)hr);
            return;
        }
        DWORD waitResult = WaitForSingleObjectEx(m_fenceEvent, 10000, FALSE);
        if (waitResult == WAIT_TIMEOUT) {
            RTX_DIAG("DX12Device::WaitForPreviousFrame() TIMEOUT — GPU may be hung");
            SPDLOG_ERROR("[RTX] WaitForPreviousFrame: 10-second timeout — GPU may be hung");
        }
    }
}

void DX12Device::FlushCommandQueue() {
    // Flush all pending GPU work by signaling and waiting on the fence.
    // This is a full GPU drain — used before operations like screenshot readback
    // that need all GPU work to be complete.
    if (!m_commandQueue || !m_fence || !m_fenceEvent) return;

    // Use the maximum of all frame fence values + 1 to ensure all work is done
    uint64_t maxFenceValue = 0;
    for (uint32_t i = 0; i < BACK_BUFFER_COUNT; i++) {
        if (m_fenceValues[i] > maxFenceValue) {
            maxFenceValue = m_fenceValues[i];
        }
    }
    maxFenceValue++;

    HRESULT hr = m_commandQueue->Signal(m_fence.Get(), maxFenceValue);
    if (FAILED(hr)) {
        RTX_DIAG("DX12Device::FlushCommandQueue() Signal FAILED hr=0x%08X", (uint32_t)hr);
        return;
    }

    if (m_fence->GetCompletedValue() < maxFenceValue) {
        hr = m_fence->SetEventOnCompletion(maxFenceValue, m_fenceEvent);
        if (FAILED(hr)) {
            RTX_DIAG("DX12Device::FlushCommandQueue() SetEventOnCompletion FAILED hr=0x%08X", (uint32_t)hr);
            return;
        }
        DWORD waitResult = WaitForSingleObjectEx(m_fenceEvent, 15000, FALSE);
        if (waitResult == WAIT_TIMEOUT) {
            RTX_DIAG("DX12Device::FlushCommandQueue() TIMEOUT — GPU may be hung (waited 15s)");
            SPDLOG_ERROR("[RTX] FlushCommandQueue: 15-second timeout — GPU may be hung");
        }
    }

    // Update all frame fence values to reflect that all work is complete
    for (uint32_t i = 0; i < BACK_BUFFER_COUNT; i++) {
        m_fenceValues[i] = maxFenceValue + 1;
    }
}

void DX12Device::MoveToNextFrame() {
    if (!m_commandQueue || !m_fence || !m_swapChain) return;

    // Schedule a signal command in the queue for the current frame
    const uint64_t currentFenceValue = m_fenceValues[m_frameIndex];
    HRESULT hr = m_commandQueue->Signal(m_fence.Get(), currentFenceValue);
    if (FAILED(hr)) {
        RTX_DIAG("DX12Device::MoveToNextFrame() Signal FAILED hr=0x%08X", (uint32_t)hr);
        return;
    }

    // Advance to the next frame
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

    // If the next frame is not ready to be rendered yet, wait until it is
    if (m_fence->GetCompletedValue() < m_fenceValues[m_frameIndex]) {
        hr = m_fence->SetEventOnCompletion(m_fenceValues[m_frameIndex], m_fenceEvent);
        if (FAILED(hr)) {
            RTX_DIAG("DX12Device::MoveToNextFrame() SetEventOnCompletion FAILED hr=0x%08X", (uint32_t)hr);
            return;
        }
        DWORD waitResult = WaitForSingleObjectEx(m_fenceEvent, 10000, FALSE); // 10s timeout
        if (waitResult == WAIT_TIMEOUT) {
            RTX_DIAG("DX12Device::MoveToNextFrame() TIMEOUT — GPU may be hung");
            SPDLOG_ERROR("[RTX] MoveToNextFrame: 10-second timeout — GPU may be hung");
        }
    }

    m_fenceValues[m_frameIndex] = currentFenceValue + 1;
}

void DX12Device::OnResize(uint32_t width, uint32_t height) {
    RTX_DIAG("DX12Device::OnResize() %ux%u (current: %ux%u, initialized=%s)",
             width, height, m_width, m_height, m_initialized ? "yes" : "no");
    if (!m_initialized || (width == m_width && height == m_height)) return;

    // If using overlay child window, resize it to match parent's client area
    if (m_usingOverlayHwnd && m_overlayHwnd && IsWindow(m_overlayHwnd)) {
        SetWindowPos(m_overlayHwnd, HWND_TOP, 0, 0, (int)width, (int)height, SWP_SHOWWINDOW);
        RTX_DIAG("DX12Device::OnResize() overlay child resized to %ux%u", width, height);
    }

    // Clamp to reasonable minimum to prevent zero-sized swap chain
    if (width < 1 || height < 1) {
        RTX_DIAG("DX12Device::OnResize() SKIPPED — invalid dimensions %ux%u", width, height);
        return;
    }

    WaitForGPU();

    // Release back buffer references
    for (UINT i = 0; i < BACK_BUFFER_COUNT; i++) {
        m_renderTargets[i].Reset();
        m_fenceValues[i] = m_fenceValues[m_frameIndex];
    }

    HRESULT hr = m_swapChain->ResizeBuffers(
        BACK_BUFFER_COUNT, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, 0
    );
    if (FAILED(hr)) {
        RTX_DIAG("DX12Device::OnResize() ResizeBuffers FAILED hr=0x%08X", (uint32_t)hr);
        SPDLOG_ERROR("[RTX] Failed to resize swap chain: 0x{:08X}", (uint32_t)hr);
        return;
    }

    m_width = width;
    m_height = height;
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

    if (!CreateRenderTargetViews()) {
        RTX_DIAG("DX12Device::OnResize() CreateRenderTargetViews FAILED after resize to %ux%u", width, height);
        SPDLOG_ERROR("[RTX] Failed to create RTVs after resize to {}x{}", width, height);
        return;
    }
    RTX_DIAG("DX12Device::OnResize() SUCCESS — resized to %ux%u", width, height);
    SPDLOG_INFO("[RTX] Resized to {}x{}", width, height);
}

void DX12Device::ResizeSwapChain(uint32_t width, uint32_t height) {
    RTX_DIAG("DX12Device::ResizeSwapChain() %ux%u (current: %ux%u)", width, height, m_width, m_height);
    if (!m_swapChain) {
        RTX_DIAG("DX12Device::ResizeSwapChain() SKIPPED — null swap chain");
        return;
    }
    if (!m_initialized) {
        RTX_DIAG("DX12Device::ResizeSwapChain() SKIPPED — not initialized");
        return;
    }
    if (width < 1 || height < 1) {
        RTX_DIAG("DX12Device::ResizeSwapChain() SKIPPED — invalid dimensions %ux%u", width, height);
        return;
    }
    if (width == m_width && height == m_height) {
        RTX_DIAG("DX12Device::ResizeSwapChain() SKIPPED — dimensions unchanged");
        return;
    }

    // Delegate to OnResize which already handles the full resize flow
    OnResize(width, height);
}

D3D12_CPU_DESCRIPTOR_HANDLE DX12Device::GetCurrentRTVHandle() const {
    if (!m_rtvHeap) {
        OutputDebugStringA("[RTX] CRITICAL: GetCurrentRTVHandle() called with null RTV heap!\n");
        D3D12_CPU_DESCRIPTOR_HANDLE nullHandle = {};
        nullHandle.ptr = 0;
        return nullHandle;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += m_frameIndex * m_rtvDescriptorSize;
    return handle;
}

bool DX12Device::CheckDeviceHealth() const {
    if (!m_device) return false;

    HRESULT reason = m_device->GetDeviceRemovedReason();
    if (reason == S_OK) return true; // Device is healthy

    const char* reasonStr = "Unknown";
    if (reason == DXGI_ERROR_DEVICE_HUNG) reasonStr = "DEVICE_HUNG (GPU command took too long — possible TDR)";
    else if (reason == DXGI_ERROR_DEVICE_REMOVED) reasonStr = "DEVICE_REMOVED (GPU physically removed or driver crash)";
    else if (reason == DXGI_ERROR_DEVICE_RESET) reasonStr = "DEVICE_RESET (driver-level reset)";
    else if (reason == DXGI_ERROR_DRIVER_INTERNAL_ERROR) reasonStr = "DRIVER_INTERNAL_ERROR";
    else if (reason == DXGI_ERROR_INVALID_CALL) reasonStr = "INVALID_CALL (bad DX12 API usage — check root signatures, descriptors, buffer sizes)";

    RTX_DIAG("DX12Device::CheckDeviceHealth() FAILED — device removed reason: 0x%08X (%s)", (uint32_t)reason, reasonStr);
    SPDLOG_ERROR("[RTX] Device health check FAILED: 0x{:08X} ({})", (uint32_t)reason, reasonStr);
    return false;
}

} // namespace RTX

#endif // ENABLE_DX12_RTX
