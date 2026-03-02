// Minimal DXR test harness.
// Creates a DX12 device with debug layer, loads precompiled shaders,
// creates a raytracing state object, builds a simple triangle BLAS/TLAS,
// dispatches rays, and writes the output to rtx_test_output.bmp.
//
// Build: cl /EHsc /std:c++17 rtx_test.cpp /link d3d12.lib dxgi.lib dxguid.lib
// Run from x64/Release/ so it finds shaders/ subdirectory.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <dxcapi.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <vector>
#include <fstream>
#include <filesystem>
#include <string>

using Microsoft::WRL::ComPtr;

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib")

static const uint32_t WIDTH = 256;
static const uint32_t HEIGHT = 256;

// Helper to check HRESULT and print error
#define CHK(hr, msg) do { \
    HRESULT _hr = (hr); \
    if (FAILED(_hr)) { \
        printf("[FAIL] %s: 0x%08X\n", msg, (uint32_t)_hr); \
        return 1; \
    } else { \
        printf("[OK]   %s\n", msg); \
    } \
} while(0)

#define CHK_WARN(hr, msg) do { \
    HRESULT _hr = (hr); \
    if (FAILED(_hr)) { \
        printf("[WARN] %s: 0x%08X\n", msg, (uint32_t)_hr); \
    } else { \
        printf("[OK]   %s\n", msg); \
    } \
} while(0)

static uint64_t Align(uint64_t size, uint64_t alignment) {
    return (size + alignment - 1) & ~(alignment - 1);
}

// Read a binary file into a vector
static std::vector<uint8_t> ReadFile(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open()) return {};
    size_t sz = (size_t)f.tellg();
    f.seekg(0);
    std::vector<uint8_t> data(sz);
    f.read((char*)data.data(), sz);
    return data;
}

// Write a simple BMP file
static void WriteBMP(const char* path, uint32_t w, uint32_t h, const uint8_t* rgba) {
    uint32_t rowBytes = w * 3;
    uint32_t rowPad = (4 - (rowBytes % 4)) % 4;
    uint32_t dataSize = (rowBytes + rowPad) * h;
    uint32_t fileSize = 54 + dataSize;

    uint8_t header[54] = {};
    header[0] = 'B'; header[1] = 'M';
    *(uint32_t*)(header + 2) = fileSize;
    *(uint32_t*)(header + 10) = 54;
    *(uint32_t*)(header + 14) = 40;
    *(int32_t*)(header + 18) = (int32_t)w;
    *(int32_t*)(header + 22) = (int32_t)h;
    *(uint16_t*)(header + 26) = 1;
    *(uint16_t*)(header + 28) = 24;
    *(uint32_t*)(header + 34) = dataSize;

    FILE* fp = fopen(path, "wb");
    if (!fp) { printf("Failed to write %s\n", path); return; }
    fwrite(header, 1, 54, fp);
    // BMP is bottom-up, BGR
    for (int y = (int)h - 1; y >= 0; y--) {
        for (uint32_t x = 0; x < w; x++) {
            const uint8_t* px = rgba + (y * w + x) * 4;
            uint8_t bgr[3] = { px[2], px[1], px[0] };
            fwrite(bgr, 1, 3, fp);
        }
        uint8_t pad[3] = {};
        fwrite(pad, 1, rowPad, fp);
    }
    fclose(fp);
    printf("[OK]   Wrote %s (%ux%u)\n", path, w, h);
}

// SceneConstants matching Common.hlsli EXACTLY
// Must match struct layout in the HLSL shader.
// Camera basis vectors are stored explicitly (not as matrices) to avoid
// column-major / row-major layout mismatches between C++ and HLSL.
// Each float3 + float scalar packs into one 16-byte constant buffer slot.
//
// BYTE OFFSET MAP (matches RTXTypes.h and Common.hlsli):
//   offset   0: camRight[3] + tanHalfFovX       (16 bytes)
//   offset  16: camUp[3] + tanHalfFovY           (16 bytes)
//   offset  32: camForward[3] + frameCount       (16 bytes)
//   offset  48: cameraPos[3] + _pad0             (16 bytes)
//   offset  64: ambientColor[3] + fogNear        (16 bytes)
//   offset  80: fogColor[3] + fogFar             (16 bytes)
//   offset  96: sunDirection1[3] + time           (16 bytes)
//   offset 112: sunColor1[3] + dekuTreeAlpha      (16 bytes)
//   offset 128: sunDirection2[3] + fogBlendAlpha  (16 bytes)
//   offset 144: sunColor2[3] + waterScrollOffset  (16 bytes)
//   offset 160: giIntensity + baseReflectivity + roughnessScale + emissiveScale (16 bytes)
//   offset 176: waterReflectivity + waterRoughness + aoRadius + aoIntensity     (16 bytes)
//   offset 192: skyZenithColor[3] + ambientMinIntensity (16 bytes)
//   offset 208: skyHorizonColor[3] + exposure           (16 bytes)
//   Total: 224 bytes used, 256 bytes with alignment padding.
struct alignas(256) SceneConstants {
    // float3 camRight + float tanHalfFovX (16 bytes)
    float camRight[3];
    float tanHalfFovX;
    // float3 camUp + float tanHalfFovY (16 bytes)
    float camUp[3];
    float tanHalfFovY;
    // float3 camForward + uint frameCount (16 bytes)
    float camForward[3];
    uint32_t frameCount;
    // float3 cameraPos + float _pad0 (16 bytes)
    float cameraPos[3];
    float _pad0;
    // float3 ambientColor + float fogNear (16 bytes)
    float ambientColor[3];
    float fogNear;
    // float3 fogColor + float fogFar (16 bytes)
    float fogColor[3];
    float fogFar;
    // float3 sunDirection1 + float time (16 bytes)
    float sunDirection1[3];
    float time;
    // float3 sunColor1 + float dekuTreeAlpha (16 bytes)
    float sunColor1[3];
    float dekuTreeAlpha;
    // float3 sunDirection2 + float fogBlendAlpha (16 bytes)
    float sunDirection2[3];
    float fogBlendAlpha;
    // float3 sunColor2 + float waterScrollOffset (16 bytes)
    float sunColor2[3];
    float waterScrollOffset;
    // Per-scene material overrides (32 bytes)
    float giIntensity;
    float baseReflectivity;
    float roughnessScale;
    float emissiveScale;
    float waterReflectivity;
    float waterRoughness;
    float aoRadius;
    float aoIntensity;
    // Sky gradient colors (32 bytes)
    float skyZenithColor[3];
    float ambientMinIntensity;
    float skyHorizonColor[3];
    float exposure;
};

// RTXVertex matching Common.hlsli
struct RTXVertex {
    float position[3];
    float normal[3];
    float uv[2];
    float color[4];
};

// Material matching Common.hlsli
struct Material {
    uint32_t textureIndex;
    uint32_t combinerMode;
    uint32_t isAlphaTested;
    uint32_t isWater;
    uint32_t wrapModeS;       // S (horizontal) wrap mode: 0=WRAP, 1=MIRROR, 2=CLAMP
    uint32_t wrapModeT;       // T (vertical) wrap mode: 0=WRAP, 1=MIRROR, 2=CLAMP
    uint32_t texWidthPx;      // Texture width in texels (e.g., 32)
    uint32_t texHeightPx;     // Texture height in texels (e.g., 32)
};

// Helper: build a simple identity matrix
static void SetIdentity(float m[16]) {
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

// Build inverse view matrix (camera at origin looking at +Z)
static void BuildViewInverse(float m[16], float eyeX, float eyeY, float eyeZ) {
    // View inverse = camera-to-world transform
    // For a camera at (eyeX, eyeY, eyeZ) looking at +Z with Y up:
    // Right = (1,0,0), Up = (0,1,0), Forward = (0,0,1)
    // Column-major layout:
    SetIdentity(m);
    m[12] = eyeX; // translation X
    m[13] = eyeY; // translation Y
    m[14] = eyeZ; // translation Z
}

// Build inverse projection matrix for perspective
static void BuildProjInverse(float m[16], float fovY, float aspect, float nearZ, float farZ) {
    // Standard perspective projection (column-major):
    // P[0][0] = 1/(aspect*tan(fovY/2))
    // P[1][1] = 1/tan(fovY/2)
    // P[2][2] = farZ/(farZ-nearZ)
    // P[2][3] = 1
    // P[3][2] = -nearZ*farZ/(farZ-nearZ)
    //
    // Inverse:
    float tanHalf = tanf(fovY * 0.5f);
    float a = aspect * tanHalf;
    float b = tanHalf;
    float c = farZ / (farZ - nearZ);
    float d = -nearZ * farZ / (farZ - nearZ);

    memset(m, 0, 16 * sizeof(float));
    m[0] = a;           // inv P[0][0]
    m[5] = b;           // inv P[1][1]
    m[11] = 1.0f;       // inv P[2][3] -> maps to w
    m[14] = 1.0f / d;   // inv P[3][2]
    m[15] = -c / d;     // inv P[3][3]
}

static void DumpDebugMessages(ComPtr<ID3D12InfoQueue>& infoQueue) {
    if (!infoQueue) return;
    uint64_t numMsgs = infoQueue->GetNumStoredMessages();
    if (numMsgs == 0) return;
    printf("\n--- Debug messages (%llu) ---\n", numMsgs);
    for (uint64_t i = 0; i < numMsgs && i < 30; i++) {
        SIZE_T msgLen = 0;
        infoQueue->GetMessage((UINT64)i, nullptr, &msgLen);
        std::vector<uint8_t> buf(msgLen);
        auto* msg = (D3D12_MESSAGE*)buf.data();
        infoQueue->GetMessage((UINT64)i, msg, &msgLen);
        const char* sev = "???";
        switch (msg->Severity) {
            case D3D12_MESSAGE_SEVERITY_CORRUPTION: sev = "CORRUPT"; break;
            case D3D12_MESSAGE_SEVERITY_ERROR: sev = "ERROR"; break;
            case D3D12_MESSAGE_SEVERITY_WARNING: sev = "WARN"; break;
            case D3D12_MESSAGE_SEVERITY_INFO: sev = "INFO"; break;
            case D3D12_MESSAGE_SEVERITY_MESSAGE: sev = "MSG"; break;
        }
        printf("  [%s] %s\n", sev, msg->pDescription);
    }
    infoQueue->ClearStoredMessages();
}

int main() {
    printf("=== RTX DXR Test Harness ===\n\n");

    // 1. Enable debug layer
    {
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
            debug->EnableDebugLayer();
            printf("[OK]   Debug layer enabled\n");
        } else {
            printf("[WARN] Debug layer not available\n");
        }
    }

    // 2. Create DXGI factory and find RTX adapter
    ComPtr<IDXGIFactory6> factory;
    CHK(CreateDXGIFactory2(DXGI_CREATE_FACTORY_DEBUG, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2");

    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                          IID_PPV_ARGS(&adapter)) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) { adapter.Reset(); continue; }
        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_1, __uuidof(ID3D12Device), nullptr))) {
            printf("[OK]   Found adapter: %ls\n", desc.Description);
            break;
        }
        adapter.Reset();
    }
    if (!adapter) { printf("[FAIL] No DX12 adapter found\n"); return 1; }

    // 3. Create device
    ComPtr<ID3D12Device5> device;
    CHK(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&device)), "CreateDevice");

    // Set up debug info queue - do NOT use SetBreakOnSeverity (hangs without debugger)
    ComPtr<ID3D12InfoQueue> infoQueue;
    if (SUCCEEDED(device.As(&infoQueue))) {
        // Don't break on errors - just collect messages for later
        infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, FALSE);
        infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, FALSE);
        printf("[OK]   Debug info queue configured (no break-on-error)\n");
    }

    // 4. Check raytracing support
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 opts5 = {};
    CHK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &opts5, sizeof(opts5)), "CheckFeatureSupport");
    printf("       Raytracing tier: %d\n", opts5.RaytracingTier);
    if (opts5.RaytracingTier < D3D12_RAYTRACING_TIER_1_0) {
        printf("[FAIL] Raytracing not supported\n"); return 1;
    }

    // 5. Create command queue, allocator, list
    ComPtr<ID3D12CommandQueue> cmdQueue;
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    CHK(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&cmdQueue)), "CreateCommandQueue");

    ComPtr<ID3D12CommandAllocator> cmdAlloc;
    CHK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&cmdAlloc)), "CreateCommandAllocator");

    ComPtr<ID3D12GraphicsCommandList4> cmdList;
    CHK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, cmdAlloc.Get(), nullptr, IID_PPV_ARGS(&cmdList)), "CreateCommandList");

    ComPtr<ID3D12Fence> fence;
    CHK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "CreateFence");
    HANDLE fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    uint64_t fenceValue = 1;

    auto WaitForGPU = [&]() {
        uint64_t val = fenceValue++;
        cmdQueue->Signal(fence.Get(), val);
        if (fence->GetCompletedValue() < val) {
            fence->SetEventOnCompletion(val, fenceEvent);
            WaitForSingleObject(fenceEvent, 10000); // 10 second timeout instead of INFINITE
        }
    };

    // 6. Load precompiled shaders
    printf("\n--- Loading shaders ---\n");
    auto exePath = std::filesystem::current_path();
    auto shaderDir = exePath / "shaders";

    auto rayGenData = ReadFile((shaderDir / "RayGen.cso").wstring());
    auto closestHitData = ReadFile((shaderDir / "ClosestHit.cso").wstring());
    auto missData = ReadFile((shaderDir / "Miss.cso").wstring());
    auto anyHitData = ReadFile((shaderDir / "AnyHit.cso").wstring());

    printf("       RayGen: %zu bytes\n", rayGenData.size());
    printf("       ClosestHit: %zu bytes\n", closestHitData.size());
    printf("       Miss: %zu bytes\n", missData.size());
    printf("       AnyHit: %zu bytes\n", anyHitData.size());

    if (rayGenData.empty() || closestHitData.empty() || missData.empty() || anyHitData.empty()) {
        printf("[FAIL] Shader files not found. Run from x64/Release/ directory.\n");
        printf("       Current dir: %s\n", exePath.string().c_str());
        printf("       Shader dir: %s\n", shaderDir.string().c_str());
        return 1;
    }

    // 7. Create global root signature
    // Must match DXRPipeline::CreateGlobalRootSignature():
    // [0] CBV b0  - SceneConstants
    // [1] SRV t0  - Acceleration structure (TLAS)
    // [2] Descriptor Table - UAV u0 (output) + u1 (accumulation)
    // [3] Descriptor Table - SRV range (textures, t4+)
    // Static sampler s0 - bilinear wrap
    printf("\n--- Creating root signatures ---\n");

    D3D12_ROOT_PARAMETER globalParams[4] = {};

    // [0] CBV b0 - Scene constants
    globalParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    globalParams[0].Descriptor.ShaderRegister = 0;
    globalParams[0].Descriptor.RegisterSpace = 0;
    globalParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // [1] SRV t0 - TLAS
    globalParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    globalParams[1].Descriptor.ShaderRegister = 0;
    globalParams[1].Descriptor.RegisterSpace = 0;
    globalParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // [2] Descriptor table for output UAVs (u0 + u1)
    D3D12_DESCRIPTOR_RANGE uavRange = {};
    uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors = 2; // u0 (output) + u1 (accumulation)
    uavRange.BaseShaderRegister = 0;
    uavRange.RegisterSpace = 0;
    uavRange.OffsetInDescriptorsFromTableStart = 0;

    globalParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    globalParams[2].DescriptorTable.NumDescriptorRanges = 1;
    globalParams[2].DescriptorTable.pDescriptorRanges = &uavRange;
    globalParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // [3] Descriptor table for texture array (SRV t4+)
    D3D12_DESCRIPTOR_RANGE texRange = {};
    texRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    texRange.NumDescriptors = 4096;
    texRange.BaseShaderRegister = 4;
    texRange.RegisterSpace = 0;
    texRange.OffsetInDescriptorsFromTableStart = 0;

    globalParams[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    globalParams[3].DescriptorTable.NumDescriptorRanges = 1;
    globalParams[3].DescriptorTable.pDescriptorRanges = &texRange;
    globalParams[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC staticSampler = {};
    staticSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    staticSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    staticSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    staticSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    staticSampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    staticSampler.MaxLOD = D3D12_FLOAT32_MAX;
    staticSampler.ShaderRegister = 0;
    staticSampler.RegisterSpace = 0;
    staticSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC globalSigDesc = {};
    globalSigDesc.NumParameters = 4;
    globalSigDesc.pParameters = globalParams;
    globalSigDesc.NumStaticSamplers = 1;
    globalSigDesc.pStaticSamplers = &staticSampler;

    ComPtr<ID3DBlob> sigBlob, sigError;
    ComPtr<ID3D12RootSignature> globalRootSig;
    HRESULT hr = D3D12SerializeRootSignature(&globalSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &sigError);
    if (FAILED(hr)) {
        if (sigError) printf("[FAIL] Root sig error: %s\n", (const char*)sigError->GetBufferPointer());
        return 1;
    }
    printf("[OK]   Serialized global root signature\n");
    CHK(device->CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(),
                                     IID_PPV_ARGS(&globalRootSig)),
        "Create global root signature");

    // Local root signature: 4 SRVs in space1
    D3D12_ROOT_PARAMETER localParams[4] = {};
    for (int i = 0; i < 4; i++) {
        localParams[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        localParams[i].Descriptor.ShaderRegister = i;
        localParams[i].Descriptor.RegisterSpace = 1;
        localParams[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }

    D3D12_ROOT_SIGNATURE_DESC localSigDesc = {};
    localSigDesc.NumParameters = 4;
    localSigDesc.pParameters = localParams;
    localSigDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE;

    ComPtr<ID3D12RootSignature> localRootSig;
    CHK(D3D12SerializeRootSignature(&localSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &sigError),
        "Serialize local root signature");
    CHK(device->CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(),
                                     IID_PPV_ARGS(&localRootSig)),
        "Create local root signature");

    // 8. Create raytracing state object
    printf("\n--- Creating raytracing state object ---\n");

    static const wchar_t* kRayGenExport = L"RayGen";
    static const wchar_t* kClosestHitExport = L"ClosestHit";
    static const wchar_t* kMissExport = L"Miss";
    static const wchar_t* kAnyHitExport = L"AnyHit";
    static const wchar_t* kHitGroupName = L"HitGroup";

    D3D12_DXIL_LIBRARY_DESC rayGenLib = {};
    rayGenLib.DXILLibrary = { rayGenData.data(), rayGenData.size() };
    D3D12_EXPORT_DESC rayGenExp = { kRayGenExport, nullptr, D3D12_EXPORT_FLAG_NONE };
    rayGenLib.NumExports = 1;
    rayGenLib.pExports = &rayGenExp;

    D3D12_DXIL_LIBRARY_DESC closestHitLib = {};
    closestHitLib.DXILLibrary = { closestHitData.data(), closestHitData.size() };
    D3D12_EXPORT_DESC closestHitExp = { kClosestHitExport, nullptr, D3D12_EXPORT_FLAG_NONE };
    closestHitLib.NumExports = 1;
    closestHitLib.pExports = &closestHitExp;

    D3D12_DXIL_LIBRARY_DESC missLib = {};
    missLib.DXILLibrary = { missData.data(), missData.size() };
    D3D12_EXPORT_DESC missExp = { kMissExport, nullptr, D3D12_EXPORT_FLAG_NONE };
    missLib.NumExports = 1;
    missLib.pExports = &missExp;

    D3D12_DXIL_LIBRARY_DESC anyHitLib = {};
    anyHitLib.DXILLibrary = { anyHitData.data(), anyHitData.size() };
    D3D12_EXPORT_DESC anyHitExp = { kAnyHitExport, nullptr, D3D12_EXPORT_FLAG_NONE };
    anyHitLib.NumExports = 1;
    anyHitLib.pExports = &anyHitExp;

    D3D12_HIT_GROUP_DESC hitGroup = {};
    hitGroup.HitGroupExport = kHitGroupName;
    hitGroup.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
    hitGroup.ClosestHitShaderImport = kClosestHitExport;
    hitGroup.AnyHitShaderImport = kAnyHitExport;

    D3D12_RAYTRACING_SHADER_CONFIG shaderConfig = {};
    shaderConfig.MaxPayloadSizeInBytes = 32;
    shaderConfig.MaxAttributeSizeInBytes = 8;

    D3D12_RAYTRACING_PIPELINE_CONFIG pipelineConfig = {};
    pipelineConfig.MaxTraceRecursionDepth = 2;

    D3D12_GLOBAL_ROOT_SIGNATURE globalRootSigSubobj = {};
    globalRootSigSubobj.pGlobalRootSignature = globalRootSig.Get();

    D3D12_LOCAL_ROOT_SIGNATURE localRootSigSubobj = {};
    localRootSigSubobj.pLocalRootSignature = localRootSig.Get();

    const wchar_t* hitGroupExports[] = { kHitGroupName };
    D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION localAssoc = {};
    localAssoc.NumExports = 1;
    localAssoc.pExports = hitGroupExports;

    constexpr uint32_t NUM_SUBOBJECTS = 10;
    D3D12_STATE_SUBOBJECT subobjects[NUM_SUBOBJECTS] = {};

    subobjects[0] = { D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &rayGenLib };
    subobjects[1] = { D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &closestHitLib };
    subobjects[2] = { D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &missLib };
    subobjects[3] = { D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &anyHitLib };
    subobjects[4] = { D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hitGroup };
    subobjects[5] = { D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &shaderConfig };
    subobjects[6] = { D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pipelineConfig };
    subobjects[7] = { D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &globalRootSigSubobj };
    subobjects[8] = { D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE, &localRootSigSubobj };

    localAssoc.pSubobjectToAssociate = &subobjects[8];
    subobjects[9] = { D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION, &localAssoc };

    D3D12_STATE_OBJECT_DESC stateObjDesc = {};
    stateObjDesc.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
    stateObjDesc.NumSubobjects = NUM_SUBOBJECTS;
    stateObjDesc.pSubobjects = subobjects;

    ComPtr<ID3D12StateObject> stateObject;
    hr = device->CreateStateObject(&stateObjDesc, IID_PPV_ARGS(&stateObject));
    if (FAILED(hr)) {
        printf("[FAIL] CreateStateObject: 0x%08X\n", (uint32_t)hr);
        DumpDebugMessages(infoQueue);
        return 1;
    }
    printf("[OK]   State object created!\n");

    ComPtr<ID3D12StateObjectProperties> stateObjProps;
    stateObject.As(&stateObjProps);

    // 9. Create a simple triangle BLAS
    printf("\n--- Building acceleration structures ---\n");

    struct Vertex { float x, y, z; };
    // Triangle at z=-2 (camera looks down -Z in standard projection)
    Vertex triVerts[] = {
        {  0.0f,  0.5f, -2.0f },
        { -0.5f, -0.5f, -2.0f },
        {  0.5f, -0.5f, -2.0f },
    };
    uint32_t triIndices[] = { 0, 1, 2 };

    // Upload vertex/index buffers
    auto CreateUploadBuffer = [&](const void* data, size_t size) -> ComPtr<ID3D12Resource> {
        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = size;
        desc.Height = 1; desc.DepthOrArraySize = 1; desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        ComPtr<ID3D12Resource> buf;
        device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                                         D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&buf));
        if (buf) {
            void* mapped;
            buf->Map(0, nullptr, &mapped);
            memcpy(mapped, data, size);
            buf->Unmap(0, nullptr);
        }
        return buf;
    };

    auto CreateDefaultBuffer = [&](size_t size, D3D12_RESOURCE_STATES state, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) -> ComPtr<ID3D12Resource> {
        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = size;
        desc.Height = 1; desc.DepthOrArraySize = 1; desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags = flags;

        ComPtr<ID3D12Resource> buf;
        device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                                         state, nullptr, IID_PPV_ARGS(&buf));
        return buf;
    };

    auto vbuf = CreateUploadBuffer(triVerts, sizeof(triVerts));
    auto ibuf = CreateUploadBuffer(triIndices, sizeof(triIndices));

    // BLAS
    D3D12_RAYTRACING_GEOMETRY_DESC geomDesc = {};
    geomDesc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    geomDesc.Triangles.VertexBuffer.StartAddress = vbuf->GetGPUVirtualAddress();
    geomDesc.Triangles.VertexBuffer.StrideInBytes = sizeof(Vertex);
    geomDesc.Triangles.VertexCount = 3;
    geomDesc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
    geomDesc.Triangles.IndexBuffer = ibuf->GetGPUVirtualAddress();
    geomDesc.Triangles.IndexCount = 3;
    geomDesc.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
    geomDesc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS blasInputs = {};
    blasInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    blasInputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    blasInputs.NumDescs = 1;
    blasInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    blasInputs.pGeometryDescs = &geomDesc;

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO blasPrebuild = {};
    device->GetRaytracingAccelerationStructurePrebuildInfo(&blasInputs, &blasPrebuild);

    auto blasScratch = CreateDefaultBuffer(blasPrebuild.ScratchDataSizeInBytes,
                                            D3D12_RESOURCE_STATE_COMMON,
                                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto blasResult = CreateDefaultBuffer(blasPrebuild.ResultDataMaxSizeInBytes,
                                           D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
                                           D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC blasDesc = {};
    blasDesc.Inputs = blasInputs;
    blasDesc.ScratchAccelerationStructureData = blasScratch->GetGPUVirtualAddress();
    blasDesc.DestAccelerationStructureData = blasResult->GetGPUVirtualAddress();

    cmdList->BuildRaytracingAccelerationStructure(&blasDesc, 0, nullptr);

    D3D12_RESOURCE_BARRIER uavBarrier = {};
    uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarrier.UAV.pResource = blasResult.Get();
    cmdList->ResourceBarrier(1, &uavBarrier);

    // TLAS
    D3D12_RAYTRACING_INSTANCE_DESC instanceDesc = {};
    // Identity transform
    instanceDesc.Transform[0][0] = 1.0f;
    instanceDesc.Transform[1][1] = 1.0f;
    instanceDesc.Transform[2][2] = 1.0f;
    instanceDesc.InstanceMask = 0xFF;
    instanceDesc.AccelerationStructure = blasResult->GetGPUVirtualAddress();

    auto instanceBuf = CreateUploadBuffer(&instanceDesc, sizeof(instanceDesc));

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tlasInputs = {};
    tlasInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    tlasInputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    tlasInputs.NumDescs = 1;
    tlasInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    tlasInputs.InstanceDescs = instanceBuf->GetGPUVirtualAddress();

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO tlasPrebuild = {};
    device->GetRaytracingAccelerationStructurePrebuildInfo(&tlasInputs, &tlasPrebuild);

    auto tlasScratch = CreateDefaultBuffer(tlasPrebuild.ScratchDataSizeInBytes,
                                            D3D12_RESOURCE_STATE_COMMON,
                                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto tlasResult = CreateDefaultBuffer(tlasPrebuild.ResultDataMaxSizeInBytes,
                                           D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
                                           D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC tlasDesc = {};
    tlasDesc.Inputs = tlasInputs;
    tlasDesc.ScratchAccelerationStructureData = tlasScratch->GetGPUVirtualAddress();
    tlasDesc.DestAccelerationStructureData = tlasResult->GetGPUVirtualAddress();

    cmdList->BuildRaytracingAccelerationStructure(&tlasDesc, 0, nullptr);
    uavBarrier.UAV.pResource = tlasResult.Get();
    cmdList->ResourceBarrier(1, &uavBarrier);

    // 10. Create output UAV textures (RWTexture2D<float4>)
    ComPtr<ID3D12Resource> outputTex, accumTex;
    {
        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC texDesc = {};
        texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texDesc.Width = WIDTH;
        texDesc.Height = HEIGHT;
        texDesc.DepthOrArraySize = 1;
        texDesc.MipLevels = 1;
        texDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        texDesc.SampleDesc.Count = 1;
        texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        CHK(device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &texDesc,
                                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                             IID_PPV_ARGS(&outputTex)),
            "Create output texture");
        CHK(device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &texDesc,
                                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                             IID_PPV_ARGS(&accumTex)),
            "Create accumulation texture");
    }

    // Create a 1x1 white dummy texture for the bindless texture array
    ComPtr<ID3D12Resource> dummyTex;
    {
        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC texDesc = {};
        texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texDesc.Width = 1;
        texDesc.Height = 1;
        texDesc.DepthOrArraySize = 1;
        texDesc.MipLevels = 1;
        texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        texDesc.SampleDesc.Count = 1;

        CHK(device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &texDesc,
                                             D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                             IID_PPV_ARGS(&dummyTex)),
            "Create dummy texture");
    }

    // Upload white pixel to dummy texture
    // Keep uploadBuf alive until GPU work completes (it's referenced in the command list)
    ComPtr<ID3D12Resource> dummyUploadBuf;
    {
        uint32_t whitePixel = 0xFFFFFFFF;
        dummyUploadBuf = CreateUploadBuffer(&whitePixel, sizeof(whitePixel));
        auto& uploadBuf = dummyUploadBuf;

        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource = uploadBuf.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Offset = 0;
        src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        src.PlacedFootprint.Footprint.Width = 1;
        src.PlacedFootprint.Footprint.Height = 1;
        src.PlacedFootprint.Footprint.Depth = 1;
        src.PlacedFootprint.Footprint.RowPitch = 256; // Minimum row pitch

        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource = dummyTex.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = 0;

        cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = dummyTex.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmdList->ResourceBarrier(1, &barrier);
    }

    // Create SRV/CBV/UAV descriptor heap
    // Layout:
    //   [0..1]  = UAV descriptors for output (u0) and accumulation (u1)
    //   [2]     = SRV for dummy texture (used as the bindless texture table start)
    //   [3..4098] = more SRVs for bindless textures (all pointing to dummy)
    ComPtr<ID3D12DescriptorHeap> srvHeap;
    {
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.NumDescriptors = 4099; // 2 UAVs + 4097 SRVs
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        CHK(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&srvHeap)), "Create SRV/UAV heap");
    }

    uint32_t descriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE heapCPU = srvHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE heapGPU = srvHeap->GetGPUDescriptorHandleForHeapStart();

    // Create UAV for output texture at slot 0
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uavDesc.Texture2D.MipSlice = 0;

        D3D12_CPU_DESCRIPTOR_HANDLE cpu;
        cpu.ptr = heapCPU.ptr + 0 * descriptorSize;
        device->CreateUnorderedAccessView(outputTex.Get(), nullptr, &uavDesc, cpu);
    }

    // Create UAV for accumulation texture at slot 1
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uavDesc.Texture2D.MipSlice = 0;

        D3D12_CPU_DESCRIPTOR_HANDLE cpu;
        cpu.ptr = heapCPU.ptr + 1 * descriptorSize;
        device->CreateUnorderedAccessView(accumTex.Get(), nullptr, &uavDesc, cpu);
    }

    // Create SRV for dummy texture at slots 2..4098 (fill all bindless slots with dummy)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = 1;

        for (uint32_t i = 0; i < 4096; i++) {
            D3D12_CPU_DESCRIPTOR_HANDLE cpu;
            cpu.ptr = heapCPU.ptr + (2 + i) * descriptorSize;
            device->CreateShaderResourceView(dummyTex.Get(), &srvDesc, cpu);
        }
    }
    printf("[OK]   Descriptor heap populated (2 UAVs + 4096 SRVs)\n");

    // GPU handle for the UAV table start (slot 0)
    D3D12_GPU_DESCRIPTOR_HANDLE uavTableGPU;
    uavTableGPU.ptr = heapGPU.ptr + 0 * descriptorSize;

    // GPU handle for the texture SRV table start (slot 2)
    D3D12_GPU_DESCRIPTOR_HANDLE texTableGPU;
    texTableGPU.ptr = heapGPU.ptr + 2 * descriptorSize;

    // 11. Create constant buffer
    SceneConstants constants = {};
    // Camera at origin, looking at +Z with explicit basis vectors
    // Right = (1,0,0), Up = (0,1,0), Forward = (0,0,1)
    constants.camRight[0] = 1.0f; constants.camRight[1] = 0.0f; constants.camRight[2] = 0.0f;
    constants.tanHalfFovX = tanf(0.5f * 1.0f); // ~60° horizontal FOV
    constants.camUp[0] = 0.0f; constants.camUp[1] = 1.0f; constants.camUp[2] = 0.0f;
    constants.tanHalfFovY = constants.tanHalfFovX / ((float)WIDTH / (float)HEIGHT);
    constants.camForward[0] = 0.0f; constants.camForward[1] = 0.0f; constants.camForward[2] = 1.0f;
    constants.frameCount = 0;
    constants.cameraPos[0] = 0; constants.cameraPos[1] = 0; constants.cameraPos[2] = 0;
    constants._pad0 = 0.0f;
    constants.ambientColor[0] = 0.15f; constants.ambientColor[1] = 0.18f; constants.ambientColor[2] = 0.25f;
    constants.fogNear = 500.0f;
    constants.fogColor[0] = 0.6f; constants.fogColor[1] = 0.7f; constants.fogColor[2] = 0.9f;
    constants.fogFar = 10000.0f;
    constants.sunDirection1[0] = 0.485071f; constants.sunDirection1[1] = 0.824621f; constants.sunDirection1[2] = 0.291043f;
    constants.time = 0.0f;
    constants.sunColor1[0] = 2.0f; constants.sunColor1[1] = 2.0f; constants.sunColor1[2] = 2.0f;
    constants.dekuTreeAlpha = 1.0f;
    constants.sunDirection2[0] = 0.0f; constants.sunDirection2[1] = 0.0f; constants.sunDirection2[2] = 0.0f;
    constants.fogBlendAlpha = 0.0f;
    constants.sunColor2[0] = 0.0f; constants.sunColor2[1] = 0.0f; constants.sunColor2[2] = 0.0f;
    constants.waterScrollOffset = 0.0f;
    constants.giIntensity = 1.0f;
    constants.baseReflectivity = 0.04f;
    constants.roughnessScale = 1.0f;
    constants.emissiveScale = 1.0f;
    constants.waterReflectivity = 0.6f;
    constants.waterRoughness = 0.1f;
    constants.aoRadius = 50.0f;
    constants.aoIntensity = 0.5f;
    constants.skyZenithColor[0] = 0.35f; constants.skyZenithColor[1] = 0.55f; constants.skyZenithColor[2] = 0.95f;
    constants.ambientMinIntensity = 0.10f;
    constants.skyHorizonColor[0] = 0.65f; constants.skyHorizonColor[1] = 0.75f; constants.skyHorizonColor[2] = 0.90f;
    constants.exposure = 1.0f;

    auto cbuf = CreateUploadBuffer(&constants, sizeof(constants));
    printf("[OK]   Constants buffer created (%zu bytes)\n", sizeof(constants));

    // 12. Create shader tables
    printf("\n--- Creating shader tables ---\n");
    uint32_t shaderIdSize = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES; // 32
    uint32_t rayGenRecordSize = (uint32_t)Align(shaderIdSize, D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT);
    uint32_t missRecordSize = (uint32_t)Align(shaderIdSize, D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT);
    // Hit group record: shader ID + 4 GPU virtual addresses (local root sig params)
    uint32_t hitRecordSize = (uint32_t)Align(shaderIdSize + 4 * sizeof(D3D12_GPU_VIRTUAL_ADDRESS),
                                              D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT);

    void* rayGenId = stateObjProps->GetShaderIdentifier(kRayGenExport);
    void* missId = stateObjProps->GetShaderIdentifier(kMissExport);
    void* hitGroupId = stateObjProps->GetShaderIdentifier(kHitGroupName);

    if (!rayGenId || !missId || !hitGroupId) {
        printf("[FAIL] Failed to get shader identifiers\n");
        return 1;
    }

    // For the hit group local root sig, we need vertex/index/materialID/material buffers
    // Create test data matching HLSL RTXVertex and Material structs
    RTXVertex testVerts[3] = {
        {{ 0.0f,  0.5f, -2.0f}, {0,0,1}, {0.5f, 0}, {1,0,0,1}},
        {{-0.5f, -0.5f, -2.0f}, {0,0,1}, {1, 1},    {0,1,0,1}},
        {{ 0.5f, -0.5f, -2.0f}, {0,0,1}, {0, 1},    {0,0,1,1}},
    };
    uint32_t testIndices[3] = {0, 1, 2};
    uint32_t testMatIds[1] = {0}; // per-triangle material ID

    Material testMat = {};
    testMat.textureIndex = 0; // Points to dummy white texture
    testMat.combinerMode = 0; // COMBINER_MODULATE_RGB
    testMat.isAlphaTested = 0;
    testMat.isWater = 0;

    auto vertBuf = CreateUploadBuffer(testVerts, sizeof(testVerts));
    auto idxBuf = CreateUploadBuffer(testIndices, sizeof(testIndices));
    auto matIdBuf = CreateUploadBuffer(testMatIds, sizeof(testMatIds));
    auto matBuf = CreateUploadBuffer(&testMat, sizeof(testMat));

    // Build shader table records
    std::vector<uint8_t> rayGenRecord(rayGenRecordSize, 0);
    memcpy(rayGenRecord.data(), rayGenId, shaderIdSize);

    std::vector<uint8_t> missRecord(missRecordSize, 0);
    memcpy(missRecord.data(), missId, shaderIdSize);

    std::vector<uint8_t> hitRecord(hitRecordSize, 0);
    memcpy(hitRecord.data(), hitGroupId, shaderIdSize);
    // Local root sig args after shader ID
    D3D12_GPU_VIRTUAL_ADDRESS* hitArgs = (D3D12_GPU_VIRTUAL_ADDRESS*)(hitRecord.data() + shaderIdSize);
    hitArgs[0] = vertBuf->GetGPUVirtualAddress();
    hitArgs[1] = idxBuf->GetGPUVirtualAddress();
    hitArgs[2] = matIdBuf->GetGPUVirtualAddress();
    hitArgs[3] = matBuf->GetGPUVirtualAddress();

    auto rayGenTable = CreateUploadBuffer(rayGenRecord.data(), rayGenRecord.size());
    auto missTable = CreateUploadBuffer(missRecord.data(), missRecord.size());
    auto hitTable = CreateUploadBuffer(hitRecord.data(), hitRecord.size());

    printf("[OK]   Shader tables created (RayGen=%u, Miss=%u, Hit=%u bytes)\n",
           rayGenRecordSize, missRecordSize, hitRecordSize);

    // 13. Dispatch rays!
    printf("\n--- Dispatching rays ---\n");

    ID3D12DescriptorHeap* heaps[] = { srvHeap.Get() };
    cmdList->SetDescriptorHeaps(1, heaps);

    cmdList->SetComputeRootSignature(globalRootSig.Get());
    cmdList->SetComputeRootConstantBufferView(0, cbuf->GetGPUVirtualAddress());
    cmdList->SetComputeRootShaderResourceView(1, tlasResult->GetGPUVirtualAddress());
    cmdList->SetComputeRootDescriptorTable(2, uavTableGPU);   // UAV table (u0=output, u1=accum)
    cmdList->SetComputeRootDescriptorTable(3, texTableGPU);    // SRV table (t4+ textures)

    cmdList->SetPipelineState1(stateObject.Get());

    D3D12_DISPATCH_RAYS_DESC dispatchDesc = {};
    dispatchDesc.RayGenerationShaderRecord = { rayGenTable->GetGPUVirtualAddress(), rayGenRecordSize };
    dispatchDesc.MissShaderTable = { missTable->GetGPUVirtualAddress(), missRecordSize, missRecordSize };
    dispatchDesc.HitGroupTable = { hitTable->GetGPUVirtualAddress(), hitRecordSize, hitRecordSize };
    dispatchDesc.Width = WIDTH;
    dispatchDesc.Height = HEIGHT;
    dispatchDesc.Depth = 1;

    cmdList->DispatchRays(&dispatchDesc);
    printf("[OK]   DispatchRays called (%ux%u)\n", WIDTH, HEIGHT);

    // 14. Copy output to readback buffer
    D3D12_RESOURCE_BARRIER copyBarrier = {};
    copyBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    copyBarrier.Transition.pResource = outputTex.Get();
    copyBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    copyBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    copyBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmdList->ResourceBarrier(1, &copyBarrier);

    // Readback buffer
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout = {};
    UINT64 totalBytes = 0;
    device->GetCopyableFootprints(&outputTex->GetDesc(), 0, 1, 0, &layout, nullptr, nullptr, &totalBytes);

    ComPtr<ID3D12Resource> readback;
    {
        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = totalBytes;
        desc.Height = 1; desc.DepthOrArraySize = 1; desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        CHK(device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                                             D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)),
            "Create readback buffer");
    }

    D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
    srcLoc.pResource = outputTex.Get();
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    srcLoc.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
    dstLoc.pResource = readback.Get();
    dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dstLoc.PlacedFootprint = layout;

    cmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

    // Execute
    cmdList->Close();
    ID3D12CommandList* lists[] = { cmdList.Get() };
    cmdQueue->ExecuteCommandLists(1, lists);
    WaitForGPU();

    // Check debug messages after execution
    DumpDebugMessages(infoQueue);

    // 15. Read back and write BMP
    printf("\n--- Reading back output ---\n");
    float* mapped = nullptr;
    D3D12_RANGE readRange = { 0, (SIZE_T)totalBytes };
    readback->Map(0, &readRange, (void**)&mapped);

    // Count non-zero pixels
    uint32_t nonBlackPixels = 0;
    std::vector<uint8_t> rgba(WIDTH * HEIGHT * 4);
    uint32_t rowPitch = layout.Footprint.RowPitch / sizeof(float); // floats per row

    for (uint32_t y = 0; y < HEIGHT; y++) {
        for (uint32_t x = 0; x < WIDTH; x++) {
            float* px = mapped + y * rowPitch + x * 4;
            float r = px[0], g = px[1], b = px[2], a = px[3];
            // Tonemap (clamp)
            auto toU8 = [](float v) -> uint8_t {
                return (uint8_t)(v < 0 ? 0 : (v > 1 ? 255 : (uint8_t)(v * 255.0f)));
            };
            uint32_t idx = (y * WIDTH + x) * 4;
            rgba[idx + 0] = toU8(r);
            rgba[idx + 1] = toU8(g);
            rgba[idx + 2] = toU8(b);
            rgba[idx + 3] = toU8(a);
            if (r > 0.001f || g > 0.001f || b > 0.001f) nonBlackPixels++;
        }
    }
    readback->Unmap(0, nullptr);

    printf("       Non-black pixels: %u / %u (%.1f%%)\n",
           nonBlackPixels, WIDTH * HEIGHT, 100.0f * nonBlackPixels / (WIDTH * HEIGHT));

    WriteBMP("rtx_test_output.bmp", WIDTH, HEIGHT, rgba.data());

    // Also write a text output file for verification
    {
        FILE* f = fopen("rtx_output.txt", "w");
        if (f) {
            fprintf(f, "RTX Test Output\n");
            fprintf(f, "Dimensions: %ux%u\n", WIDTH, HEIGHT);
            fprintf(f, "Non-black pixels: %u / %u (%.1f%%)\n",
                    nonBlackPixels, WIDTH * HEIGHT, 100.0f * nonBlackPixels / (WIDTH * HEIGHT));
            fprintf(f, "Status: %s\n", nonBlackPixels > 0 ? "SUCCESS" : "ALL_BLACK");
            fclose(f);
        }
    }

    if (nonBlackPixels > 0) {
        printf("\n=== SUCCESS: RTX pipeline produced non-black output! ===\n");
    } else {
        printf("\n=== RESULT: All black output. Pipeline may have issues. ===\n");
    }

    CloseHandle(fenceEvent);
    return 0;
}
