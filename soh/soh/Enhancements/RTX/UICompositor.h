#pragma once
#ifndef UI_COMPOSITOR_H
#define UI_COMPOSITOR_H

#ifdef ENABLE_DX12_RTX

#include "DX12Device.h"
#include "RTXTypes.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <vector>
#include <mutex>

using Microsoft::WRL::ComPtr;

namespace RTX {

// ============================================================================
// CompositeConstants - Matches the cbuffer in Composite.hlsl
// ============================================================================
struct CompositeConstants {
    uint32_t resolutionX;     // Output width
    uint32_t resolutionY;     // Output height
    float    uiOpacity;       // Global UI opacity (0.0 - 1.0)
    uint32_t flags;           // Bit 0 = UI enabled, Bit 1 = debug outline
};

// ============================================================================
// UICompositor - Manages the DX12 UI overlay texture and composite pipeline.
//
// The game's HUD (hearts, rupees, minimap, text) is rendered by the existing
// DX11 Fast3D interpreter to an offscreen render target. The pixel data is
// read back to CPU memory and uploaded to a DX12 texture. A compute shader
// then alpha-blends this UI overlay on top of the raytraced scene.
//
// Lifecycle:
//   1. Initialize() - Creates DX12 resources (texture, upload buffer, pipeline)
//   2. UploadUIFrame() - Called each frame with UI pixel data from DX11 readback
//   3. Composite() - Dispatches the composite compute shader
//   4. Shutdown() - Releases all resources
// ============================================================================
class UICompositor {
public:
    UICompositor();
    ~UICompositor();

    // Initialize the compositor. Creates the UI texture, upload buffer,
    // compute pipeline, and descriptor entries.
    // Must be called after DXRPipeline::CreateUAVDescriptors() so that
    // the TextureManager's SRV heap is available for descriptor placement.
    bool Initialize(DX12Device* device, uint32_t width, uint32_t height);

    // Shutdown and release all DX12 resources.
    void Shutdown();

    // Upload UI frame pixel data from CPU to the DX12 UI texture.
    // pixelData: RGBA8 pixel data (width * height * 4 bytes), or nullptr for transparent.
    // width/height: must match the initialized dimensions.
    // The upload is staged through an upload heap buffer and a copy command.
    void UploadUIFrame(ID3D12GraphicsCommandList4* cmdList,
                       const uint8_t* pixelData, uint32_t width, uint32_t height);

    // Clear the UI overlay to fully transparent (no UI visible).
    void ClearUIOverlay(ID3D12GraphicsCommandList4* cmdList);

    // Dispatch the composite compute shader to blend UI over the RTX scene.
    // rtxOutputUAV: GPU descriptor handle for the RTX post-processed output (RWTexture2D u0)
    // The UI texture is bound as SRV (Texture2D t0).
    void Composite(ID3D12GraphicsCommandList4* cmdList,
                   uint32_t width, uint32_t height,
                   D3D12_GPU_DESCRIPTOR_HANDLE rtxOutputUAV);

    // Set the global UI opacity (0.0 = invisible, 1.0 = fully visible).
    void SetUIOpacity(float opacity) { m_uiOpacity = opacity; }

    // Enable/disable debug outline around UI elements.
    void SetDebugOutline(bool enabled) { m_debugOutline = enabled; }

    // Check if the compositor is initialized and ready.
    bool IsInitialized() const { return m_initialized; }

    // Check if the compositor has received at least one UI frame.
    bool HasUIData() const { return m_hasUIData; }

    // Get the UI overlay texture resource (for barrier management).
    ID3D12Resource* GetUITexture() const { return m_uiTexture.Get(); }

    // Get the UI texture SRV descriptor handle (for binding).
    D3D12_GPU_DESCRIPTOR_HANDLE GetUITextureSRV() const { return m_uiTextureSRVGPU; }

    // Get the composite descriptor table handle.
    // This table contains: [u0 = RTX output UAV, t0 = UI overlay SRV]
    D3D12_GPU_DESCRIPTOR_HANDLE GetCompositeTable() const { return m_compositeTableGPU; }

private:
    bool CreateCompositeComputePipeline();
    bool CreateUITexture(uint32_t width, uint32_t height);
    bool CreateUploadBuffer(uint32_t width, uint32_t height);
    bool CreateDescriptors();

    DX12Device* m_device = nullptr;
    bool m_initialized = false;
    bool m_hasUIData = false;
    bool m_firstUpload = true;  // Tracks first upload for state transition

    uint32_t m_width = 0;
    uint32_t m_height = 0;
    float m_uiOpacity = 1.0f;
    bool m_debugOutline = false;

    // UI overlay texture (RGBA8, DEFAULT heap)
    ComPtr<ID3D12Resource> m_uiTexture;

    // Upload buffer for CPU→GPU transfer (UPLOAD heap)
    ComPtr<ID3D12Resource> m_uploadBuffer;
    void* m_uploadBufferMapped = nullptr;

    // Composite compute pipeline
    ComPtr<ID3D12PipelineState> m_compositePipelineState;
    ComPtr<ID3D12RootSignature> m_compositeRootSignature;

    // Descriptor handles
    // The composite shader needs:
    //   u0 = RTX scene output (RWTexture2D) - passed per-dispatch
    //   t0 = UI overlay (Texture2D SRV)
    //   b0 = CompositeConstants (32-bit root constants)
    //   s0 = Sampler (point sampling for UI)
    D3D12_GPU_DESCRIPTOR_HANDLE m_uiTextureSRVGPU = {};
    D3D12_CPU_DESCRIPTOR_HANDLE m_uiTextureSRVCPU = {};
    D3D12_GPU_DESCRIPTOR_HANDLE m_compositeTableGPU = {};

    // Descriptor heap index for the UI texture SRV
    // Placed in the TextureManager's SRV heap at a reserved slot
    static constexpr uint32_t UI_SRV_SLOT = MAX_BINDLESS_TEXTURES - 40; // 4056
    // Composite table: [u0 = postprocess output UAV, t0 = UI SRV]
    static constexpr uint32_t COMPOSITE_TABLE_SLOT = MAX_BINDLESS_TEXTURES - 42; // 4054
};

} // namespace RTX

#endif // ENABLE_DX12_RTX
#endif // UI_COMPOSITOR_H
