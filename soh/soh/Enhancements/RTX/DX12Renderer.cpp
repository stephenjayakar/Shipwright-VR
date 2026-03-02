#ifdef ENABLE_DX12_RTX

#include "DX12Renderer.h"
#include <spdlog/spdlog.h>

namespace RTX {

DX12Renderer::DX12Renderer() = default;

DX12Renderer::~DX12Renderer() {
    Shutdown();
}

bool DX12Renderer::Initialize(DX12Device* device) {
    if (!device) {
        SPDLOG_ERROR("[RTX] DX12Renderer::Initialize: null device");
        return false;
    }
    m_device = device;
    m_initialized = true;
    SPDLOG_INFO("[RTX] DX12Renderer initialized");
    return true;
}

void DX12Renderer::Shutdown() {
    m_device = nullptr;
    m_initialized = false;
}

void DX12Renderer::TransitionResource(ID3D12GraphicsCommandList4* cmdList,
                                       ID3D12Resource* resource,
                                       D3D12_RESOURCE_STATES before,
                                       D3D12_RESOURCE_STATES after) {
    if (!cmdList || !resource) return;

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmdList->ResourceBarrier(1, &barrier);
}

void DX12Renderer::CopyOutputToBackBuffer(ID3D12GraphicsCommandList4* cmdList,
                                           ID3D12Resource* outputBuffer,
                                           ID3D12Resource* backBuffer) {
    if (!cmdList || !outputBuffer || !backBuffer) return;

    // Transition output from UAV to copy source
    D3D12_RESOURCE_BARRIER barriers[2] = {};
    barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[0].Transition.pResource = outputBuffer;
    barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    // Transition back buffer from render target to copy dest
    barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[1].Transition.pResource = backBuffer;
    barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    cmdList->ResourceBarrier(2, barriers);
    cmdList->CopyResource(backBuffer, outputBuffer);

    // Transition back buffer to present, output back to UAV
    D3D12_RESOURCE_BARRIER postBarriers[2] = {};
    postBarriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    postBarriers[0].Transition.pResource = backBuffer;
    postBarriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    postBarriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    postBarriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    postBarriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    postBarriers[1].Transition.pResource = outputBuffer;
    postBarriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    postBarriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    postBarriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    cmdList->ResourceBarrier(2, postBarriers);
}

void DX12Renderer::UAVBarrier(ID3D12GraphicsCommandList4* cmdList, ID3D12Resource* resource) {
    if (!cmdList) return;

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = resource;
    cmdList->ResourceBarrier(1, &barrier);
}

void DX12Renderer::BindSRVHeap(ID3D12GraphicsCommandList4* cmdList, ID3D12DescriptorHeap* heap) {
    if (!cmdList || !heap) return;

    ID3D12DescriptorHeap* heaps[] = { heap };
    cmdList->SetDescriptorHeaps(1, heaps);
}

void DX12Renderer::Present(DX12Device* device) {
    if (device) {
        device->Present();
    }
}

} // namespace RTX

#endif // ENABLE_DX12_RTX
