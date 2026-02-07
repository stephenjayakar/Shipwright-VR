#ifdef ENABLE_DX12_RTX

#include "DX12Device.h"
#include <cassert>
#include <spdlog/spdlog.h>

// Link libraries
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

namespace RTX {

DX12Device::DX12Device() = default;

DX12Device::~DX12Device() {
    Shutdown();
}

bool DX12Device::Initialize(HWND hwnd, uint32_t width, uint32_t height) {
    if (m_initialized) {
        return true;
    }

    m_hwnd = hwnd;
    m_width = width;
    m_height = height;

    if (!CreateDevice()) return false;
    if (!CheckRaytracingSupport()) return false;
    if (!CreateCommandQueue()) return false;
    if (!CreateSwapChain(hwnd)) return false;
    if (!CreateDescriptorHeaps()) return false;
    if (!CreateRenderTargetViews()) return false;
    if (!CreateCommandAllocatorsAndList()) return false;
    if (!CreateFence()) return false;

    m_initialized = true;
    SPDLOG_INFO("[RTX] DX12 device initialized successfully ({}x{})", width, height);
    return true;
}

void DX12Device::Shutdown() {
    if (!m_initialized) return;

    WaitForGPU();

    if (m_fenceEvent) {
        CloseHandle(m_fenceEvent);
        m_fenceEvent = nullptr;
    }

    m_initialized = false;
    SPDLOG_INFO("[RTX] DX12 device shut down");
}

bool DX12Device::CreateDevice() {
    UINT dxgiFactoryFlags = 0;

#ifdef _DEBUG
    // Enable debug layer in debug builds
    ComPtr<ID3D12Debug> debugController;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
        debugController->EnableDebugLayer();
        dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
        SPDLOG_INFO("[RTX] DX12 debug layer enabled");
    }
#endif

    HRESULT hr = CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(&m_factory));
    if (FAILED(hr)) {
        SPDLOG_ERROR("[RTX] Failed to create DXGI factory: 0x{:08X}", (uint32_t)hr);
        return false;
    }

    // Enumerate adapters, prefer discrete GPU with DXR support
    ComPtr<IDXGIAdapter1> adapter;
    bool adapterFound = false;

    for (UINT i = 0; m_factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);

        // Skip software adapters
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;

        // Check if adapter supports D3D12 with feature level 12.0
        hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&m_device));
        if (SUCCEEDED(hr)) {
            m_adapter = adapter;
            adapterFound = true;
            SPDLOG_INFO("[RTX] Using adapter: {} (VRAM: {} MB)",
                        std::string(desc.Description, desc.Description + wcslen(desc.Description)),
                        desc.DedicatedVideoMemory / (1024 * 1024));
            break;
        }
    }

    if (!adapterFound) {
        SPDLOG_ERROR("[RTX] No DX12-capable adapter found");
        return false;
    }

    return true;
}

bool DX12Device::CheckRaytracingSupport() {
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5 = {};
    HRESULT hr = m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5));

    if (FAILED(hr) || options5.RaytracingTier < D3D12_RAYTRACING_TIER_1_0) {
        SPDLOG_ERROR("[RTX] Raytracing not supported on this device (tier: {})",
                     (int)options5.RaytracingTier);
        m_raytracingSupported = false;
        return false;
    }

    m_raytracingSupported = true;
    SPDLOG_INFO("[RTX] Raytracing tier: {}", (int)options5.RaytracingTier);
    return true;
}

bool DX12Device::CreateCommandQueue() {
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;

    HRESULT hr = m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_commandQueue));
    if (FAILED(hr)) {
        SPDLOG_ERROR("[RTX] Failed to create command queue: 0x{:08X}", (uint32_t)hr);
        return false;
    }

    return true;
}

bool DX12Device::CreateSwapChain(HWND hwnd) {
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

    if (FAILED(hr)) {
        SPDLOG_ERROR("[RTX] Failed to create swap chain: 0x{:08X}", (uint32_t)hr);
        return false;
    }

    // Disable ALT+ENTER fullscreen toggle (game handles its own fullscreen)
    m_factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

    hr = swapChain1.As(&m_swapChain);
    if (FAILED(hr)) {
        SPDLOG_ERROR("[RTX] Failed to query IDXGISwapChain3: 0x{:08X}", (uint32_t)hr);
        return false;
    }

    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
    return true;
}

bool DX12Device::CreateDescriptorHeaps() {
    // RTV heap: 2 descriptors for double-buffered back buffers
    {
        D3D12_DESCRIPTOR_HEAP_DESC desc = {};
        desc.NumDescriptors = BACK_BUFFER_COUNT;
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

        HRESULT hr = m_device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_rtvHeap));
        if (FAILED(hr)) {
            SPDLOG_ERROR("[RTX] Failed to create RTV heap: 0x{:08X}", (uint32_t)hr);
            return false;
        }

        m_rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    }

    // SRV heap: 1024 descriptors for textures (shader-visible)
    {
        D3D12_DESCRIPTOR_HEAP_DESC desc = {};
        desc.NumDescriptors = 1024;
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

        HRESULT hr = m_device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_srvHeap));
        if (FAILED(hr)) {
            SPDLOG_ERROR("[RTX] Failed to create SRV heap: 0x{:08X}", (uint32_t)hr);
            return false;
        }

        m_srvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }

    // UAV heap: 16 descriptors for output buffers + GI accumulation (shader-visible)
    {
        D3D12_DESCRIPTOR_HEAP_DESC desc = {};
        desc.NumDescriptors = 16;
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

        HRESULT hr = m_device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_uavHeap));
        if (FAILED(hr)) {
            SPDLOG_ERROR("[RTX] Failed to create UAV heap: 0x{:08X}", (uint32_t)hr);
            return false;
        }

        m_uavDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }

    return true;
}

bool DX12Device::CreateRenderTargetViews() {
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();

    for (UINT i = 0; i < BACK_BUFFER_COUNT; i++) {
        HRESULT hr = m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_renderTargets[i]));
        if (FAILED(hr)) {
            SPDLOG_ERROR("[RTX] Failed to get swap chain buffer {}: 0x{:08X}", i, (uint32_t)hr);
            return false;
        }

        m_device->CreateRenderTargetView(m_renderTargets[i].Get(), nullptr, rtvHandle);
        rtvHandle.ptr += m_rtvDescriptorSize;
    }

    return true;
}

bool DX12Device::CreateCommandAllocatorsAndList() {
    for (UINT i = 0; i < BACK_BUFFER_COUNT; i++) {
        HRESULT hr = m_device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&m_commandAllocators[i])
        );
        if (FAILED(hr)) {
            SPDLOG_ERROR("[RTX] Failed to create command allocator {}: 0x{:08X}", i, (uint32_t)hr);
            return false;
        }
    }

    HRESULT hr = m_device->CreateCommandList(
        0,
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        m_commandAllocators[m_frameIndex].Get(),
        nullptr,
        IID_PPV_ARGS(&m_commandList)
    );
    if (FAILED(hr)) {
        SPDLOG_ERROR("[RTX] Failed to create command list: 0x{:08X}", (uint32_t)hr);
        return false;
    }

    // Command list is created in recording state; close it so we can reset it per frame
    m_commandList->Close();
    return true;
}

bool DX12Device::CreateFence() {
    HRESULT hr = m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence));
    if (FAILED(hr)) {
        SPDLOG_ERROR("[RTX] Failed to create fence: 0x{:08X}", (uint32_t)hr);
        return false;
    }

    m_fenceValues[m_frameIndex] = 1;

    m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!m_fenceEvent) {
        SPDLOG_ERROR("[RTX] Failed to create fence event");
        return false;
    }

    // Wait for the GPU to complete any initial work
    WaitForGPU();
    return true;
}

void DX12Device::BeginFrame() {
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
    HRESULT hr = m_swapChain->Present(1, 0); // VSync on
    if (FAILED(hr)) {
        SPDLOG_ERROR("[RTX] Present failed: 0x{:08X}", (uint32_t)hr);
        return false;
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
