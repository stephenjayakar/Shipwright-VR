#ifdef ENABLE_DX12_RTX

#include "DXRPipeline.h"
#include "TextureManager.h"
#include <spdlog/spdlog.h>
#include <cstdio>
#include <cassert>
#include <fstream>
#include <filesystem>
#ifdef _WIN32
#include <Windows.h>
#endif

#include "RTXDiagLog.h"

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
    printf("[RTX] DXRPipeline::CreatePipeline() called (%ux%u)\n", width, height);
    RTX_DIAG("DXRPipeline::Initialize() starting (%ux%u)", width, height);
    m_device = device;

    RTX_DIAG("DXRPipeline: Creating global root signature...");
    if (!CreateGlobalRootSignature()) { RTX_DIAG("DXRPipeline: Global root sig FAILED"); return false; }
    RTX_DIAG("DXRPipeline: Global root signature created OK");

    RTX_DIAG("DXRPipeline: Creating local root signature...");
    if (!CreateLocalRootSignature()) { RTX_DIAG("DXRPipeline: Local root sig FAILED"); return false; }
    RTX_DIAG("DXRPipeline: Local root signature created OK");
    RTX_DIAG("DXRPipeline: Loading shaders...");
    if (!LoadShaders()) {
        printf("[RTX] Shaders FAILED to load\n");
        RTX_DIAG("DXRPipeline: Shader loading FAILED (pipeline incomplete)");
        SPDLOG_WARN("[RTX] Shader loading failed — pipeline will be created without shaders "
                     "(DispatchRays will be a no-op until shaders are available)");
    } else {
        printf("[RTX] Shaders loaded\n");
        RTX_DIAG("DXRPipeline: Shaders loaded OK");
    }

    RTX_DIAG("DXRPipeline: Creating raytracing pipeline state object...");
    if (!CreateRaytracingPipeline()) {
        printf("[RTX] Pipeline state creation FAILED/deferred\n");
        RTX_DIAG("DXRPipeline: PSO creation deferred/failed");
        SPDLOG_WARN("[RTX] Raytracing pipeline creation deferred — shaders may not be available yet");
    } else {
        printf("[RTX] Pipeline state created\n");
        RTX_DIAG("DXRPipeline: PSO created OK");
    }

    RTX_DIAG("DXRPipeline: Creating shader tables...");
    if (!CreateShaderTables()) {
        RTX_DIAG("DXRPipeline: Shader table creation deferred/failed");
        SPDLOG_WARN("[RTX] Shader table creation deferred — state object may not be available yet");
    } else {
        RTX_DIAG("DXRPipeline: Shader tables created OK");
    }

    RTX_DIAG("DXRPipeline: Creating constant buffer...");
    if (!CreateConstantBuffer()) { RTX_DIAG("DXRPipeline: Constant buffer FAILED"); return false; }
    RTX_DIAG("DXRPipeline: Constant buffer OK");

    RTX_DIAG("DXRPipeline: Creating output buffers (%ux%u)...", width, height);
    if (!CreateOutputBuffers(width, height)) { RTX_DIAG("DXRPipeline: Output buffers FAILED"); return false; }
    RTX_DIAG("DXRPipeline: Output buffers OK");

    RTX_DIAG("DXRPipeline: Creating denoise compute pipeline...");
    if (!CreateDenoiseComputePipeline()) {
        RTX_DIAG("DXRPipeline: Denoise pipeline deferred/failed");
        SPDLOG_WARN("[RTX] Denoise pipeline creation deferred — shader may not be available yet");
    } else {
        RTX_DIAG("DXRPipeline: Denoise pipeline OK");
    }

    SPDLOG_INFO("[RTX] DXR pipeline initialized");
    RTX_DIAG("DXRPipeline::Initialize() COMPLETE");
    return true;
}

void DXRPipeline::Shutdown() {
    RTX_DIAG("DXRPipeline::Shutdown() starting (uavCreated=%s, stateObject=%p)", m_uavDescriptorsCreated ? "yes" : "no", (void*)m_stateObject.Get());
    if (m_constantBufferMapped) {
        m_constantBuffer->Unmap(0, nullptr);
        m_constantBufferMapped = nullptr;
    }

    // Release shader blobs
    m_rayGenBlob.Reset();
    m_closestHitBlob.Reset();
    m_missBlob.Reset();
    m_anyHitBlob.Reset();
    m_denoiseBlob.Reset();

    // Release pipeline state objects
    m_stateObjectProperties.Reset();
    m_stateObject.Reset();
    m_denoisePipelineState.Reset();

    // Release shader tables
    m_rayGenShaderTable.Reset();
    m_missShaderTable.Reset();
    m_hitGroupShaderTable.Reset();

    // Release output buffers
    m_outputBuffer.Reset();
    m_accumulationBuffer.Reset();
    m_denoiseTempBuffer.Reset();

    // Release root signatures
    m_denoiseRootSignature.Reset();
    m_localRootSignature.Reset();
    m_globalRootSignature.Reset();

    // Release constant buffer
    m_constantBuffer.Reset();

    m_uavDescriptorsCreated = false;
    m_device = nullptr;
}

bool DXRPipeline::CreateGlobalRootSignature() {
    RTX_DIAG("DXRPipeline::CreateGlobalRootSignature() starting...");
    // Global root signature layout:
    // [0] CBV (b0)  - SceneConstants
    // [1] SRV (t0)  - Acceleration structure (TLAS)
    // [2] Descriptor Table - UAV u0 (output) + u1 (accumulation)
    // [3] Descriptor Table - SRV range (textures, t4+)
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

    // [2] Descriptor table for output UAVs (u0 + u1)
    // RWTexture2D must be bound via descriptor tables, not root descriptors.
    // Root UAV descriptors only work for raw/structured buffers.
    D3D12_DESCRIPTOR_RANGE uavRange = {};
    uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors = 2; // u0 (output) + u1 (accumulation)
    uavRange.BaseShaderRegister = 0;
    uavRange.RegisterSpace = 0;
    uavRange.OffsetInDescriptorsFromTableStart = 0;

    rootParams[GlobalRootParam_UAVTable].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[GlobalRootParam_UAVTable].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[GlobalRootParam_UAVTable].DescriptorTable.pDescriptorRanges = &uavRange;
    rootParams[GlobalRootParam_UAVTable].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // [3] Descriptor table for texture array (SRV t4+)
    // NumDescriptors must match TextureManager::MAX_TEXTURES (via MAX_BINDLESS_TEXTURES)
    // so shaders can access all loaded textures in the bindless SRV heap.
    D3D12_DESCRIPTOR_RANGE texRange = {};
    texRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    texRange.NumDescriptors = MAX_BINDLESS_TEXTURES; // Must match TextureManager heap size
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
        RTX_DIAG("DXRPipeline: Global root signature creation FAILED hr=0x%08X", (uint32_t)hr);
        SPDLOG_ERROR("[RTX] Failed to create global root signature: 0x{:08X}", (uint32_t)hr);
        return false;
    }

    RTX_DIAG("DXRPipeline: Global root signature created OK");
    return true;
}

bool DXRPipeline::CreateLocalRootSignature() {
    RTX_DIAG("DXRPipeline::CreateLocalRootSignature() starting...");
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
        RTX_DIAG("DXRPipeline: Local root signature creation FAILED hr=0x%08X", (uint32_t)hr);
        SPDLOG_ERROR("[RTX] Failed to create local root signature: 0x{:08X}", (uint32_t)hr);
        return false;
    }

    RTX_DIAG("DXRPipeline: Local root signature created OK");
    return true;
}

bool DXRPipeline::LoadShaders() {
    RTX_DIAG("DXRPipeline::LoadShaders() starting...");
    RTXShaderCompiler compiler;
    if (!compiler.Initialize()) {
        RTX_DIAG("DXRPipeline: Shader compiler initialization FAILED");
        SPDLOG_ERROR("[RTX] Failed to initialize shader compiler");
        return false;
    }
    RTX_DIAG("DXRPipeline: Shader compiler initialized OK");

    // Determine shader paths.
    // Precompiled .dxil files are placed by CMake in the RTX_COMPILED_SHADER_SUBDIR.
    // HLSL source files are in RTX_SHADER_SOURCE_DIR (set by CMake defines).
    // If neither CMake define is available, use relative paths from the executable.

    // Get the directory of the running executable to find shader output
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::filesystem::path exeDir = std::filesystem::path(exePath).parent_path();

    // Precompiled path: <exe>/shaders/<name>.cso
    // The CMake build compiles HLSL shaders to .cso files in the "shaders" subdirectory.
#ifdef RTX_COMPILED_SHADER_SUBDIR
    std::filesystem::path precompiledDir = exeDir / std::string(RTX_COMPILED_SHADER_SUBDIR);
#else
    std::filesystem::path precompiledDir = exeDir / L"shaders";
#endif

    // Source path: try compile-time define, then look relative to source tree
    std::filesystem::path sourceDir;
#ifdef RTX_SHADER_SOURCE_DIR
    // RTX_SHADER_SOURCE_DIR is defined by CMake as a quoted string: "path/to/shaders"
    // Use the helper macro to stringify it properly for std::filesystem::path
    sourceDir = std::filesystem::path(std::string(RTX_SHADER_SOURCE_DIR));
#endif
    if (sourceDir.empty() || !std::filesystem::exists(sourceDir)) {
        // Fallback: look for Shaders dir relative to executable
        sourceDir = exeDir / L"Shaders";
    }

    // Log shader search paths
    {
        char narrowPrecompiled[512] = {};
        char narrowSource[512] = {};
        wcstombs(narrowPrecompiled, precompiledDir.wstring().c_str(), sizeof(narrowPrecompiled) - 1);
        wcstombs(narrowSource, sourceDir.wstring().c_str(), sizeof(narrowSource) - 1);
        RTX_DIAG("DXRPipeline: Precompiled shader dir: %s", narrowPrecompiled);
        RTX_DIAG("DXRPipeline: HLSL source dir: %s", narrowSource);
        RTX_DIAG("DXRPipeline: Precompiled dir exists: %s",
                 std::filesystem::exists(precompiledDir) ? "yes" : "no");
    }

    // Load raytracing shaders as lib_6_3 (DXR library shaders)
    auto loadRT = [&](const std::wstring& name) -> ComPtr<IDxcBlob> {
        std::wstring precompiled = (precompiledDir / (name + L".cso")).wstring();
        std::wstring hlsl = (sourceDir / (name + L".hlsl")).wstring();
        char narrowName[128] = {};
        wcstombs(narrowName, name.c_str(), sizeof(narrowName) - 1);
        char narrowPath[512] = {};
        wcstombs(narrowPath, precompiled.c_str(), sizeof(narrowPath) - 1);
        bool csoExists = std::filesystem::exists(precompiled);
        RTX_DIAG("DXRPipeline: Loading shader '%s' (cso exists: %s, path: %s)",
                 narrowName, csoExists ? "yes" : "no", narrowPath);
        auto blob = compiler.LoadOrCompile(precompiled, hlsl, L"", L"lib_6_3", name);
        RTX_DIAG("DXRPipeline: Shader '%s' loaded: %s (size=%zu bytes)",
                 narrowName,
                 blob ? "OK" : "FAILED",
                 blob ? blob->GetBufferSize() : 0);
        return blob;
    };

    m_rayGenBlob = loadRT(L"RayGen");
    m_closestHitBlob = loadRT(L"ClosestHit");
    m_missBlob = loadRT(L"Miss");
    m_anyHitBlob = loadRT(L"AnyHit");

    // Load denoise shader as compute shader (cs_6_0)
    {
        std::wstring precompiled = (precompiledDir / L"Denoise.cso").wstring();
        std::wstring hlsl = (sourceDir / L"Denoise.hlsl").wstring();
        bool csoExists = std::filesystem::exists(precompiled);
        RTX_DIAG("DXRPipeline: Loading shader 'Denoise' (cso exists: %s)", csoExists ? "yes" : "no");
        m_denoiseBlob = compiler.LoadOrCompile(precompiled, hlsl, L"Denoise", L"cs_6_0", L"Denoise");
        RTX_DIAG("DXRPipeline: Shader 'Denoise' loaded: %s (size=%zu bytes)",
                 m_denoiseBlob ? "OK" : "FAILED",
                 m_denoiseBlob ? m_denoiseBlob->GetBufferSize() : 0);
    }

    bool allLoaded = m_rayGenBlob && m_closestHitBlob && m_missBlob && m_anyHitBlob;
    RTX_DIAG("DXRPipeline: Shader load summary: RayGen=%s ClosestHit=%s Miss=%s AnyHit=%s Denoise=%s",
             m_rayGenBlob ? "OK" : "MISSING",
             m_closestHitBlob ? "OK" : "MISSING",
             m_missBlob ? "OK" : "MISSING",
             m_anyHitBlob ? "OK" : "MISSING",
             m_denoiseBlob ? "OK" : "MISSING");

    if (!allLoaded) {
        SPDLOG_WARN("[RTX] Some raytracing shaders failed to load — pipeline will be incomplete. "
                     "RayGen={} ClosestHit={} Miss={} AnyHit={}",
                     m_rayGenBlob ? "OK" : "MISSING",
                     m_closestHitBlob ? "OK" : "MISSING",
                     m_missBlob ? "OK" : "MISSING",
                     m_anyHitBlob ? "OK" : "MISSING");
    }

    if (!m_denoiseBlob) {
        SPDLOG_WARN("[RTX] Denoise shader failed to load — denoising will be disabled");
    }

    return allLoaded;
}

bool DXRPipeline::CreateRaytracingPipeline() {
    RTX_DIAG("DXRPipeline::CreateRaytracingPipeline() starting...");
    // All four RT shader blobs are required to create the state object
    if (!m_rayGenBlob || !m_closestHitBlob || !m_missBlob || !m_anyHitBlob) {
        RTX_DIAG("DXRPipeline: Cannot create PSO - missing shader blobs");
        SPDLOG_WARN("[RTX] Cannot create raytracing pipeline — shader blobs not loaded");
        return false;
    }

    auto* d3dDevice = m_device->GetDevice();

    // We use CD3DX12_STATE_OBJECT_DESC-style building via raw D3D12_STATE_SUBOBJECT arrays.
    // Layout:
    //   [0] DXIL Library (RayGen)
    //   [1] DXIL Library (ClosestHit)
    //   [2] DXIL Library (Miss)
    //   [3] DXIL Library (AnyHit)
    //   [4] Hit Group ("HitGroup" = ClosestHit + AnyHit)
    //   [5] Shader Config (payload size, attribute size)
    //   [6] Pipeline Config (max recursion depth)
    //   [7] Global Root Signature
    //   [8] Local Root Signature
    //   [9] Local Root Signature Association

    // DXIL library descriptors
    D3D12_DXIL_LIBRARY_DESC rayGenLib = {};
    rayGenLib.DXILLibrary.pShaderBytecode = m_rayGenBlob->GetBufferPointer();
    rayGenLib.DXILLibrary.BytecodeLength = m_rayGenBlob->GetBufferSize();
    D3D12_EXPORT_DESC rayGenExport = { kRayGenExport, nullptr, D3D12_EXPORT_FLAG_NONE };
    rayGenLib.NumExports = 1;
    rayGenLib.pExports = &rayGenExport;

    D3D12_DXIL_LIBRARY_DESC closestHitLib = {};
    closestHitLib.DXILLibrary.pShaderBytecode = m_closestHitBlob->GetBufferPointer();
    closestHitLib.DXILLibrary.BytecodeLength = m_closestHitBlob->GetBufferSize();
    D3D12_EXPORT_DESC closestHitExport = { kClosestHitExport, nullptr, D3D12_EXPORT_FLAG_NONE };
    closestHitLib.NumExports = 1;
    closestHitLib.pExports = &closestHitExport;

    D3D12_DXIL_LIBRARY_DESC missLib = {};
    missLib.DXILLibrary.pShaderBytecode = m_missBlob->GetBufferPointer();
    missLib.DXILLibrary.BytecodeLength = m_missBlob->GetBufferSize();
    D3D12_EXPORT_DESC missExportDesc = { kMissExport, nullptr, D3D12_EXPORT_FLAG_NONE };
    missLib.NumExports = 1;
    missLib.pExports = &missExportDesc;

    D3D12_DXIL_LIBRARY_DESC anyHitLib = {};
    anyHitLib.DXILLibrary.pShaderBytecode = m_anyHitBlob->GetBufferPointer();
    anyHitLib.DXILLibrary.BytecodeLength = m_anyHitBlob->GetBufferSize();
    D3D12_EXPORT_DESC anyHitExportDesc = { kAnyHitExport, nullptr, D3D12_EXPORT_FLAG_NONE };
    anyHitLib.NumExports = 1;
    anyHitLib.pExports = &anyHitExportDesc;

    // Hit group: combines ClosestHit and AnyHit
    D3D12_HIT_GROUP_DESC hitGroup = {};
    hitGroup.HitGroupExport = kHitGroupName;
    hitGroup.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
    hitGroup.ClosestHitShaderImport = kClosestHitExport;
    hitGroup.AnyHitShaderImport = kAnyHitExport;
    hitGroup.IntersectionShaderImport = nullptr;

    // Shader config: payload and attribute sizes
    D3D12_RAYTRACING_SHADER_CONFIG shaderConfig = {};
    // RayPayload: float3 color + float distance + bool hit + uint recursionDepth
    // = 3*4 + 4 + 4 + 4 = 24 bytes (round up to be safe)
    shaderConfig.MaxPayloadSizeInBytes = 32;
    // BuiltInTriangleIntersectionAttributes: float2 barycentrics = 8 bytes
    shaderConfig.MaxAttributeSizeInBytes = 8;

    // Pipeline config: max recursion depth = 2 (primary + 1 GI bounce)
    D3D12_RAYTRACING_PIPELINE_CONFIG pipelineConfig = {};
    pipelineConfig.MaxTraceRecursionDepth = 2;

    // Global root signature subobject
    D3D12_GLOBAL_ROOT_SIGNATURE globalRootSig = {};
    globalRootSig.pGlobalRootSignature = m_globalRootSignature.Get();

    // Local root signature subobject and association with hit group
    D3D12_LOCAL_ROOT_SIGNATURE localRootSig = {};
    localRootSig.pLocalRootSignature = m_localRootSignature.Get();

    D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION localAssociation = {};
    const wchar_t* hitGroupExports[] = { kHitGroupName };
    localAssociation.NumExports = 1;
    localAssociation.pExports = hitGroupExports;
    // pSubobjectToAssociate is set below after we know the subobject pointer

    // Build the subobject array
    constexpr uint32_t NUM_SUBOBJECTS = 10;
    D3D12_STATE_SUBOBJECT subobjects[NUM_SUBOBJECTS] = {};

    subobjects[0].Type = D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY;
    subobjects[0].pDesc = &rayGenLib;

    subobjects[1].Type = D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY;
    subobjects[1].pDesc = &closestHitLib;

    subobjects[2].Type = D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY;
    subobjects[2].pDesc = &missLib;

    subobjects[3].Type = D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY;
    subobjects[3].pDesc = &anyHitLib;

    subobjects[4].Type = D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP;
    subobjects[4].pDesc = &hitGroup;

    subobjects[5].Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG;
    subobjects[5].pDesc = &shaderConfig;

    subobjects[6].Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG;
    subobjects[6].pDesc = &pipelineConfig;

    subobjects[7].Type = D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE;
    subobjects[7].pDesc = &globalRootSig;

    subobjects[8].Type = D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE;
    subobjects[8].pDesc = &localRootSig;

    // Associate local root signature with hit group
    localAssociation.pSubobjectToAssociate = &subobjects[8];
    subobjects[9].Type = D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION;
    subobjects[9].pDesc = &localAssociation;

    // Create the state object
    D3D12_STATE_OBJECT_DESC stateObjectDesc = {};
    stateObjectDesc.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
    stateObjectDesc.NumSubobjects = NUM_SUBOBJECTS;
    stateObjectDesc.pSubobjects = subobjects;

    RTX_DIAG("DXRPipeline: Creating state object with %u subobjects...", NUM_SUBOBJECTS);
    HRESULT hr = d3dDevice->CreateStateObject(&stateObjectDesc, IID_PPV_ARGS(&m_stateObject));
    if (FAILED(hr)) {
        RTX_DIAG("DXRPipeline: CreateStateObject FAILED hr=0x%08X", (uint32_t)hr);
        SPDLOG_ERROR("[RTX] Failed to create raytracing state object: 0x{:08X}", (uint32_t)hr);
        return false;
    }
    RTX_DIAG("DXRPipeline: State object created successfully");

    // Query state object properties for shader identifiers
    hr = m_stateObject.As(&m_stateObjectProperties);
    if (FAILED(hr)) {
        RTX_DIAG("DXRPipeline: QueryInterface for StateObjectProperties FAILED hr=0x%08X", (uint32_t)hr);
        SPDLOG_ERROR("[RTX] Failed to query ID3D12StateObjectProperties: 0x{:08X}", (uint32_t)hr);
        return false;
    }
    RTX_DIAG("DXRPipeline: StateObjectProperties obtained");

    SPDLOG_INFO("[RTX] Raytracing pipeline state object created successfully");
    RTX_DIAG("DXRPipeline::CreateRaytracingPipeline() SUCCESS");
    return true;
}

bool DXRPipeline::CreateShaderTables() {
    RTX_DIAG("DXRPipeline::CreateShaderTables() starting (stateObjectProperties=%p)", (void*)m_stateObjectProperties.Get());
    if (!m_stateObjectProperties) {
        // State object not yet created; calculate record sizes for later use
        m_rayGenRecordSize = Align(D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES, D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT);
        m_missRecordSize = Align(D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES, D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT);
        m_hitGroupRecordSize = Align(
            D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES + LocalRootParam_Count * sizeof(D3D12_GPU_VIRTUAL_ADDRESS),
            D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT
        );
        RTX_DIAG("DXRPipeline: Shader table creation deferred - no state object. Record sizes: RayGen=%u, Miss=%u, HitGroup=%u",
                 m_rayGenRecordSize, m_missRecordSize, m_hitGroupRecordSize);
        SPDLOG_INFO("[RTX] Shader table record sizes calculated (GPU allocation deferred): "
                     "RayGen={}, Miss={}, HitGroup={}",
                     m_rayGenRecordSize, m_missRecordSize, m_hitGroupRecordSize);
        return false;
    }

    auto* d3dDevice = m_device->GetDevice();

    // Record sizes (aligned to D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT = 32)
    m_rayGenRecordSize = Align(D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES, D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT);
    m_missRecordSize = Align(D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES, D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT);
    m_hitGroupRecordSize = Align(
        D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES + LocalRootParam_Count * sizeof(D3D12_GPU_VIRTUAL_ADDRESS),
        D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT
    );

    // Helper lambda to create an upload heap buffer
    auto createUploadBuffer = [&](uint32_t size, ComPtr<ID3D12Resource>& outBuffer) -> bool {
        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC bufDesc = {};
        bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufDesc.Width = size;
        bufDesc.Height = 1;
        bufDesc.DepthOrArraySize = 1;
        bufDesc.MipLevels = 1;
        bufDesc.SampleDesc.Count = 1;
        bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        HRESULT hr = d3dDevice->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &bufDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&outBuffer));
        return SUCCEEDED(hr);
    };

    // Get shader identifiers
    void* rayGenID = m_stateObjectProperties->GetShaderIdentifier(kRayGenExport);
    void* missID = m_stateObjectProperties->GetShaderIdentifier(kMissExport);
    void* hitGroupID = m_stateObjectProperties->GetShaderIdentifier(kHitGroupName);

    RTX_DIAG("DXRPipeline: Shader identifiers: RayGen=%p, Miss=%p, HitGroup=%p",
             rayGenID, missID, hitGroupID);
    if (!rayGenID || !missID || !hitGroupID) {
        RTX_DIAG("DXRPipeline: FAILED to get shader identifiers!");
        SPDLOG_ERROR("[RTX] Failed to get shader identifiers from state object");
        return false;
    }

    // ---- Ray Generation shader table (1 record) ----
    {
        uint32_t tableSize = Align(m_rayGenRecordSize, D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT);
        if (!createUploadBuffer(tableSize, m_rayGenShaderTable)) {
            SPDLOG_ERROR("[RTX] Failed to create ray gen shader table buffer");
            return false;
        }

        void* mapped = nullptr;
        m_rayGenShaderTable->Map(0, nullptr, &mapped);
        memcpy(mapped, rayGenID, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
        m_rayGenShaderTable->Unmap(0, nullptr);
    }

    // ---- Miss shader table (1 record) ----
    {
        uint32_t tableSize = Align(m_missRecordSize, D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT);
        if (!createUploadBuffer(tableSize, m_missShaderTable)) {
            SPDLOG_ERROR("[RTX] Failed to create miss shader table buffer");
            return false;
        }

        void* mapped = nullptr;
        m_missShaderTable->Map(0, nullptr, &mapped);
        memcpy(mapped, missID, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
        m_missShaderTable->Unmap(0, nullptr);
    }

    // ---- Hit Group shader table ----
    // Pre-allocate space for up to 6 geometry entries (3 rooms * 2 geometries each: opaque + alpha).
    // The local root arguments (vertex buffer, index buffer, material ID buffer, material table
    // GPU addresses) are initially zeroed; they are updated when geometry is loaded.
    {
        constexpr uint32_t MAX_HIT_RECORDS = 6;
        uint32_t tableSize = Align(m_hitGroupRecordSize * MAX_HIT_RECORDS,
                                    D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT);
        if (!createUploadBuffer(tableSize, m_hitGroupShaderTable)) {
            SPDLOG_ERROR("[RTX] Failed to create hit group shader table buffer");
            return false;
        }

        uint8_t* mapped = nullptr;
        m_hitGroupShaderTable->Map(0, nullptr, (void**)&mapped);
        // Fill all records with the hit group shader ID and zero local root args
        for (uint32_t i = 0; i < MAX_HIT_RECORDS; i++) {
            uint8_t* record = mapped + i * m_hitGroupRecordSize;
            memcpy(record, hitGroupID, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
            // Local root arguments (4 GPU virtual addresses) are zeroed by the upload heap
        }
        m_hitGroupShaderTable->Unmap(0, nullptr);
    }

    RTX_DIAG("DXRPipeline::CreateShaderTables() SUCCESS - RayGen=%u, Miss=%u, HitGroup=%u (record sizes)",
             m_rayGenRecordSize, m_missRecordSize, m_hitGroupRecordSize);
    SPDLOG_INFO("[RTX] Shader tables created: RayGen={}, Miss={}, HitGroup={} (record sizes)",
                m_rayGenRecordSize, m_missRecordSize, m_hitGroupRecordSize);
    return true;
}

bool DXRPipeline::CreateConstantBuffer() {
    RTX_DIAG("DXRPipeline::CreateConstantBuffer() starting (size=%zu bytes)", sizeof(SceneConstants));
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
        RTX_DIAG("DXRPipeline: Constant buffer creation FAILED hr=0x%08X", (uint32_t)hr);
        SPDLOG_ERROR("[RTX] Failed to create constant buffer: 0x{:08X}", (uint32_t)hr);
        return false;
    }
    RTX_DIAG("DXRPipeline: Constant buffer created OK");

    // Map persistently (upload heap stays mapped)
    hr = m_constantBuffer->Map(0, nullptr, &m_constantBufferMapped);
    if (FAILED(hr)) {
        RTX_DIAG("DXRPipeline: Constant buffer map FAILED hr=0x%08X", (uint32_t)hr);
        SPDLOG_ERROR("[RTX] Failed to map constant buffer: 0x{:08X}", (uint32_t)hr);
        return false;
    }

    RTX_DIAG("DXRPipeline::CreateConstantBuffer() SUCCESS (mapped=%p)", m_constantBufferMapped);
    return true;
}

bool DXRPipeline::CreateOutputBuffers(uint32_t width, uint32_t height) {
    RTX_DIAG("DXRPipeline::CreateOutputBuffers() %ux%u", width, height);
    auto device = m_device->GetDevice();

    // Helper to create a UAV-capable texture with a specific format.
    auto createUAVTexture = [&](ComPtr<ID3D12Resource>& resource, const char* name,
                                DXGI_FORMAT format) -> bool {
        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC texDesc = {};
        texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texDesc.Width = width;
        texDesc.Height = height;
        texDesc.DepthOrArraySize = 1;
        texDesc.MipLevels = 1;
        texDesc.Format = format;
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

    // Output and denoise temp buffers use R8G8B8A8_UNORM to match the swap chain
    // format (DXGI_FORMAT_R8G8B8A8_UNORM), enabling direct CopyResource to the
    // back buffer without format conversion. The HLSL shaders write float4 values
    // which are automatically clamped to [0,1] and converted to UNORM on write.
    //
    // The accumulation buffer uses R32G32B32A32_FLOAT to preserve full precision
    // for temporal accumulation (values may exceed 1.0 during blending).
    if (!createUAVTexture(m_outputBuffer, "output", DXGI_FORMAT_R8G8B8A8_UNORM)) return false;
    if (!createUAVTexture(m_accumulationBuffer, "accumulation", DXGI_FORMAT_R32G32B32A32_FLOAT)) return false;
    if (!createUAVTexture(m_denoiseTempBuffer, "denoise temp", DXGI_FORMAT_R8G8B8A8_UNORM)) return false;

    // NOTE: UAV descriptors are NOT created here because TextureManager may not
    // be initialized yet (the SRV heap lives in TextureManager). Instead,
    // CreateUAVDescriptors() must be called explicitly AFTER TextureManager is
    // initialized. RTXRenderer::Initialize() handles this ordering.
    // See CreateUAVDescriptors() for the actual UAV descriptor creation.

    SPDLOG_INFO("[RTX] Output buffers created ({}x{})", width, height);
    return true;
}

bool DXRPipeline::CreateDenoiseComputePipeline() {
    // The denoise pipeline uses a compute shader root signature:
    // [0] Descriptor Table - UAV u0 (input) + u1 (output) as contiguous pair
    // [1] 32-bit Constants b0 - DenoiseConstants
    //
    // RWTexture2D must be bound via descriptor tables, not root UAV descriptors.
    // Ping-pong is achieved by binding different descriptor table offsets
    // (even pass: u0=output, u1=temp; odd pass: u0=temp, u1=output).

    D3D12_DESCRIPTOR_RANGE denoiseUavRange = {};
    denoiseUavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    denoiseUavRange.NumDescriptors = 2; // u0 (input) + u1 (output)
    denoiseUavRange.BaseShaderRegister = 0;
    denoiseUavRange.RegisterSpace = 0;
    denoiseUavRange.OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER denoiseParams[2] = {};

    denoiseParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    denoiseParams[0].DescriptorTable.NumDescriptorRanges = 1;
    denoiseParams[0].DescriptorTable.pDescriptorRanges = &denoiseUavRange;
    denoiseParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    denoiseParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    denoiseParams[1].Constants.ShaderRegister = 0;
    denoiseParams[1].Constants.Num32BitValues = sizeof(DenoiseConstants) / 4;
    denoiseParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC denoiseSigDesc = {};
    denoiseSigDesc.NumParameters = 2;
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
        RTX_DIAG("DXRPipeline: Denoise root signature creation FAILED hr=0x%08X", (uint32_t)hr);
        SPDLOG_ERROR("[RTX] Failed to create denoise root signature: 0x{:08X}", (uint32_t)hr);
        return false;
    }
    RTX_DIAG("DXRPipeline: Denoise root signature created OK");

    // Create the compute PSO if the denoise shader blob is available
    if (m_denoiseBlob) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.pRootSignature = m_denoiseRootSignature.Get();
        psoDesc.CS.pShaderBytecode = m_denoiseBlob->GetBufferPointer();
        psoDesc.CS.BytecodeLength = m_denoiseBlob->GetBufferSize();

        hr = m_device->GetDevice()->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&m_denoisePipelineState));
        if (FAILED(hr)) {
            RTX_DIAG("DXRPipeline: Denoise compute PSO creation FAILED hr=0x%08X", (uint32_t)hr);
            SPDLOG_ERROR("[RTX] Failed to create denoise compute PSO: 0x{:08X}", (uint32_t)hr);
            // Non-fatal: denoising will be disabled
        } else {
            RTX_DIAG("DXRPipeline: Denoise compute PSO created OK");
            SPDLOG_INFO("[RTX] Denoise compute pipeline created successfully");
        }
    } else {
        SPDLOG_WARN("[RTX] Denoise shader blob not available — PSO creation deferred");
    }

    SPDLOG_INFO("[RTX] Denoise compute pipeline root signature created");
    return true;
}

void DXRPipeline::UpdateHitGroupShaderTable(const std::vector<GeometryBufferAddresses>& geometries) {
    RTX_DIAG("DXRPipeline::UpdateHitGroupShaderTable() %zu geometries", geometries.size());
    if (!m_hitGroupShaderTable || !m_stateObjectProperties) {
        RTX_DIAG("DXRPipeline: UpdateHitGroupShaderTable skipped - table=%p, stateObj=%p",
                 (void*)m_hitGroupShaderTable.Get(), (void*)m_stateObjectProperties.Get());
        SPDLOG_WARN("[RTX] UpdateHitGroupShaderTable: shader table or state object not ready");
        return;
    }

    constexpr uint32_t MAX_HIT_RECORDS = 6;
    uint32_t count = static_cast<uint32_t>(geometries.size());
    if (count > MAX_HIT_RECORDS) {
        SPDLOG_WARN("[RTX] UpdateHitGroupShaderTable: {} geometries exceeds max {} — clamping",
                     count, MAX_HIT_RECORDS);
        count = MAX_HIT_RECORDS;
    }

    void* hitGroupID = m_stateObjectProperties->GetShaderIdentifier(kHitGroupName);
    if (!hitGroupID) {
        SPDLOG_ERROR("[RTX] UpdateHitGroupShaderTable: failed to get hit group shader identifier");
        return;
    }

    uint8_t* mapped = nullptr;
    m_hitGroupShaderTable->Map(0, nullptr, (void**)&mapped);
    if (!mapped) {
        SPDLOG_ERROR("[RTX] UpdateHitGroupShaderTable: failed to map hit group shader table");
        return;
    }

    for (uint32_t i = 0; i < count; i++) {
        uint8_t* record = mapped + i * m_hitGroupRecordSize;
        // Write shader identifier
        memcpy(record, hitGroupID, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
        // Write local root arguments (4 GPU virtual addresses) after the shader ID
        D3D12_GPU_VIRTUAL_ADDRESS* localArgs = reinterpret_cast<D3D12_GPU_VIRTUAL_ADDRESS*>(
            record + D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
        localArgs[LocalRootParam_VertexBuffer]    = geometries[i].vertexBuffer;
        localArgs[LocalRootParam_IndexBuffer]     = geometries[i].indexBuffer;
        localArgs[LocalRootParam_MaterialIDBuffer] = geometries[i].materialIDBuffer;
        localArgs[LocalRootParam_MaterialTable]   = geometries[i].materialTable;
    }

    m_hitGroupShaderTable->Unmap(0, nullptr);
    m_activeHitGroupCount = count;

    SPDLOG_INFO("[RTX] Updated hit group shader table with {} geometry records", count);
}

void DXRPipeline::UpdateSceneConstants(const SceneConstants& constants) {
    if (m_constantBufferMapped) {
        memcpy(m_constantBufferMapped, &constants, sizeof(SceneConstants));
    }
}

void DXRPipeline::DispatchRays(ID3D12GraphicsCommandList4* commandList, uint32_t width, uint32_t height,
                               D3D12_GPU_VIRTUAL_ADDRESS tlasAddress,
                               D3D12_GPU_DESCRIPTOR_HANDLE textureTableGPU) {
    static uint32_t s_dispatchCount = 0;
    s_dispatchCount++;
    if (!m_stateObject) {
        // Pipeline not ready yet (shaders not loaded)
        if (s_dispatchCount <= 5) {
            RTX_DIAG("DXRPipeline::DispatchRays() skipped #%u - no state object (shaders not loaded)", s_dispatchCount);
        }
        return;
    }

    if (!m_rayGenShaderTable || !m_missShaderTable || !m_hitGroupShaderTable) {
        // Shader tables not created yet
        if (s_dispatchCount <= 5) {
            RTX_DIAG("DXRPipeline::DispatchRays() skipped #%u - shader tables not ready (rayGen=%p, miss=%p, hitGroup=%p)",
                     s_dispatchCount, (void*)m_rayGenShaderTable.Get(), (void*)m_missShaderTable.Get(), (void*)m_hitGroupShaderTable.Get());
        }
        return;
    }
    if (s_dispatchCount <= 5 || (s_dispatchCount % 300) == 0) {
        RTX_DIAG("DXRPipeline::DispatchRays() #%u: %ux%u, TLAS=0x%llX, texTable=0x%llX, hitGroups=%u, uavReady=%s",
                 s_dispatchCount, width, height,
                 (unsigned long long)tlasAddress, (unsigned long long)textureTableGPU.ptr,
                 m_activeHitGroupCount, m_uavDescriptorsCreated ? "yes" : "no");
    }

    // Set global root signature and resources
    commandList->SetComputeRootSignature(m_globalRootSignature.Get());
    commandList->SetComputeRootConstantBufferView(GlobalRootParam_SceneConstants,
                                                   m_constantBuffer->GetGPUVirtualAddress());

    // Bind TLAS to root param 1 (SRV t0, space0)
    if (tlasAddress != 0) {
        commandList->SetComputeRootShaderResourceView(GlobalRootParam_AccelerationStructure,
                                                       tlasAddress);
    }

    // Bind output UAV descriptor table (u0 + u1) via descriptor table.
    // RWTexture2D resources must use descriptor tables, not root descriptors.
    // The UAV descriptors are created in the DX12Device's UAV heap or the
    // TextureManager's SRV heap at known offsets.
    if (m_uavDescriptorsCreated && m_outputBufferUAV.ptr != 0) {
        commandList->SetComputeRootDescriptorTable(GlobalRootParam_UAVTable, m_outputBufferUAV);
    }

    // Bind the bindless texture SRV table from TextureManager (root param 3)
    if (textureTableGPU.ptr != 0) {
        commandList->SetComputeRootDescriptorTable(GlobalRootParam_TextureTable, textureTableGPU);
    }

    // Build dispatch desc
    D3D12_DISPATCH_RAYS_DESC dispatchDesc = {};

    // Ray generation
    dispatchDesc.RayGenerationShaderRecord.StartAddress = m_rayGenShaderTable->GetGPUVirtualAddress();
    dispatchDesc.RayGenerationShaderRecord.SizeInBytes = m_rayGenRecordSize;

    // Miss
    dispatchDesc.MissShaderTable.StartAddress = m_missShaderTable->GetGPUVirtualAddress();
    dispatchDesc.MissShaderTable.SizeInBytes = m_missRecordSize;
    dispatchDesc.MissShaderTable.StrideInBytes = m_missRecordSize;

    // Hit group — SizeInBytes must cover all active geometry records.
    // The SizeInBytes must be aligned to D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT (64 bytes).
    dispatchDesc.HitGroupTable.StartAddress = m_hitGroupShaderTable->GetGPUVirtualAddress();
    dispatchDesc.HitGroupTable.SizeInBytes = Align(
        m_hitGroupRecordSize * (m_activeHitGroupCount > 0 ? m_activeHitGroupCount : 1),
        D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT);
    dispatchDesc.HitGroupTable.StrideInBytes = m_hitGroupRecordSize;

    // Dispatch dimensions
    dispatchDesc.Width = width;
    dispatchDesc.Height = height;
    dispatchDesc.Depth = 1;

    commandList->SetPipelineState1(m_stateObject.Get());
    commandList->DispatchRays(&dispatchDesc);
}

void DXRPipeline::DispatchDenoise(ID3D12GraphicsCommandList4* commandList, uint32_t width, uint32_t height, int pass) {
    // Default denoise constants (used when GISystem is not available)
    DenoiseConstants dc = {};
    dc.stepSize = 1 << pass;      // 1, 2, 4 for passes 0, 1, 2
    dc.colorSigma = 0.1f;
    dc.normalSigma = 0.1f;
    dc._pad = 0.0f;
    DispatchDenoise(commandList, width, height, pass, dc);
}

void DXRPipeline::DispatchDenoise(ID3D12GraphicsCommandList4* commandList, uint32_t width, uint32_t height,
                                   int passIndex, const DenoiseConstants& constants) {
    if (!m_denoisePipelineState || !m_uavDescriptorsCreated) return;

    commandList->SetComputeRootSignature(m_denoiseRootSignature.Get());
    commandList->SetPipelineState(m_denoisePipelineState.Get());

    // Set denoise constants from caller (GISystem provides per-scene tuned values)
    // Root param [1] = 32-bit constants b0
    commandList->SetComputeRoot32BitConstants(1, sizeof(DenoiseConstants) / 4, &constants, 0);

    // Ping-pong between output buffer and temp buffer via descriptor tables.
    // Each table is a contiguous pair of UAV descriptors (u0, u1).
    // Even pass indices (0, 2, ...): u0=output, u1=temp  (m_denoiseEvenPassTable)
    // Odd pass indices  (1, 3, ...): u0=temp,   u1=output (m_denoiseOddPassTable)
    // Root param [0] = descriptor table for UAVs u0+u1
    if (passIndex % 2 == 0) {
        commandList->SetComputeRootDescriptorTable(0, m_denoiseEvenPassTable);
    } else {
        commandList->SetComputeRootDescriptorTable(0, m_denoiseOddPassTable);
    }

    // Dispatch with 8x8 thread groups
    uint32_t groupsX = (width + 7) / 8;
    uint32_t groupsY = (height + 7) / 8;
    commandList->Dispatch(groupsX, groupsY, 1);
}

bool DXRPipeline::CreateUAVDescriptors() {
    RTX_DIAG("DXRPipeline::CreateUAVDescriptors() starting (already created=%s)", m_uavDescriptorsCreated ? "yes" : "no");
    // This method must be called AFTER TextureManager::Initialize() so that the
    // TextureManager's SRV heap is available. UAV descriptors for the raytracing
    // output, accumulation, and denoise temp buffers are placed at reserved slots
    // at the end of the shared SRV heap so they are accessible during DispatchRays
    // (DX12 only allows one CBV_SRV_UAV heap at a time).
    if (m_uavDescriptorsCreated) {
        RTX_DIAG("DXRPipeline::CreateUAVDescriptors() already created, returning true");
        return true; // Already created
    }

    if (!m_device || !m_outputBuffer || !m_accumulationBuffer || !m_denoiseTempBuffer) {
        RTX_DIAG("DXRPipeline::CreateUAVDescriptors() FAILED - missing buffers (device=%p, output=%p, accum=%p, denoiseTemp=%p)",
                 (void*)m_device, (void*)m_outputBuffer.Get(), (void*)m_accumulationBuffer.Get(), (void*)m_denoiseTempBuffer.Get());
        SPDLOG_WARN("[RTX] CreateUAVDescriptors: output buffers not yet created");
        return false;
    }

    auto& texMgr = TextureManager::GetInstance();
    ID3D12DescriptorHeap* srvHeap = texMgr.GetSRVHeap();
    auto* d3dDevice = m_device->GetDevice();

    if (!srvHeap || !d3dDevice) {
        RTX_DIAG("DXRPipeline::CreateUAVDescriptors() FAILED - srvHeap=%p, d3dDevice=%p", (void*)srvHeap, (void*)d3dDevice);
        SPDLOG_WARN("[RTX] CreateUAVDescriptors: TextureManager SRV heap or device not available");
        return false;
    }
    RTX_DIAG("DXRPipeline::CreateUAVDescriptors() creating descriptors in TextureManager SRV heap...");

    uint32_t descriptorSize = d3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_CPU_DESCRIPTOR_HANDLE heapStartCPU = srvHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE heapStartGPU = srvHeap->GetGPUDescriptorHandleForHeapStart();

    // UAV desc for output buffer (R8G8B8A8_UNORM — matches swap chain for CopyResource)
    D3D12_UNORDERED_ACCESS_VIEW_DESC outputUavDesc = {};
    outputUavDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    outputUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    outputUavDesc.Texture2D.MipSlice = 0;
    outputUavDesc.Texture2D.PlaneSlice = 0;

    // UAV desc for accumulation buffer (R32G32B32A32_FLOAT — full precision for temporal blending)
    D3D12_UNORDERED_ACCESS_VIEW_DESC accumUavDesc = {};
    accumUavDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    accumUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    accumUavDesc.Texture2D.MipSlice = 0;
    accumUavDesc.Texture2D.PlaneSlice = 0;

    // Output UAV at index UAV_DESCRIPTOR_START (u0: R8G8B8A8_UNORM)
    D3D12_CPU_DESCRIPTOR_HANDLE outputCPU;
    outputCPU.ptr = heapStartCPU.ptr + UAV_DESCRIPTOR_START * descriptorSize;
    d3dDevice->CreateUnorderedAccessView(m_outputBuffer.Get(), nullptr, &outputUavDesc, outputCPU);
    m_outputBufferUAV.ptr = heapStartGPU.ptr + UAV_DESCRIPTOR_START * descriptorSize;

    // Accumulation UAV at index UAV_DESCRIPTOR_START+1 (u1: R32G32B32A32_FLOAT)
    D3D12_CPU_DESCRIPTOR_HANDLE accumCPU;
    accumCPU.ptr = heapStartCPU.ptr + (UAV_DESCRIPTOR_START + 1) * descriptorSize;
    d3dDevice->CreateUnorderedAccessView(m_accumulationBuffer.Get(), nullptr, &accumUavDesc, accumCPU);
    m_accumulationBufferUAV.ptr = heapStartGPU.ptr + (UAV_DESCRIPTOR_START + 1) * descriptorSize;

    // Denoise temp UAV at index UAV_DESCRIPTOR_START+2 (R8G8B8A8_UNORM — same as output)
    D3D12_CPU_DESCRIPTOR_HANDLE tempCPU;
    tempCPU.ptr = heapStartCPU.ptr + (UAV_DESCRIPTOR_START + 2) * descriptorSize;
    d3dDevice->CreateUnorderedAccessView(m_denoiseTempBuffer.Get(), nullptr, &outputUavDesc, tempCPU);
    m_denoiseTempBufferUAV.ptr = heapStartGPU.ptr + (UAV_DESCRIPTOR_START + 2) * descriptorSize;

    // Create denoise ping-pong descriptor pairs (contiguous u0+u1)
    // Both output and temp are R8G8B8A8_UNORM for denoise ping-pong.
    // Even pass: u0=output, u1=temp (at indices DENOISE_EVEN_PASS_START, +1)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE evenU0;
        evenU0.ptr = heapStartCPU.ptr + DENOISE_EVEN_PASS_START * descriptorSize;
        d3dDevice->CreateUnorderedAccessView(m_outputBuffer.Get(), nullptr, &outputUavDesc, evenU0);

        D3D12_CPU_DESCRIPTOR_HANDLE evenU1;
        evenU1.ptr = heapStartCPU.ptr + (DENOISE_EVEN_PASS_START + 1) * descriptorSize;
        d3dDevice->CreateUnorderedAccessView(m_denoiseTempBuffer.Get(), nullptr, &outputUavDesc, evenU1);

        m_denoiseEvenPassTable.ptr = heapStartGPU.ptr + DENOISE_EVEN_PASS_START * descriptorSize;
    }

    // Odd pass: u0=temp, u1=output (at indices DENOISE_ODD_PASS_START, +1)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE oddU0;
        oddU0.ptr = heapStartCPU.ptr + DENOISE_ODD_PASS_START * descriptorSize;
        d3dDevice->CreateUnorderedAccessView(m_denoiseTempBuffer.Get(), nullptr, &outputUavDesc, oddU0);

        D3D12_CPU_DESCRIPTOR_HANDLE oddU1;
        oddU1.ptr = heapStartCPU.ptr + (DENOISE_ODD_PASS_START + 1) * descriptorSize;
        d3dDevice->CreateUnorderedAccessView(m_outputBuffer.Get(), nullptr, &outputUavDesc, oddU1);

        m_denoiseOddPassTable.ptr = heapStartGPU.ptr + DENOISE_ODD_PASS_START * descriptorSize;
    }

    m_uavDescriptorsCreated = true;
    SPDLOG_INFO("[RTX] UAV descriptors created in TextureManager SRV heap at indices {}-{}, denoise pairs at {}-{} and {}-{}",
                UAV_DESCRIPTOR_START, UAV_DESCRIPTOR_START + 2,
                DENOISE_EVEN_PASS_START, DENOISE_EVEN_PASS_START + 1,
                DENOISE_ODD_PASS_START, DENOISE_ODD_PASS_START + 1);
    return true;
}

D3D12_GPU_DESCRIPTOR_HANDLE DXRPipeline::GetOutputUAV() const {
    if (m_uavDescriptorsCreated) {
        return m_outputBufferUAV;
    }
    
    // Return null handle if descriptors haven't been created
    D3D12_GPU_DESCRIPTOR_HANDLE nullHandle = {};
    return nullHandle;
}

} // namespace RTX

#endif // ENABLE_DX12_RTX
