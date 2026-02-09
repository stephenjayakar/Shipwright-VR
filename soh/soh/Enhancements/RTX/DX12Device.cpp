#ifdef ENABLE_DX12_RTX

#include "DX12Device.h"
#include "RTXDiagLog.h"
#include <cassert>
#include <cstdio>
#include <spdlog/spdlog.h>

// Link libraries
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

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

    m_probed = true;
    SPDLOG_INFO("[RTX] Probe succeeded: DX12 + DXR available");
    RTX_DIAG("DX12Device::ProbeDevice() SUCCESS - DX12 + DXR available");
    return true;
}

bool DX12Device::CompleteInitialization(HWND hwnd, uint32_t width, uint32_t height) {
    printf("[RTX] DX12Device::Initialize() called (two-phase complete, %ux%u, hwnd=%p)\n", width, height, (void*)hwnd);
    RTX_DIAG("DX12Device::CompleteInitialization() hwnd=%p, %ux%u", (void*)hwnd, width, height);
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

    RTX_DIAG("DX12Device: Creating command queue...");
    if (!CreateCommandQueue()) { RTX_DIAG("DX12Device: Command queue creation FAILED"); return false; }
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
    printf("[RTX] DX12 Device created successfully (two-phase, %ux%u)\n", width, height);
    RTX_DIAG("[DIAG] DX12Device::CompleteInitialization SUCCESS - Device fully initialized (%ux%u), DXR=%s",
             width, height, m_raytracingSupported ? "supported" : "NOT supported");
    SPDLOG_INFO("[RTX] DX12 device fully initialized ({}x{})", width, height);
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
    m_swapChain.Reset();
    m_commandQueue.Reset();
    m_device.Reset();
    m_adapter.Reset();
    m_factory.Reset();

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

bool DX12Device::CreateSwapChain(HWND hwnd) {
    RTX_DIAG("DX12Device::CreateSwapChain() hwnd=%p, %ux%u, buffers=%u", (void*)hwnd, m_width, m_height, BACK_BUFFER_COUNT);
    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
    swapChainDesc.BufferCount = BACK_BUFFER_COUNT;
    swapChainDesc.Width = m_width;
    swapChainDesc.Height = m_height;
    swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.SampleDesc.Count = 1;

    ComPtr<IDXGISwapChain1> swapChain1;
    HRESULT hr = m_factory->CreateSwapChainForHwnd(
        m_commandQueue.Get(),
        hwnd,
        &swapChainDesc,
        nullptr,
        nullptr,
        &swapChain1
    );
    if (FAILED(hr)) { printf("[RTX] DX12 FAILED: 0x%08X at line %d\n", (unsigned)hr, __LINE__); }

    if (FAILED(hr)) {
        RTX_DIAG("DX12Device::CreateSwapChain() CreateSwapChainForHwnd FAILED hr=0x%08X", (uint32_t)hr);
        SPDLOG_ERROR("[RTX] Failed to create swap chain: 0x{:08X}", (uint32_t)hr);
        return false;
    }
    RTX_DIAG("DX12Device::CreateSwapChain() CreateSwapChainForHwnd OK");

    // Disable ALT+ENTER fullscreen toggle (game handles its own fullscreen)
    m_factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

    hr = swapChain1.As(&m_swapChain);
    if (FAILED(hr)) {
        RTX_DIAG("DX12Device::CreateSwapChain() IDXGISwapChain3 query FAILED hr=0x%08X", (uint32_t)hr);
        SPDLOG_ERROR("[RTX] Failed to query IDXGISwapChain3: 0x{:08X}", (uint32_t)hr);
        return false;
    }

    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
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
    // Reset command allocator and command list for this frame
    m_commandAllocators[m_frameIndex]->Reset();
    m_commandList->Reset(m_commandAllocators[m_frameIndex].Get(), nullptr);
}

void DX12Device::EndFrame() {
    // Transition back buffer to present state
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = m_renderTargets[m_frameIndex].Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_commandList->ResourceBarrier(1, &barrier);

    // Close and execute the command list
    m_commandList->Close();
    ID3D12CommandList* ppCommandLists[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(1, ppCommandLists);
}

bool DX12Device::Present() {
    static uint32_t s_presentCount = 0;
    s_presentCount++;
    HRESULT hr = m_swapChain->Present(1, 0); // VSync on
    if (FAILED(hr)) {
        RTX_DIAG("DX12Device: Present #%u FAILED hr=0x%08X", s_presentCount, (uint32_t)hr);
        SPDLOG_ERROR("[RTX] Present failed: 0x{:08X}", (uint32_t)hr);
        return false;
    }
    if (s_presentCount <= 5 || (s_presentCount % 300) == 0) {
        RTX_DIAG("DX12Device: Present #%u OK", s_presentCount);
    }

    MoveToNextFrame();
    return true;
}

void DX12Device::WaitForGPU() {
    if (!m_commandQueue || !m_fence || !m_fenceEvent) return;

    // Signal the fence with the current value
    const uint64_t fenceValue = m_fenceValues[m_frameIndex];
    m_commandQueue->Signal(m_fence.Get(), fenceValue);

    // Wait for the fence to be triggered
    m_fence->SetEventOnCompletion(fenceValue, m_fenceEvent);
    WaitForSingleObjectEx(m_fenceEvent, INFINITE, FALSE);

    m_fenceValues[m_frameIndex]++;
}

void DX12Device::MoveToNextFrame() {
    // Schedule a signal command in the queue for the current frame
    const uint64_t currentFenceValue = m_fenceValues[m_frameIndex];
    m_commandQueue->Signal(m_fence.Get(), currentFenceValue);

    // Advance to the next frame
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

    // If the next frame is not ready to be rendered yet, wait until it is
    if (m_fence->GetCompletedValue() < m_fenceValues[m_frameIndex]) {
        m_fence->SetEventOnCompletion(m_fenceValues[m_frameIndex], m_fenceEvent);
        WaitForSingleObjectEx(m_fenceEvent, INFINITE, FALSE);
    }

    m_fenceValues[m_frameIndex] = currentFenceValue + 1;
}

void DX12Device::OnResize(uint32_t width, uint32_t height) {
    RTX_DIAG("DX12Device::OnResize() %ux%u (current: %ux%u, initialized=%s)",
             width, height, m_width, m_height, m_initialized ? "yes" : "no");
    if (!m_initialized || (width == m_width && height == m_height)) return;

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
        SPDLOG_ERROR("[RTX] Failed to resize swap chain: 0x{:08X}", (uint32_t)hr);
        return;
    }

    m_width = width;
    m_height = height;
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

    CreateRenderTargetViews();
    SPDLOG_INFO("[RTX] Resized to {}x{}", width, height);
}

D3D12_CPU_DESCRIPTOR_HANDLE DX12Device::GetCurrentRTVHandle() const {
    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += m_frameIndex * m_rtvDescriptorSize;
    return handle;
}

} // namespace RTX

#endif // ENABLE_DX12_RTX
