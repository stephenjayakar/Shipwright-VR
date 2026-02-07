#pragma once
#ifndef DX12_RENDERER_H
#define DX12_RENDERER_H

#ifdef ENABLE_DX12_RTX

#include "DX12Device.h"
#include "DXRPipeline.h"
#include "RTXTypes.h"
#include <cstdint>
#include <memory>

namespace RTX {

/**
 * DX12Renderer - Low-level DX12 rendering utilities.
 *
 * Encapsulates common DX12 rendering patterns such as resource transitions,
 * descriptor heap management, and present logic that are shared across
 * the RTX subsystem.  RTXRenderer delegates low-level GPU work here.
 */
class DX12Renderer {
public:
    DX12Renderer();
    ~DX12Renderer();

    bool Initialize(DX12Device* device);
    void Shutdown();

    // Transition a resource between states
    void TransitionResource(ID3D12GraphicsCommandList4* cmdList,
                            ID3D12Resource* resource,
                            D3D12_RESOURCE_STATES before,
                            D3D12_RESOURCE_STATES after);

    // Copy a UAV output buffer to the back buffer and transition for present
    void CopyOutputToBackBuffer(ID3D12GraphicsCommandList4* cmdList,
                                ID3D12Resource* outputBuffer,
                                ID3D12Resource* backBuffer);

    // Issue a UAV barrier
    void UAVBarrier(ID3D12GraphicsCommandList4* cmdList, ID3D12Resource* resource);

    // Set the SRV descriptor heap on a command list
    void BindSRVHeap(ID3D12GraphicsCommandList4* cmdList, ID3D12DescriptorHeap* heap);

    // Present the current frame
    void Present(DX12Device* device);

private:
    DX12Device* m_device = nullptr;
    bool m_initialized = false;
};

} // namespace RTX

#endif // ENABLE_DX12_RTX
#endif // DX12_RENDERER_H
