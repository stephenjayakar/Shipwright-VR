#pragma once
#ifndef RTX_DENOISER_H
#define RTX_DENOISER_H

#ifdef ENABLE_DX12_RTX

#include "DX12Device.h"
#include "RTXTypes.h"
#include <d3d12.h>
#include <dxcapi.h>
#include <wrl/client.h>
#include <cstdint>

using Microsoft::WRL::ComPtr;

namespace RTX {

// Forward declarations
class DXRPipeline;

// ============================================================================
// RTX_Denoiser - Enhanced Denoiser with Temporal Reprojection
//
// This module manages an improved denoising pipeline that builds on top of
// the existing A-Trous wavelet denoise and temporal accumulation:
//
// Pipeline order (called from RTXRenderer::DispatchAndPresent):
//   1. DXR DispatchRays writes: output(u0), giAccum(u1), normals(u2), depth(u3)
//   2. TemporalReprojection pass: reprojects history using camera matrices,
//      performs variance-clipping, writes to accumulation buffer
//   3. VarianceEstimation pass: computes per-pixel variance from a local
//      neighborhood of the accumulated result, writes to variance buffer
//   4. 4-pass A-Trous wavelet denoise (existing) with the variance buffer
//      providing per-pixel adaptive filter strength
//   5. PostProcess (existing tone mapping + gamma)
//
// Key improvements over the base temporal accumulation:
//   - Camera motion reprojection using previous/current view-projection matrices
//   - Per-pixel velocity for disocclusion detection
//   - Variance buffer guides the spatial filter kernel size
//   - Improved firefly rejection using temporal neighborhood statistics
// ============================================================================

// Constants for the temporal reprojection shader
struct TemporalReprojectionConstants {
    float prevViewProj[16];   // Previous frame's view-projection matrix (row-major)
    float currViewProjInv[16]; // Current frame's inverse view-projection matrix
    float cameraPos[3];       // Current camera position
    float blendAlpha;         // Base blend factor for new frame contribution
    uint32_t resolutionX;     // Output width
    uint32_t resolutionY;     // Output height
    uint32_t frameCount;      // Accumulation frame counter (0 = reset)
    float depthRejectThreshold; // Depth difference threshold for history rejection
};

// Constants for the variance estimation shader
struct VarianceEstimationConstants {
    uint32_t resolutionX;
    uint32_t resolutionY;
    uint32_t kernelRadius;    // 1 = 3x3, 2 = 5x5
    float varianceBoost;      // Multiplier for variance signal (default 1.0)
};

class RTX_Denoiser {
public:
    RTX_Denoiser();
    ~RTX_Denoiser();

    // Initialize the denoiser. Creates compute pipelines and buffers.
    // Must be called after DX12Device and DXRPipeline are initialized.
    bool Initialize(DX12Device* device, DXRPipeline* pipeline, uint32_t width, uint32_t height);

    // Shutdown and release all resources.
    void Shutdown();

    // Check if initialized
    bool IsInitialized() const { return m_initialized; }

    // === Per-Frame Camera State ===

    // Store the current frame's camera matrices for reprojection next frame.
    // Must be called each frame before DispatchTemporalReprojection.
    // viewMatrix: 4x4 view matrix (row-major float[16])
    // projMatrix: 4x4 projection matrix (row-major float[16])
    void UpdateCameraMatrices(const float viewMatrix[16], const float projMatrix[16]);

    // === Dispatch Methods ===
    // These are called by RTXRenderer in the correct order.

    // Dispatch temporal reprojection (replaces the simpler Accumulate pass).
    // Reads from: output buffer (current frame), accumulation buffer (history),
    //             normals buffer, depth buffer
    // Writes to: accumulation buffer (blended result), output buffer (for denoise input)
    void DispatchTemporalReprojection(
        ID3D12GraphicsCommandList4* cmdList,
        uint32_t width, uint32_t height,
        uint32_t frameCount, float blendAlpha);

    // Dispatch variance estimation.
    // Reads from: output buffer (after temporal accumulation)
    // Writes to: variance buffer
    void DispatchVarianceEstimation(
        ID3D12GraphicsCommandList4* cmdList,
        uint32_t width, uint32_t height);

    // Get the variance buffer resource (for denoise shader to read)
    ID3D12Resource* GetVarianceBuffer() const { return m_varianceBuffer.Get(); }

    // Get the motion vector buffer resource
    ID3D12Resource* GetMotionVectorBuffer() const { return m_motionVectorBuffer.Get(); }

    // === Accessors ===
    bool HasValidHistory() const { return m_hasValidHistory; }

private:
    // Pipeline creation
    bool CreateTemporalReprojectionPipeline();
    bool CreateVarianceEstimationPipeline();
    bool CreateBuffers(uint32_t width, uint32_t height);
    bool CreateDescriptors();

    // Matrix utilities
    void MultiplyMatrix4x4(const float a[16], const float b[16], float out[16]);
    void InvertMatrix4x4(const float m[16], float out[16]);

    // Device references (not owned)
    DX12Device* m_device = nullptr;
    DXRPipeline* m_pipeline = nullptr;
    bool m_initialized = false;

    // Temporal reprojection compute pipeline
    ComPtr<ID3D12PipelineState> m_temporalReprojPSO;
    ComPtr<ID3D12RootSignature> m_temporalReprojRootSig;
    ComPtr<IDxcBlob> m_temporalReprojBlob;

    // Variance estimation compute pipeline
    ComPtr<ID3D12PipelineState> m_varianceEstPSO;
    ComPtr<ID3D12RootSignature> m_varianceEstRootSig;
    ComPtr<IDxcBlob> m_varianceEstBlob;

    // Buffers
    ComPtr<ID3D12Resource> m_varianceBuffer;      // R16_FLOAT: per-pixel variance
    ComPtr<ID3D12Resource> m_motionVectorBuffer;   // R16G16_FLOAT: screen-space motion vectors
    ComPtr<ID3D12Resource> m_prevDepthBuffer;      // R32_FLOAT: previous frame depth
    ComPtr<ID3D12Resource> m_prevNormalsBuffer;    // R16G16B16A16_FLOAT: previous frame normals

    // Descriptor handles for temporal reprojection
    D3D12_GPU_DESCRIPTOR_HANDLE m_temporalReprojTable;
    // Descriptor handles for variance estimation
    D3D12_GPU_DESCRIPTOR_HANDLE m_varianceEstTable;

    bool m_descriptorsCreated = false;

    // Camera matrix history
    float m_prevViewMatrix[16] = {};
    float m_prevProjMatrix[16] = {};
    float m_currViewMatrix[16] = {};
    float m_currProjMatrix[16] = {};
    float m_prevViewProjMatrix[16] = {};
    bool m_hasValidHistory = false;

    // Descriptor heap slot indices (reserved at end of TextureManager SRV heap)
    // These must not conflict with DXRPipeline's descriptor slots.
    // DXRPipeline uses slots 4060-4095, so we use slots before that.
    static constexpr uint32_t DENOISER_DESCRIPTOR_BASE = MAX_BINDLESS_TEXTURES - 50; // 4046
    // Temporal reprojection table: 8 descriptors
    //   [0] UAV: output buffer (current frame, read/write)
    //   [1] UAV: accumulation buffer (history, read/write)
    //   [2] SRV: normals buffer (current frame, read)
    //   [3] SRV: depth buffer (current frame, read)
    //   [4] SRV: previous depth buffer (read)
    //   [5] SRV: previous normals buffer (read)
    //   [6] UAV: motion vector buffer (write)
    //   [7] UAV: prev depth copy target (write)
    static constexpr uint32_t TEMPORAL_REPROJ_TABLE_START = DENOISER_DESCRIPTOR_BASE;     // 4046
    static constexpr uint32_t TEMPORAL_REPROJ_TABLE_SIZE  = 8;

    // Variance estimation table: 3 descriptors
    //   [0] SRV: accumulated color (read)
    //   [1] UAV: variance buffer (write)
    //   [2] (reserved)
    static constexpr uint32_t VARIANCE_EST_TABLE_START = DENOISER_DESCRIPTOR_BASE + 8;    // 4054
    static constexpr uint32_t VARIANCE_EST_TABLE_SIZE  = 3;
};

} // namespace RTX

#endif // ENABLE_DX12_RTX
#endif // RTX_DENOISER_H
