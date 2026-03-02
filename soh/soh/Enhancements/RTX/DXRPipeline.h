#pragma once
#ifndef DXR_PIPELINE_H
#define DXR_PIPELINE_H

#ifdef ENABLE_DX12_RTX

#include "DX12Device.h"
#include "RTXTypes.h"
#include "RTXShaderCompiler.h"
#include <d3d12.h>
#include <dxcapi.h>
#include <wrl/client.h>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace RTX {

class DXRPipeline {
public:
    DXRPipeline();
    ~DXRPipeline();

    bool Initialize(DX12Device* device, uint32_t width, uint32_t height);
    void Shutdown();

    // Create/update output and accumulation buffers
    bool CreateOutputBuffers(uint32_t width, uint32_t height);

    // Create UAV descriptors in the TextureManager's SRV heap.
    // Must be called AFTER TextureManager::Initialize() since the SRV heap
    // is not available during DXRPipeline::Initialize().
    bool CreateUAVDescriptors();

    // Dispatch rays
    // textureTableGPU: GPU descriptor handle for the start of the bindless texture
    //   SRV table (from TextureManager). If ptr==0, no texture table is bound.
    void DispatchRays(ID3D12GraphicsCommandList4* commandList, uint32_t width, uint32_t height,
                      D3D12_GPU_VIRTUAL_ADDRESS tlasAddress = 0,
                      D3D12_GPU_DESCRIPTOR_HANDLE textureTableGPU = {});

    // Dispatch denoise compute shader (uses default denoise constants)
    void DispatchDenoise(ID3D12GraphicsCommandList4* commandList, uint32_t width, uint32_t height, int pass);

    // Dispatch denoise compute shader with explicit constants from GISystem.
    // passIndex is used for ping-pong buffer selection (0, 1, 2, ...).
    void DispatchDenoise(ID3D12GraphicsCommandList4* commandList, uint32_t width, uint32_t height,
                         int passIndex, const DenoiseConstants& constants);

    // Dispatch temporal accumulation compute shader
    void DispatchAccumulate(ID3D12GraphicsCommandList4* commandList, uint32_t width, uint32_t height,
                            const AccumulateConstants& constants);

    // Dispatch post-process (tone mapping + gamma) compute shader
    void DispatchPostProcess(ID3D12GraphicsCommandList4* commandList, uint32_t width, uint32_t height,
                             const PostProcessConstants& constants);

    // Update scene constants
    void UpdateSceneConstants(const SceneConstants& constants);

    // Update the hit group shader table local root arguments with per-geometry
    // buffer GPU addresses.  Must be called after AccelerationStructure::BuildBLAS()
    // so the geometry buffer list is populated.  Also updates the active hit group
    // count used by DispatchRays to set the correct SizeInBytes.
    struct GeometryBufferAddresses {
        D3D12_GPU_VIRTUAL_ADDRESS vertexBuffer;
        D3D12_GPU_VIRTUAL_ADDRESS indexBuffer;
        D3D12_GPU_VIRTUAL_ADDRESS materialIDBuffer;
        D3D12_GPU_VIRTUAL_ADDRESS materialTable;
    };
    void UpdateHitGroupShaderTable(const std::vector<GeometryBufferAddresses>& geometries);

    // Getters
    ID3D12StateObject* GetStateObject() const { return m_stateObject.Get(); }
    ID3D12RootSignature* GetGlobalRootSignature() const { return m_globalRootSignature.Get(); }
    ID3D12Resource* GetOutputBuffer() const { return m_outputBuffer.Get(); }
    ID3D12Resource* GetDenoiseTempBuffer() const { return m_denoiseTempBuffer.Get(); }
    ID3D12Resource* GetAccumulationBuffer() const { return m_accumulationBuffer.Get(); }
    ID3D12Resource* GetNormalsBuffer() const { return m_normalsBuffer.Get(); }
    ID3D12Resource* GetDepthBuffer() const { return m_depthBuffer.Get(); }
    ID3D12Resource* GetPostProcessBuffer() const { return m_postProcessBuffer.Get(); }
    ID3D12Resource* GetConstantBuffer() const { return m_constantBuffer.Get(); }
    // Returns the PostProcess compute pipeline state (null if shader failed to compile)
    ID3D12PipelineState* GetPostProcessPipelineState() const { return m_postProcessPipelineState.Get(); }
    // Returns the Denoise compute pipeline state (null if shader failed to compile)
    ID3D12PipelineState* GetDenoisePipelineState() const { return m_denoisePipelineState.Get(); }
    // Returns the Accumulate compute pipeline state (null if shader failed to compile)
    ID3D12PipelineState* GetAccumulatePipelineState() const { return m_accumulatePipelineState.Get(); }
    D3D12_GPU_DESCRIPTOR_HANDLE GetOutputUAV() const;
    D3D12_GPU_DESCRIPTOR_HANDLE GetPostProcessBufferUAV() const { return m_postProcessBufferUAV; }

    // After N denoise passes with ping-pong, returns the buffer containing
    // the final denoised result. For odd N, result is in temp buffer;
    // for even N (or 0), result is in output buffer.
    ID3D12Resource* GetFinalDenoisedBuffer(int totalPasses) const {
        return (totalPasses > 0 && totalPasses % 2 != 0)
            ? m_denoiseTempBuffer.Get()
            : m_outputBuffer.Get();
    }

    // Returns the post-process output buffer if available,
    // otherwise falls back to denoised buffer.
    ID3D12Resource* GetFinalOutputBuffer(int totalDenoisePasses) const {
        if (m_postProcessBuffer.Get())
            return m_postProcessBuffer.Get();
        return GetFinalDenoisedBuffer(totalDenoisePasses);
    }

private:
    // Pipeline creation
    bool CreateGlobalRootSignature();
    bool CreateLocalRootSignature();
    bool CreateRaytracingPipeline();
    bool CreateShaderTables();
    bool CreateConstantBuffer();
    bool CreateDenoiseComputePipeline();
    bool CreateAccumulateComputePipeline();
    bool CreatePostProcessComputePipeline();

    // Shader loading (uses RTXShaderCompiler)
    bool LoadShaders();

    // Device reference (not owned)
    DX12Device* m_device = nullptr;

    // DXR state object
    ComPtr<ID3D12StateObject> m_stateObject;
    ComPtr<ID3D12StateObjectProperties> m_stateObjectProperties;

    // Root signatures
    ComPtr<ID3D12RootSignature> m_globalRootSignature;
    ComPtr<ID3D12RootSignature> m_localRootSignature;

    // Denoise compute pipeline
    ComPtr<ID3D12PipelineState> m_denoisePipelineState;
    ComPtr<ID3D12RootSignature> m_denoiseRootSignature;

    // Accumulate compute pipeline
    ComPtr<ID3D12PipelineState> m_accumulatePipelineState;
    ComPtr<ID3D12RootSignature> m_accumulateRootSignature;

    // Post-process compute pipeline (tone mapping + gamma)
    ComPtr<ID3D12PipelineState> m_postProcessPipelineState;
    ComPtr<ID3D12RootSignature> m_postProcessRootSignature;

    // Shader tables
    ComPtr<ID3D12Resource> m_rayGenShaderTable;
    ComPtr<ID3D12Resource> m_missShaderTable;
    ComPtr<ID3D12Resource> m_hitGroupShaderTable;
    uint32_t m_rayGenRecordSize = 0;
    uint32_t m_missRecordSize = 0;
    uint32_t m_hitGroupRecordSize = 0;

    // Output buffers
    ComPtr<ID3D12Resource> m_outputBuffer;         // RWTexture2D<float4> for ray tracing output
    ComPtr<ID3D12Resource> m_accumulationBuffer;   // RWTexture2D<float4> for temporal accumulation
    ComPtr<ID3D12Resource> m_denoiseTempBuffer;    // Temp buffer for denoise ping-pong
    ComPtr<ID3D12Resource> m_normalsBuffer;        // RWTexture2D<float4> for G-buffer normals
    ComPtr<ID3D12Resource> m_depthBuffer;          // RWTexture2D<float> for G-buffer depth
    ComPtr<ID3D12Resource> m_postProcessBuffer;    // RWTexture2D<float4> for post-process output
    
    // UAV descriptor handles (stored for GetOutputUAV)
    D3D12_GPU_DESCRIPTOR_HANDLE m_outputBufferUAV;
    D3D12_GPU_DESCRIPTOR_HANDLE m_accumulationBufferUAV;
    D3D12_GPU_DESCRIPTOR_HANDLE m_denoiseTempBufferUAV;
    D3D12_GPU_DESCRIPTOR_HANDLE m_normalsBufferUAV;
    D3D12_GPU_DESCRIPTOR_HANDLE m_depthBufferUAV;
    D3D12_GPU_DESCRIPTOR_HANDLE m_postProcessBufferUAV;
    // Denoise ping-pong descriptor table handles (contiguous u0+u1 pairs)
    D3D12_GPU_DESCRIPTOR_HANDLE m_denoiseEvenPassTable; // u0=output, u1=temp
    D3D12_GPU_DESCRIPTOR_HANDLE m_denoiseOddPassTable;  // u0=temp, u1=output
    // Accumulate descriptor table: u0=current(output), u1=history(accum)
    D3D12_GPU_DESCRIPTOR_HANDLE m_accumulateTable;
    // Post-process descriptor table: u0=input(denoised), u1=output(postprocess)
    D3D12_GPU_DESCRIPTOR_HANDLE m_postProcessTable;
    bool m_uavDescriptorsCreated = false;

    // Scene constant buffer (per-frame, CPU-visible upload heap)
    ComPtr<ID3D12Resource> m_constantBuffer;
    void* m_constantBufferMapped = nullptr;

    // Active hit group record count (updated by UpdateHitGroupShaderTable)
    uint32_t m_activeHitGroupCount = 0;

    // UAV descriptors are placed at the end of the TextureManager's SRV heap.
    // The heap has MAX_BINDLESS_TEXTURES (4096) entries; we reserve slots at the end:
    //
    //   Layout (end of heap):
    //   4060-4061: accumulate table (u0=output, u1=accum)
    //   4062-4063: post-process table (u0=denoised, u1=postprocess)
    //   4064-4069: denoise tables (see below)
    //     4064-4065: denoise even pass (u0=output, u1=temp) + SRV t0=normals, SRV t1=depth
    //     4066-4067: denoise odd pass  (u0=temp, u1=output)
    //     4068-4069: denoise SRVs for normals + depth (shared by both passes)
    //   4070-4079: (reserved)
    //   4080: output UAV  (raytracing u0)
    //   4081: accumulation UAV (raytracing u1)
    //   4082: normals UAV (raytracing u2)
    //   4083: depth UAV (raytracing u3)
    //   4084: denoise temp UAV
    //   4085: post-process output UAV
    //   4086-4095: (reserved)
    //
    static constexpr uint32_t ACCUMULATE_TABLE_START    = MAX_BINDLESS_TEXTURES - 36; // 4060
    static constexpr uint32_t POSTPROCESS_TABLE_START   = MAX_BINDLESS_TEXTURES - 34; // 4062
    static constexpr uint32_t DENOISE_EVEN_PASS_START   = MAX_BINDLESS_TEXTURES - 32; // 4064
    static constexpr uint32_t DENOISE_ODD_PASS_START    = MAX_BINDLESS_TEXTURES - 30; // 4066
    static constexpr uint32_t DENOISE_SRV_START         = MAX_BINDLESS_TEXTURES - 28; // 4068
    static constexpr uint32_t UAV_DESCRIPTOR_START      = MAX_BINDLESS_TEXTURES - 16; // 4080

    // Compiled shader blobs (loaded by RTXShaderCompiler)
    ComPtr<IDxcBlob> m_rayGenBlob;
    ComPtr<IDxcBlob> m_closestHitBlob;
    ComPtr<IDxcBlob> m_missBlob;
    ComPtr<IDxcBlob> m_anyHitBlob;
    ComPtr<IDxcBlob> m_denoiseBlob;
    ComPtr<IDxcBlob> m_accumulateBlob;
    ComPtr<IDxcBlob> m_postProcessBlob;

    // Root parameter indices (global root signature)
    enum GlobalRootParam {
        GlobalRootParam_SceneConstants = 0,    // CBV b0 (root constant buffer view, NOT inline 32-bit constants)
        GlobalRootParam_AccelerationStructure, // SRV t0
        GlobalRootParam_UAVTable,              // Descriptor table: UAV u0 (output) + u1 (accumulation) + u2 (normals) + u3 (depth)
        GlobalRootParam_TextureTable,          // Descriptor table (SRV t4+)
        GlobalRootParam_Sampler,               // Static sampler s0
        GlobalRootParam_Count
    };

    // Root parameter indices (local root signature, per hit group)
    enum LocalRootParam {
        LocalRootParam_VertexBuffer = 0,   // SRV t0, space1
        LocalRootParam_IndexBuffer,        // SRV t1, space1
        LocalRootParam_MaterialIDBuffer,   // SRV t2, space1
        LocalRootParam_MaterialTable,      // SRV t3, space1
        LocalRootParam_Count
    };
};

} // namespace RTX

#endif // ENABLE_DX12_RTX
#endif // DXR_PIPELINE_H
