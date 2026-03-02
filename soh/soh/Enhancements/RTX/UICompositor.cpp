#ifdef ENABLE_DX12_RTX

#include "UICompositor.h"
#include "TextureManager.h"
#include "RTXShaderCompiler.h"
#include "RTXDiagLog.h"
#include <spdlog/spdlog.h>
#include <cstring>
#include <filesystem>
#ifdef _WIN32
#include <Windows.h>
#endif

namespace RTX {

UICompositor::UICompositor() = default;

UICompositor::~UICompositor() {
    Shutdown();
}

bool UICompositor::Initialize(DX12Device* device, uint32_t width, uint32_t height) {
    if (!device || width == 0 || height == 0) {
        RTX_DIAG("UICompositor::Initialize() FAILED - invalid params (device=%p, %ux%u)",
                 (void*)device, width, height);
        return false;
    }

    m_device = device;
    m_width = width;
    m_height = height;

    RTX_DIAG("UICompositor::Initialize() %ux%u", width, height);

    if (!CreateUITexture(width, height)) {
        RTX_DIAG("UICompositor::Initialize() FAILED - CreateUITexture");
        return false;
    }

    if (!CreateUploadBuffer(width, height)) {
        RTX_DIAG("UICompositor::Initialize() FAILED - CreateUploadBuffer");
        return false;
    }

    if (!CreateCompositeComputePipeline()) {
        RTX_DIAG("UICompositor::Initialize() FAILED - CreateCompositeComputePipeline");
        // Non-fatal: compositing just won't work until the shader is available
        SPDLOG_WARN("[RTX] UICompositor: composite pipeline creation deferred");
    }

    if (!CreateDescriptors()) {
        RTX_DIAG("UICompositor::Initialize() FAILED - CreateDescriptors");
        return false;
    }

    m_initialized = true;
    SPDLOG_INFO("[RTX] UICompositor initialized ({}x{})", width, height);
    RTX_DIAG("UICompositor::Initialize() SUCCESS");
    return true;
}

void UICompositor::Shutdown() {
    RTX_DIAG("UICompositor::Shutdown()");

    if (m_uploadBufferMapped && m_uploadBuffer) {
        m_uploadBuffer->Unmap(0, nullptr);
        m_uploadBufferMapped = nullptr;
    }

    m_uiTexture.Reset();
    m_uploadBuffer.Reset();
    m_compositePipelineState.Reset();
    m_compositeRootSignature.Reset();

    m_device = nullptr;
    m_initialized = false;
    m_hasUIData = false;
    m_firstUpload = true;
    m_width = 0;
    m_height = 0;
}

bool UICompositor::CreateUITexture(uint32_t width, uint32_t height) {
    auto* d3dDevice = m_device->GetDevice();
    if (!d3dDevice) return false;

    // Create a RGBA8 texture in DEFAULT heap for the UI overlay
    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Alignment = 0;
    texDesc.Width = width;
    texDesc.Height = height;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.SampleDesc.Quality = 0;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texDesc.Flags = D3D12_RESOURCE_FLAG_NONE; // SRV only (no UAV needed for this texture)

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    // Start in COPY_DEST state since first use is upload
    HRESULT hr = d3dDevice->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &texDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        IID_PPV_ARGS(&m_uiTexture));

    if (FAILED(hr)) {
        RTX_DIAG("UICompositor: CreateUITexture FAILED hr=0x%08X", (uint32_t)hr);
        return false;
    }

    RTX_DIAG("UICompositor: UI texture created %ux%u RGBA8", width, height);
    return true;
}

bool UICompositor::CreateUploadBuffer(uint32_t width, uint32_t height) {
    auto* d3dDevice = m_device->GetDevice();
    if (!d3dDevice) return false;

    // Calculate the required size for the upload buffer
    // Must be aligned to D3D12_TEXTURE_DATA_PITCH_ALIGNMENT (256 bytes)
    uint32_t rowPitch = (width * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) &
                        ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
    uint64_t uploadSize = static_cast<uint64_t>(rowPitch) * height;

    D3D12_RESOURCE_DESC bufDesc = {};
    bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Width = uploadSize;
    bufDesc.Height = 1;
    bufDesc.DepthOrArraySize = 1;
    bufDesc.MipLevels = 1;
    bufDesc.Format = DXGI_FORMAT_UNKNOWN;
    bufDesc.SampleDesc.Count = 1;
    bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    HRESULT hr = d3dDevice->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &bufDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&m_uploadBuffer));

    if (FAILED(hr)) {
        RTX_DIAG("UICompositor: CreateUploadBuffer FAILED hr=0x%08X", (uint32_t)hr);
        return false;
    }

    // Map the upload buffer persistently
    hr = m_uploadBuffer->Map(0, nullptr, &m_uploadBufferMapped);
    if (FAILED(hr)) {
        RTX_DIAG("UICompositor: Upload buffer Map FAILED hr=0x%08X", (uint32_t)hr);
        m_uploadBuffer.Reset();
        return false;
    }

    RTX_DIAG("UICompositor: Upload buffer created (%llu bytes, pitch=%u)", uploadSize, rowPitch);
    return true;
}

bool UICompositor::CreateCompositeComputePipeline() {
    auto* d3dDevice = m_device->GetDevice();
    if (!d3dDevice) return false;

    // Load the Composite shader
    RTXShaderCompiler compiler;
    if (!compiler.Initialize()) {
        RTX_DIAG("UICompositor: Shader compiler init failed");
        return false;
    }

    // Look for precompiled CSO first, then fall back to HLSL
    // Use the same path resolution strategy as DXRPipeline::LoadShaders()
    // Use the executable directory (not current_path which may differ)
    std::filesystem::path exeDir;
    try {
#ifdef _WIN32
        wchar_t exePath[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        exeDir = std::filesystem::path(exePath).parent_path();
#else
        exeDir = std::filesystem::current_path();
#endif
    } catch (const std::exception& e) {
        RTX_DIAG("UICompositor: Failed to determine exe dir: %s — using current_path", e.what());
        try { exeDir = std::filesystem::current_path(); } catch (...) {}
    }

    std::filesystem::path precompiledDir;
    try {
#ifdef RTX_COMPILED_SHADER_SUBDIR
        precompiledDir = exeDir / std::string(RTX_COMPILED_SHADER_SUBDIR);
#else
        precompiledDir = exeDir / "shaders";
#endif
    } catch (...) {
        precompiledDir = exeDir / "shaders";
    }

    // Source path: try compile-time define, then look relative to source tree
    std::filesystem::path sourceDir;
    try {
#ifdef RTX_SHADER_SOURCE_DIR
        sourceDir = std::filesystem::path(std::string(RTX_SHADER_SOURCE_DIR));
#endif
        if (sourceDir.empty() || !std::filesystem::exists(sourceDir)) {
            sourceDir = exeDir / "Shaders";
        }
        if (!std::filesystem::exists(sourceDir)) {
            sourceDir = std::filesystem::path(__FILE__).parent_path() / "Shaders";
        }
    } catch (const std::exception& e) {
        RTX_DIAG("UICompositor: Exception resolving shader source dir: %s", e.what());
        sourceDir = exeDir / "Shaders";
    }

    std::wstring precompiled = (precompiledDir / L"Composite.cso").wstring();
    std::wstring hlsl = (sourceDir / L"Composite.hlsl").wstring();

    RTX_DIAG("UICompositor: Loading Composite shader (precompiled=%ls, hlsl=%ls)",
             precompiled.c_str(), hlsl.c_str());

    auto compositeBlob = compiler.LoadOrCompile(precompiled, hlsl, L"Composite", L"cs_6_0", L"Composite");
    if (!compositeBlob) {
        RTX_DIAG("UICompositor: Composite shader load FAILED");
        SPDLOG_WARN("[RTX] UICompositor: Composite shader not available");
        return false;
    }

    RTX_DIAG("UICompositor: Composite shader loaded (%zu bytes)", compositeBlob->GetBufferSize());

    // Create root signature:
    // [0] Descriptor Table: UAV u0 (RTX scene output, read/write)
    // [1] Descriptor Table: SRV t0 (UI overlay texture, read-only)
    // [2] 32-bit Constants b0: CompositeConstants
    // Static sampler s0: point sampling for UI texture

    D3D12_DESCRIPTOR_RANGE uavRange = {};
    uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors = 1;
    uavRange.BaseShaderRegister = 0;
    uavRange.RegisterSpace = 0;
    uavRange.OffsetInDescriptorsFromTableStart = 0;

    D3D12_DESCRIPTOR_RANGE srvRange = {};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 1;
    srvRange.BaseShaderRegister = 0;
    srvRange.RegisterSpace = 0;
    srvRange.OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER params[3] = {};

    // [0] UAV table (u0 = RTX scene)
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 1;
    params[0].DescriptorTable.pDescriptorRanges = &uavRange;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // [1] SRV table (t0 = UI overlay)
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &srvRange;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // [2] 32-bit constants (CompositeConstants)
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[2].Constants.ShaderRegister = 0;
    params[2].Constants.Num32BitValues = sizeof(CompositeConstants) / 4;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC sigDesc = {};
    sigDesc.NumParameters = 3;
    sigDesc.pParameters = params;
    sigDesc.NumStaticSamplers = 0; // UI texture uses Load(), not Sample()
    sigDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> sigBlob, sigError;
    HRESULT hr = D3D12SerializeRootSignature(&sigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &sigError);
    if (FAILED(hr)) {
        if (sigError) {
            RTX_DIAG("UICompositor: Root sig serialize error: %s", (const char*)sigError->GetBufferPointer());
        }
        return false;
    }

    hr = d3dDevice->CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(),
                                         IID_PPV_ARGS(&m_compositeRootSignature));
    if (FAILED(hr)) {
        RTX_DIAG("UICompositor: Root sig creation FAILED hr=0x%08X", (uint32_t)hr);
        return false;
    }

    // Create compute PSO
    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = m_compositeRootSignature.Get();
    psoDesc.CS.pShaderBytecode = compositeBlob->GetBufferPointer();
    psoDesc.CS.BytecodeLength = compositeBlob->GetBufferSize();

    hr = d3dDevice->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&m_compositePipelineState));
    if (FAILED(hr)) {
        RTX_DIAG("UICompositor: Composite PSO creation FAILED hr=0x%08X", (uint32_t)hr);
        return false;
    }

    RTX_DIAG("UICompositor: Composite pipeline created successfully");
    SPDLOG_INFO("[RTX] UICompositor: Composite compute pipeline created");
    return true;
}

bool UICompositor::CreateDescriptors() {
    auto& texMgr = TextureManager::GetInstance();
    ID3D12DescriptorHeap* srvHeap = texMgr.GetSRVHeap();
    auto* d3dDevice = m_device->GetDevice();

    if (!srvHeap || !d3dDevice) {
        RTX_DIAG("UICompositor::CreateDescriptors() FAILED - heap=%p, device=%p",
                 (void*)srvHeap, (void*)d3dDevice);
        return false;
    }

    uint32_t descriptorSize = d3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE heapStartCPU = srvHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE heapStartGPU = srvHeap->GetGPUDescriptorHandleForHeapStart();

    // Create SRV for UI overlay texture at UI_SRV_SLOT
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = 1;
        srvDesc.Texture2D.MostDetailedMip = 0;

        D3D12_CPU_DESCRIPTOR_HANDLE cpu;
        cpu.ptr = heapStartCPU.ptr + UI_SRV_SLOT * descriptorSize;
        d3dDevice->CreateShaderResourceView(m_uiTexture.Get(), &srvDesc, cpu);

        m_uiTextureSRVCPU.ptr = cpu.ptr;
        m_uiTextureSRVGPU.ptr = heapStartGPU.ptr + UI_SRV_SLOT * descriptorSize;
    }

    RTX_DIAG("UICompositor: Descriptors created - UI SRV at slot %u", UI_SRV_SLOT);
    SPDLOG_INFO("[RTX] UICompositor: UI SRV descriptor at slot {}", UI_SRV_SLOT);
    return true;
}

void UICompositor::UploadUIFrame(ID3D12GraphicsCommandList4* cmdList,
                                  const uint8_t* pixelData, uint32_t width, uint32_t height) {
    if (!cmdList || !m_initialized || !m_uiTexture || !m_uploadBuffer || !m_uploadBufferMapped) {
        return;
    }

    if (width != m_width || height != m_height) {
        RTX_DIAG("UICompositor: UploadUIFrame dimension mismatch (%ux%u vs %ux%u)",
                 width, height, m_width, m_height);
        return;
    }

    // Calculate aligned row pitch
    uint32_t srcRowPitch = width * 4;
    uint32_t dstRowPitch = (srcRowPitch + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) &
                           ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);

    // Copy pixel data to the upload buffer (with row pitch alignment)
    uint8_t* dst = static_cast<uint8_t*>(m_uploadBufferMapped);
    if (pixelData) {
        for (uint32_t row = 0; row < height; row++) {
            memcpy(dst + row * dstRowPitch, pixelData + row * srcRowPitch, srcRowPitch);
        }
        m_hasUIData = true;
    } else {
        // Clear to transparent
        memset(dst, 0, static_cast<size_t>(dstRowPitch) * height);
        m_hasUIData = false;
    }

    // Transition UI texture to COPY_DEST
    // On the first upload, the texture is already in COPY_DEST state (from creation).
    // On subsequent uploads, it's in NON_PIXEL_SHADER_RESOURCE state (from the
    // previous composite dispatch).
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = m_uiTexture.Get();
    barrier.Transition.StateBefore = m_firstUpload ? D3D12_RESOURCE_STATE_COPY_DEST
                                                    : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    // Skip barrier if source == dest (first upload)
    if (barrier.Transition.StateBefore != barrier.Transition.StateAfter) {
        cmdList->ResourceBarrier(1, &barrier);
    }
    m_firstUpload = false;

    // Copy from upload buffer to UI texture
    D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
    srcLoc.pResource = m_uploadBuffer.Get();
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcLoc.PlacedFootprint.Offset = 0;
    srcLoc.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srcLoc.PlacedFootprint.Footprint.Width = width;
    srcLoc.PlacedFootprint.Footprint.Height = height;
    srcLoc.PlacedFootprint.Footprint.Depth = 1;
    srcLoc.PlacedFootprint.Footprint.RowPitch = dstRowPitch;

    D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
    dstLoc.pResource = m_uiTexture.Get();
    dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dstLoc.SubresourceIndex = 0;

    cmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

    // Transition UI texture to SRV for the composite shader
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    cmdList->ResourceBarrier(1, &barrier);
}

void UICompositor::ClearUIOverlay(ID3D12GraphicsCommandList4* cmdList) {
    UploadUIFrame(cmdList, nullptr, m_width, m_height);
}

void UICompositor::Composite(ID3D12GraphicsCommandList4* cmdList,
                              uint32_t width, uint32_t height,
                              D3D12_GPU_DESCRIPTOR_HANDLE rtxOutputUAV) {
    if (!cmdList || !m_initialized || !m_compositePipelineState || !m_compositeRootSignature) {
        return;
    }

    if (!m_hasUIData || !m_uiTexture) {
        return; // Nothing to composite or UI texture not available
    }

    if (width == 0 || height == 0 || rtxOutputUAV.ptr == 0 || m_uiTextureSRVGPU.ptr == 0) {
        return; // Invalid parameters — skip to avoid GPU errors
    }

    // Set up the composite compute pipeline
    cmdList->SetComputeRootSignature(m_compositeRootSignature.Get());
    cmdList->SetPipelineState(m_compositePipelineState.Get());

    // [0] = UAV table (u0 = RTX scene output)
    cmdList->SetComputeRootDescriptorTable(0, rtxOutputUAV);

    // [1] = SRV table (t0 = UI overlay)
    cmdList->SetComputeRootDescriptorTable(1, m_uiTextureSRVGPU);

    // [2] = CompositeConstants
    CompositeConstants cc = {};
    cc.resolutionX = width;
    cc.resolutionY = height;
    cc.uiOpacity = m_uiOpacity;
    cc.flags = 1; // UI enabled
    if (m_debugOutline) cc.flags |= 2;

    cmdList->SetComputeRoot32BitConstants(2, sizeof(CompositeConstants) / 4, &cc, 0);

    // Dispatch
    uint32_t groupsX = (width + 7) / 8;
    uint32_t groupsY = (height + 7) / 8;
    cmdList->Dispatch(groupsX, groupsY, 1);

    static uint32_t s_compositeCount = 0;
    s_compositeCount++;
    if (s_compositeCount <= 5 || (s_compositeCount % 300) == 0) {
        RTX_DIAG("UICompositor::Composite() dispatched %ux%u (%ux%u groups), frame #%u",
                 width, height, groupsX, groupsY, s_compositeCount);
    }
}

} // namespace RTX

#endif // ENABLE_DX12_RTX
