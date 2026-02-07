#ifndef DXR_PIPELINE_H
#define DXR_PIPELINE_H

#ifdef ENABLE_DX12_RTX

#include "DX12Device.h"
#include "RTXTypes.h"
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

    // Dispatch rays
    void DispatchRays(ID3D12GraphicsCommandList4* commandList, uint32_t width, uint32_t height);

    // Dispatch denoise compute shader
    void DispatchDenoise(ID3D12GraphicsCommandList4* commandList, uint32_t width, uint32_t height, int pass);

    // Update scene constants
    void UpdateSceneConstants(const SceneConstants& constants);

    // Getters
    ID3D12StateObject* GetStateObject() const { return m_stateObject.Get(); }
    ID3D12RootSignature* GetGlobalRootSignature() const { return m_globalRootSignature.Get(); }
    ID3D12Resource* GetOutputBuffer() const { return m_outputBuffer.Get(); }
    ID3D12Resource* GetAccumulationBuffer() const { return m_accumulationBuffer.Get(); }
    ID3D12Resource* GetConstantBuffer() const { return m_constantBuffer.Get(); }

private:
    // Pipeline creation
    bool CreateGlobalRootSignature();
    bool CreateLocalRootSignature();
    bool CreateRaytracingPipeline();
    bool CreateShaderTables();
    bool CreateConstantBuffer();
    bool CreateDenoiseComputePipeline();

    // Shader compilation
    ComPtr<IDxcBlob> CompileShader(const std::wstring& filePath, const wchar_t* entryPoint, const wchar_t* target);
    bool LoadPrecompiledShader(const std::wstring& filePath, ComPtr<IDxcBlob>& blob);

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

    // Scene constant buffer (per-frame, CPU-visible upload heap)
    ComPtr<ID3D12Resource> m_constantBuffer;
    void* m_constantBufferMapped = nullptr;

    // Shader compiler
    ComPtr<IDxcLibrary> m_dxcLibrary;
    ComPtr<IDxcCompiler> m_dxcCompiler;

    // Root parameter indices (global root signature)
    enum GlobalRootParam {
        GlobalRootParam_SceneConstants = 0,    // CBV b0
        GlobalRootParam_AccelerationStructure, // SRV t0
        GlobalRootParam_OutputBuffer,          // UAV u0
        GlobalRootParam_AccumulationBuffer,    // UAV u1
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
