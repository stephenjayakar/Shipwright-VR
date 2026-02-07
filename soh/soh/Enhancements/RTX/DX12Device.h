#ifndef DX12_DEVICE_H
#define DX12_DEVICE_H

#ifdef ENABLE_DX12_RTX

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdint>

using Microsoft::WRL::ComPtr;

namespace RTX {

class DX12Device {
public:
    DX12Device();
    ~DX12Device();

    // Lifecycle
    bool Initialize(HWND hwnd, uint32_t width, uint32_t height);
    void Shutdown();

    // Frame lifecycle
    void BeginFrame();
    void EndFrame();
    bool Present();

    // GPU synchronization
    void WaitForGPU();
    void MoveToNextFrame();

    // Resize
    void OnResize(uint32_t width, uint32_t height);

    // Getters
    ID3D12Device5* GetDevice() const { return m_device.Get(); }
    ID3D12CommandQueue* GetCommandQueue() const { return m_commandQueue.Get(); }
    ID3D12GraphicsCommandList4* GetCommandList() const { return m_commandList.Get(); }
    ID3D12CommandAllocator* GetCurrentCommandAllocator() const { return m_commandAllocators[m_frameIndex].Get(); }
    IDXGISwapChain3* GetSwapChain() const { return m_swapChain.Get(); }
    ID3D12Resource* GetCurrentBackBuffer() const { return m_renderTargets[m_frameIndex].Get(); }
    D3D12_CPU_DESCRIPTOR_HANDLE GetCurrentRTVHandle() const;

    ID3D12DescriptorHeap* GetSRVHeap() const { return m_srvHeap.Get(); }
    ID3D12DescriptorHeap* GetUAVHeap() const { return m_uavHeap.Get(); }
    ID3D12DescriptorHeap* GetRTVHeap() const { return m_rtvHeap.Get(); }

    uint32_t GetSRVDescriptorSize() const { return m_srvDescriptorSize; }
    uint32_t GetRTVDescriptorSize() const { return m_rtvDescriptorSize; }

    uint32_t GetWidth() const { return m_width; }
    uint32_t GetHeight() const { return m_height; }
    uint32_t GetFrameIndex() const { return m_frameIndex; }

    bool IsInitialized() const { return m_initialized; }
    bool SupportsRaytracing() const { return m_raytracingSupported; }

private:
    // Initialization helpers
    bool CreateDevice();
    bool CreateCommandQueue();
    bool CreateSwapChain(HWND hwnd);
    bool CreateDescriptorHeaps();
    bool CreateRenderTargetViews();
    bool CreateCommandAllocatorsAndList();
    bool CreateFence();
    bool CheckRaytracingSupport();

    // Device
    ComPtr<IDXGIFactory6> m_factory;
    ComPtr<IDXGIAdapter1> m_adapter;
    ComPtr<ID3D12Device5> m_device;

    // Command infrastructure
    ComPtr<ID3D12CommandQueue> m_commandQueue;
    static const uint32_t BACK_BUFFER_COUNT = 2;
    ComPtr<ID3D12CommandAllocator> m_commandAllocators[BACK_BUFFER_COUNT];
    ComPtr<ID3D12GraphicsCommandList4> m_commandList;

    // Swap chain
    ComPtr<IDXGISwapChain3> m_swapChain;
    ComPtr<ID3D12Resource> m_renderTargets[BACK_BUFFER_COUNT];
    uint32_t m_frameIndex = 0;

    // Descriptor heaps
    ComPtr<ID3D12DescriptorHeap> m_rtvHeap;   // Render target views (2 descriptors)
    ComPtr<ID3D12DescriptorHeap> m_srvHeap;   // Shader resource views (1024, shader-visible)
    ComPtr<ID3D12DescriptorHeap> m_uavHeap;   // Unordered access views (16, shader-visible)
    uint32_t m_rtvDescriptorSize = 0;
    uint32_t m_srvDescriptorSize = 0;

    // Synchronization
    ComPtr<ID3D12Fence> m_fence;
    HANDLE m_fenceEvent = nullptr;
    uint64_t m_fenceValues[BACK_BUFFER_COUNT] = {};

    // Window
    HWND m_hwnd = nullptr;
    uint32_t m_width = 0;
    uint32_t m_height = 0;

    // State
    bool m_initialized = false;
    bool m_raytracingSupported = false;
};

} // namespace RTX

#endif // ENABLE_DX12_RTX
#endif // DX12_DEVICE_H
