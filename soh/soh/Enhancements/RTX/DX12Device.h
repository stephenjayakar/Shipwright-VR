#pragma once
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

    // Two-phase initialization for safe DX11->DX12 handoff.
    // ProbeDevice() creates the DX12 device and checks raytracing support
    // WITHOUT creating a swap chain. This allows verifying DXR capability
    // before tearing down the DX11 swap chain.
    // Returns true if DX12 + DXR are available on this system.
    bool ProbeDevice();
    // CompleteInitialization() creates the swap chain and remaining resources.
    // Must be called after ProbeDevice() returns true AND after the DX11 swap
    // chain has been released. The HWND must be free of any other swap chain.
    bool CompleteInitialization(HWND hwnd, uint32_t width, uint32_t height);

    // Frame lifecycle
    void BeginFrame();
    void EndFrame();
    bool Present();

    // GPU synchronization
    void WaitForGPU();
    void WaitForPreviousFrame();
    void FlushCommandQueue();
    void MoveToNextFrame();

    // Resize
    void OnResize(uint32_t width, uint32_t height);
    void ResizeSwapChain(uint32_t width, uint32_t height);

    // Shared swap chain support (DX12 Bridge)
    // Called from DX12Bridge.cpp when gfx_dxgi.cpp creates a swap chain using
    // our DX12 command queue. The swap chain is "shared" — owned by gfx_dxgi
    // but rendered into by DX12.
    void SetSharedSwapChain(IDXGISwapChain1* swapChain);
    
    // Complete initialization using the shared swap chain (no CreateSwapChain call).
    // This is used in the bridge path where the swap chain comes from gfx_dxgi.
    bool CompleteInitializationWithSharedSwapChain(uint32_t width, uint32_t height);
    
    // Returns the shared swap chain (as IDXGISwapChain3, QI'd from the IDXGISwapChain1)
    IDXGISwapChain3* GetSharedSwapChain() const { return m_swapChain.Get(); }
    bool HasSharedSwapChain() const { return m_hasSharedSwapChain; }

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
    uint32_t GetUAVDescriptorSize() const { return m_uavDescriptorSize; }

    uint32_t GetWidth() const { return m_width; }
    uint32_t GetHeight() const { return m_height; }
    uint32_t GetFrameIndex() const { return m_frameIndex; }
    uint32_t GetCurrentBackBufferIndex() const { return m_frameIndex; }
    ID3D12Fence* GetFence() const { return m_fence.Get(); }
    HANDLE GetFenceEvent() const { return m_fenceEvent; }
    uint64_t GetCurrentFenceValue() const { return m_fenceValues[m_frameIndex]; }

    bool IsInitialized() const { return m_initialized; }
    bool SupportsRaytracing() const { return m_raytracingSupported; }

    // Diagnostic: check if the device has been removed (TDR, driver crash, etc.)
    // and log a detailed reason. Returns true if the device is still healthy.
    bool CheckDeviceHealth() const;

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
    static const uint32_t BACK_BUFFER_COUNT = 3;  // Match gfx_dxgi's buffer count
    ComPtr<ID3D12CommandAllocator> m_commandAllocators[BACK_BUFFER_COUNT];
    ComPtr<ID3D12GraphicsCommandList4> m_commandList;

    // Swap chain
    ComPtr<IDXGISwapChain3> m_swapChain;
    ComPtr<ID3D12Resource> m_renderTargets[BACK_BUFFER_COUNT];
    uint32_t m_frameIndex = 0;
    bool m_hasSharedSwapChain = false;  // True when swap chain came from DX12 bridge

    // Descriptor heaps
    ComPtr<ID3D12DescriptorHeap> m_rtvHeap;   // Render target views (BACK_BUFFER_COUNT descriptors)
    ComPtr<ID3D12DescriptorHeap> m_srvHeap;   // Shader resource views (1024, shader-visible)
    // NOTE: The UAV heap below is currently unused because DX12 only allows one
    // CBV_SRV_UAV heap bound at a time, and all UAV descriptors are placed in the
    // TextureManager's SRV heap (which is bound during DispatchRays). This heap is
    // retained as a fallback for non-TextureManager usage paths.
    ComPtr<ID3D12DescriptorHeap> m_uavHeap;   // Unordered access views (16, shader-visible) [see note above]
    uint32_t m_rtvDescriptorSize = 0;
    uint32_t m_srvDescriptorSize = 0;
    uint32_t m_uavDescriptorSize = 0;

    // Synchronization
    ComPtr<ID3D12Fence> m_fence;
    HANDLE m_fenceEvent = nullptr;
    uint64_t m_fenceValues[BACK_BUFFER_COUNT] = {};

    // Window
    HWND m_hwnd = nullptr;           // Game's main HWND (parent)
    HWND m_overlayHwnd = nullptr;    // Child overlay HWND for DX12 swap chain (avoids DX11 conflict)
    uint32_t m_width = 0;
    uint32_t m_height = 0;

    // Helper: create or get the overlay child window for the DX12 swap chain.
    // If the parent HWND already has a DX11 swap chain, we can't create a
    // DX12 swap chain on it. Instead, create a WS_CHILD window that covers
    // the entire client area and use THAT for the DX12 swap chain.
    HWND GetOrCreateOverlayHwnd(HWND parentHwnd);

    // State
    bool m_initialized = false;
    bool m_probed = false;      // True after ProbeDevice() succeeds (device + DXR check done)
    bool m_raytracingSupported = false;
    bool m_usingOverlayHwnd = false; // True if we created a child overlay for swap chain
};

} // namespace RTX

#endif // ENABLE_DX12_RTX
#endif // DX12_DEVICE_H
