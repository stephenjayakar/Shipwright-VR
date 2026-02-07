#ifdef ENABLE_DX12_RTX

#include "DXRPipeline.h"
#include <spdlog/spdlog.h>
#include <cassert>
#include <fstream>

// Link DXC
#pragma comment(lib, "dxcompiler.lib")

namespace RTX {

// Shader entry point names (must match HLSL exports)
static const wchar_t* kRayGenExport = L"RayGen";
static const wchar_t* kClosestHitExport = L"ClosestHit";
static const wchar_t* kMissExport = L"Miss";
static const wchar_t* kAnyHitExport = L"AnyHit";
static const wchar_t* kHitGroupName = L"HitGroup";

// Align to D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT
static uint32_t Align(uint32_t size, uint32_t alignment) {
    return (size + alignment - 1) & ~(alignment - 1);
}

DXRPipeline::DXRPipeline() = default;

DXRPipeline::~DXRPipeline() {
    Shutdown();
}

bool DXRPipeline::Initialize(DX12Device* device, uint32_t width, uint32_t height) {
    m_device = device;

    // Initialize DXC compiler
    HRESULT hr = DxcCreateInstance(CLSID_DxcLibrary, IID_PPV_ARGS(&m_dxcLibrary));
    if (FAILED(hr)) {
        SPDLOG_ERROR("[RTX] Failed to create DXC library: 0x{:08X}", (uint32_t)hr);
        return false;
    }
    hr = DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&m_dxcCompiler));
    if (FAILED(hr)) {
        SPDLOG_ERROR("[RTX] Failed to create DXC compiler: 0x{:08X}", (uint32_t)hr);
        return false;
    }

    if (!CreateGlobalRootSignature()) return false;
    if (!CreateLocalRootSignature()) return false;
    if (!CreateRaytracingPipeline()) return false;
    if (!CreateShaderTables()) return false;
    if (!CreateConstantBuffer()) return false;
    if (!CreateOutputBuffers(width, height)) return false;
    if (!CreateDenoiseComputePipeline()) return false;

    SPDLOG_INFO("[RTX] DXR pipeline initialized");
    return true;
}

void DXRPipeline::Shutdown() {
    if (m_constantBufferMapped) {
        m_constantBuffer->Unmap(0, nullptr);
        m_constantBufferMapped = nullptr;
    }
    m_device = nullptr;
}

bool DXRPipeline::CreateGlobalRootSignature() {
    // Global root signature layout:
    // [0] CBV (b0)  - SceneConstants
    // [1] SRV (t0)  - Acceleration structure (TLAS)
    // [2] UAV (u0)  - Output buffer
    // [3] UAV (u1)  - GI accumulation buffer
    // [4] Descriptor Table - SRV range (textures, t4+)
    // Static sampler s0 - bilinear wrap

    D3D12_ROOT_PARAMETER rootParams[GlobalRootParam_Count - 1] = {}; // -1 because sampler is static

    // [0] CBV b0 - Scene constants
    rootParams[GlobalRootParam_SceneConstants].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[GlobalRootParam_SceneConstants].Descriptor.ShaderRegister = 0;
    rootParams[GlobalRootParam_SceneConstants].Descriptor.RegisterSpace = 0;
    rootParams[GlobalRootParam_SceneConstants].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // [1] SRV t0 - TLAS
    rootParams[GlobalRootParam_AccelerationStructure].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    rootParams[GlobalRootParam_AccelerationStructure].Descriptor.ShaderRegister = 0;
    rootParams[GlobalRootParam_AccelerationStructure].Descriptor.RegisterSpace = 0;
    rootParams[GlobalRootParam_AccelerationStructure].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // [2] UAV u0 - Output buffer
    rootParams[GlobalRootParam_OutputBuffer].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rootParams[GlobalRootParam_OutputBuffer].Descriptor.ShaderRegister = 0;
    rootParams[GlobalRootParam_OutputBuffer].Descriptor.RegisterSpace = 0;
    rootParams[GlobalRootParam_OutputBuffer].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // [3] UAV u1 - Accumulation buffer
    rootParams[GlobalRootParam_AccumulationBuffer].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rootParams[GlobalRootParam_AccumulationBuffer].Descriptor.ShaderRegister = 1;
    rootParams[GlobalRootParam_AccumulationBuffer].Descriptor.RegisterSpace = 0;
    rootParams[GlobalRootParam_AccumulationBuffer].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // [4] Descriptor table for texture array (SRV t4+)
    D3D12_DESCRIPTOR_RANGE texRange = {};
    texRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    texRange.NumDescriptors = 512; // Up to 512 textures
    texRange.BaseShaderRegister = 4;
    texRange.RegisterSpace = 0;
    texRange.OffsetInDescriptorsFromTableStart = 0;

    rootParams[GlobalRootParam_TextureTable].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[GlobalRootParam_TextureTable].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[GlobalRootParam_TextureTable].DescriptorTable.pDescriptorRanges = &texRange;
    rootParams[GlobalRootParam_TextureTable].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // Static sampler: bilinear wrap
    D3D12_STATIC_SAMPLER_DESC staticSampler = {};
    staticSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    staticSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    staticSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    staticSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    staticSampler.MipLODBias = 0;
    staticSampler.MaxAnisotropy = 1;
    staticSampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    staticSampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
    staticSampler.MinLOD = 0.0f;
    staticSampler.MaxLOD = D3D12_FLOAT32_MAX;
    staticSampler.ShaderRegister = 0;
    staticSampler.RegisterSpace = 0;
    staticSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rootSigDesc = {};
    rootSigDesc.NumParameters = GlobalRootParam_Count - 1;
    rootSigDesc.pParameters = rootParams;
    rootSigDesc.NumStaticSamplers = 1;
    rootSigDesc.pStaticSamplers = &staticSampler;

    ComPtr<ID3DBlob> blob;
    ComPtr<ID3DBlob> error;
    HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error);
    if (FAILED(hr)) {
        if (error) {
            SPDLOG_ERROR("[RTX] Global root signature serialization error: {}",
                         (const char*)error->GetBufferPointer());
        }
        return false;
    }

    hr = m_device->GetDevice()->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                                     IID_PPV_ARGS(&m_globalRootSignature));
    if (FAILED(hr)) {
        SPDLOG_ERROR("[RTX] Failed to create global root signature: 0x{:08X}", (uint32_t)hr);
        return false;
    }

    return true;
}

bool DXRPipeline::CreateLocalRootSignature() {
    // Local root signature (per hit group):
    // [0] SRV t0, space1 - Vertex buffer
    // [1] SRV t1, space1 - Index buffer
    // [2] SRV t2, space1 - Material ID buffer
    // [3] SRV t3, space1 - Material table

    D3D12_ROOT_PARAMETER localParams[LocalRootParam_Count] = {};

    for (int i = 0; i < LocalRootParam_Count; i++) {
        localParams[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        localParams[i].Descriptor.ShaderRegister = i;
        localParams[i].Descriptor.RegisterSpace = 1;
        localParams[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }

    D3D12_ROOT_SIGNATURE_DESC localSigDesc = {};
    localSigDesc.NumParameters = LocalRootParam_Count;
    localSigDesc.pParameters = localParams;
    localSigDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE;

    ComPtr<ID3DBlob> blob;
    ComPtr<ID3DBlob> error;
    HRESULT hr = D3D12SerializeRootSignature(&localSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error);
    if (FAILED(hr)) {
        if (error) {
            SPDLOG_ERROR("[RTX] Local root signature serialization error: {}",
                         (const char*)error->GetBufferPointer());
        }
        return false;
    }

    hr = m_device->GetDevice()->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                                     IID_PPV_ARGS(&m_localRootSignature));
    if (FAILED(hr)) {
        SPDLOG_ERROR("[RTX] Failed to create local root signature: 0x{:08X}", (uint32_t)hr);
        return false;
    }

    return true;
}

ComPtr<IDxcBlob> DXRPipeline::CompileShader(const std::wstring& filePath, const wchar_t* entryPoint, const wchar_t* target) {
    // Read shader file
    ComPtr<IDxcBlobEncoding> sourceBlob;
    HRESULT hr = m_dxcLibrary->CreateBlobFromFile(filePath.c_str(), nullptr, &sourceBlob);
    if (FAILED(hr)) {
        SPDLOG_ERROR("[RTX] Failed to load shader file");
        return nullptr;
    }

    // Compile
    ComPtr<IDxcOperationResult> result;
    hr = m_dxcCompiler->Compile(
        sourceBlob.Get(),
        filePath.c_str(),
        entryPoint,
        target,
        nullptr, 0,    // arguments
        nullptr, 0,    // defines
        nullptr,       // include handler
        &result
    );

    if (SUCCEEDED(hr)) {
        result->GetStatus(&hr);
    }

    if (FAILED(hr)) {
        ComPtr<IDxcBlobEncoding> errors;
        result->GetErrorBuffer(&errors);
        if (errors && errors->GetBufferSize() > 0) {
            SPDLOG_ERROR("[RTX] Shader compilation error: {}",
                         (const char*)errors->GetBufferPointer());
        }
        return nullptr;
    }

    ComPtr<IDxcBlob> compiled;
    result->GetResult(&compiled);
    return compiled;
}

bool DXRPipeline::CreateRaytracingPipeline() {
    // For now, we'll set up the pipeline structure. Actual shader compilation
    // requires the HLSL files to be present on the target Windows machine.
    // The pipeline will be created from precompiled DXIL blobs or compiled at runtime.

    // This is a placeholder that creates the state object descriptor.
    // Full implementation requires:
    // 1. Loading/compiling RayGen, ClosestHit, Miss, AnyHit shaders
    // 2. Creating DXIL library sub-objects
    // 3. Creating hit group
    // 4. Setting shader config (payload/attribute sizes)
    // 5. Setting pipeline config (max recursion depth = 2)
    // 6. Associating root signatures

    // We'll compile shaders at build time via CMake and load DXIL at runtime.
    // For now, log that we'd create the pipeline here.
    SPDLOG_INFO("[RTX] Raytracing pipeline creation deferred (shaders loaded at runtime)");

    // TODO: When running on Windows with compiled shaders:
    // - Load .dxil files from shader output directory
    // - Build D3D12_STATE_OBJECT_DESC with all subobjects
    // - Create state object via CreateStateObject()
    // - Query shader identifiers for shader table building

    return true;
}

bool DXRPipeline::CreateShaderTables() {
    // Shader tables will be created after the state object is built.
    // Each table contains shader records:
    //   Ray Gen table: 1 record (shader ID only, 32 bytes)
    //   Miss table:    1 record (shader ID only, 32 bytes)
    //   Hit Group table: N records (shader ID + local root args per geometry)

    // Shader record sizes must be aligned to D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT (32)
    m_rayGenRecordSize = Align(D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES, D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT);
    m_missRecordSize = Align(D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES, D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT);

    // Hit group record: shader ID + 4 GPU virtual addresses (vertex, index, matID, material buffers)
    m_hitGroupRecordSize = Align(
        D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES + LocalRootParam_Count * sizeof(D3D12_GPU_VIRTUAL_ADDRESS),
        D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT
    );

    SPDLOG_INFO("[RTX] Shader table record sizes: RayGen={}, Miss={}, HitGroup={}",
                m_rayGenRecordSize, m_missRecordSize, m_hitGroupRecordSize);

    // Actual GPU buffer allocation happens when geometry is loaded and we know
    // how many hit group records we need.
    return true;
}

bool DXRPipeline::CreateConstantBuffer() {
    // Create an upload heap constant buffer for SceneConstants
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC bufferDesc = {};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = sizeof(SceneConstants); // Already 256-byte aligned
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    HRESULT hr = m_device->GetDevice()->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &bufferDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&m_constantBuffer)
    );

    if (FAILED(hr)) {
        SPDLOG_ERROR("[RTX] Failed to create constant buffer: 0x{:08X}", (uint32_t)hr);
        return false;
    }

    // Map persistently (upload heap stays mapped)
    hr = m_constantBuffer->Map(0, nullptr, &m_constantBufferMapped);
    if (FAILED(hr)) {
        SPDLOG_ERROR("[RTX] Failed to map constant buffer: 0x{:08X}", (uint32_t)hr);
        return false;
    }

    return true;
}

bool DXRPipeline::CreateOutputBuffers(uint32_t width, uint32_t height) {
    auto device = m_device->GetDevice();

    auto createUAVTexture = [&](ComPtr<ID3D12Resource>& resource, const char* name) -> bool {
        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC texDesc = {};
        texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texDesc.Width = width;
        texDesc.Height = height;
        texDesc.DepthOrArraySize = 1;
        texDesc.MipLevels = 1;
        texDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        texDesc.SampleDesc.Count = 1;
        texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        HRESULT hr = device->CreateCommittedResource(
            &heapProps,
            D3D12_HEAP_FLAG_NONE,
            &texDesc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            nullptr,
            IID_PPV_ARGS(&resource)
        );

        if (FAILED(hr)) {
            SPDLOG_ERROR("[RTX] Failed to create {} buffer: 0x{:08X}", name, (uint32_t)hr);
            return false;
        }
        return true;
    };

    if (!createUAVTexture(m_outputBuffer, "output")) return false;
    if (!createUAVTexture(m_accumulationBuffer, "accumulation")) return false;
    if (!createUAVTexture(m_denoiseTempBuffer, "denoise temp")) return false;

    SPDLOG_INFO("[RTX] Output buffers created ({}x{})", width, height);
    return true;
}

bool DXRPipeline::CreateDenoiseComputePipeline() {
    // The denoise pipeline uses a simple compute shader root signature:
    // [0] UAV u0 - input
    // [1] UAV u1 - output
    // [2] CBV b0 - DenoiseConstants

    D3D12_ROOT_PARAMETER denoiseParams[3] = {};

    denoiseParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    denoiseParams[0].Descriptor.ShaderRegister = 0;
    denoiseParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    denoiseParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    denoiseParams[1].Descriptor.ShaderRegister = 1;
    denoiseParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    denoiseParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    denoiseParams[2].Constants.ShaderRegister = 0;
    denoiseParams[2].Constants.Num32BitValues = sizeof(DenoiseConstants) / 4;
    denoiseParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC denoiseSigDesc = {};
    denoiseSigDesc.NumParameters = 3;
    denoiseSigDesc.pParameters = denoiseParams;

    ComPtr<ID3DBlob> blob;
    ComPtr<ID3DBlob> error;
    HRESULT hr = D3D12SerializeRootSignature(&denoiseSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error);
    if (FAILED(hr)) {
        SPDLOG_ERROR("[RTX] Denoise root signature serialization failed");
        return false;
    }

    hr = m_device->GetDevice()->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                                     IID_PPV_ARGS(&m_denoiseRootSignature));
    if (FAILED(hr)) {
        SPDLOG_ERROR("[RTX] Failed to create denoise root signature: 0x{:08X}", (uint32_t)hr);
        return false;
    }

    // Denoise compute PSO will be created when shader is loaded
    SPDLOG_INFO("[RTX] Denoise compute pipeline root signature created");
    return true;
}

void DXRPipeline::UpdateSceneConstants(const SceneConstants& constants) {
    if (m_constantBufferMapped) {
        memcpy(m_constantBufferMapped, &constants, sizeof(SceneConstants));
    }
}

void DXRPipeline::DispatchRays(ID3D12GraphicsCommandList4* commandList, uint32_t width, uint32_t height) {
    if (!m_stateObject) {
        // Pipeline not ready yet (shaders not loaded)
        return;
    }

    // Set global root signature and resources
    commandList->SetComputeRootSignature(m_globalRootSignature.Get());
    commandList->SetComputeRootConstantBufferView(GlobalRootParam_SceneConstants,
                                                   m_constantBuffer->GetGPUVirtualAddress());

    // Build dispatch desc
    D3D12_DISPATCH_RAYS_DESC dispatchDesc = {};

    // Ray generation
    dispatchDesc.RayGenerationShaderRecord.StartAddress = m_rayGenShaderTable->GetGPUVirtualAddress();
    dispatchDesc.RayGenerationShaderRecord.SizeInBytes = m_rayGenRecordSize;

    // Miss
    dispatchDesc.MissShaderTable.StartAddress = m_missShaderTable->GetGPUVirtualAddress();
    dispatchDesc.MissShaderTable.SizeInBytes = m_missRecordSize;
    dispatchDesc.MissShaderTable.StrideInBytes = m_missRecordSize;

    // Hit group
    dispatchDesc.HitGroupTable.StartAddress = m_hitGroupShaderTable->GetGPUVirtualAddress();
    dispatchDesc.HitGroupTable.SizeInBytes = m_hitGroupRecordSize; // Updated when geometry loaded
    dispatchDesc.HitGroupTable.StrideInBytes = m_hitGroupRecordSize;

    // Dispatch dimensions
    dispatchDesc.Width = width;
    dispatchDesc.Height = height;
    dispatchDesc.Depth = 1;

    commandList->SetPipelineState1(m_stateObject.Get());
    commandList->DispatchRays(&dispatchDesc);
}

void DXRPipeline::DispatchDenoise(ID3D12GraphicsCommandList4* commandList, uint32_t width, uint32_t height, int pass) {
    if (!m_denoisePipelineState) return;

    commandList->SetComputeRootSignature(m_denoiseRootSignature.Get());
    commandList->SetPipelineState(m_denoisePipelineState.Get());

    // Set denoise constants
    DenoiseConstants dc = {};
    dc.stepSize = 1 << pass;      // 1, 2, 4 for passes 0, 1, 2
    dc.colorSigma = 0.1f;
    dc.normalSigma = 0.1f;
    commandList->SetComputeRoot32BitConstants(2, sizeof(DenoiseConstants) / 4, &dc, 0);

    // Ping-pong between output buffer and temp buffer
    if (pass % 2 == 0) {
        commandList->SetComputeRootUnorderedAccessView(0, m_outputBuffer->GetGPUVirtualAddress());
        commandList->SetComputeRootUnorderedAccessView(1, m_denoiseTempBuffer->GetGPUVirtualAddress());
    } else {
        commandList->SetComputeRootUnorderedAccessView(0, m_denoiseTempBuffer->GetGPUVirtualAddress());
        commandList->SetComputeRootUnorderedAccessView(1, m_outputBuffer->GetGPUVirtualAddress());
    }

    // Dispatch with 8x8 thread groups
    uint32_t groupsX = (width + 7) / 8;
    uint32_t groupsY = (height + 7) / 8;
    commandList->Dispatch(groupsX, groupsY, 1);
}

} // namespace RTX

#endif // ENABLE_DX12_RTX
