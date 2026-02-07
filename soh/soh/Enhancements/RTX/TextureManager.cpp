#ifdef ENABLE_DX12_RTX

#include "TextureManager.h"
#include <spdlog/spdlog.h>
#include <cassert>
#include <cstring>
#include <algorithm>

namespace RTX {

// ============================================================================
// Singleton
// ============================================================================

TextureManager* TextureManager::s_instance = nullptr;

TextureManager& TextureManager::GetInstance() {
    if (!s_instance) {
        s_instance = new TextureManager();
    }
    return *s_instance;
}

// ============================================================================
// Construction / Destruction
// ============================================================================

TextureManager::TextureManager()
    : mDefaultWhiteTexture{}
    , mDefaultNormalMapTexture{}
    , mCheckerboardFallback{}
{
    mDefaultWhiteTexture.hash = 0;
    mDefaultWhiteTexture.width = 0;
    mDefaultWhiteTexture.height = 0;
    mDefaultWhiteTexture.srvIndex = 0;
    mDefaultWhiteTexture.format = DXGI_FORMAT_UNKNOWN;
    mDefaultWhiteTexture.srvGpuHandle.ptr = 0;
    mDefaultWhiteTexture.isBound = false;
    mDefaultWhiteTexture.uploaded = false;

    mDefaultNormalMapTexture.hash = 0xFFFFFFFFFFFFFFFFULL;
    mDefaultNormalMapTexture.width = 0;
    mDefaultNormalMapTexture.height = 0;
    mDefaultNormalMapTexture.srvIndex = 0;
    mDefaultNormalMapTexture.format = DXGI_FORMAT_UNKNOWN;
    mDefaultNormalMapTexture.srvGpuHandle.ptr = 0;
    mDefaultNormalMapTexture.isBound = false;
    mDefaultNormalMapTexture.uploaded = false;

    mCheckerboardFallback.hashKey = 0;
    mCheckerboardFallback.width = 0;
    mCheckerboardFallback.height = 0;
    mCheckerboardFallback.srvIndex = 1;
    mCheckerboardFallback.dxgiFormat = DXGI_FORMAT_UNKNOWN;
    mCheckerboardFallback.srvGpuHandle.ptr = 0;
    mCheckerboardFallback.srvCpuHandle.ptr = 0;
    mCheckerboardFallback.lastUsedFrame = 0;
    mCheckerboardFallback.uploaded = false;
}

TextureManager::~TextureManager() {
    Shutdown();
    if (s_instance == this) {
        s_instance = nullptr;
    }
}

// ============================================================================
// FNV-1a 64-bit Hash
// ============================================================================

uint64_t TextureManager::HashTextureData(const uint8_t* data, size_t size) {
    constexpr uint64_t FNV_OFFSET_BASIS = 0xcbf29ce484222325ULL;
    constexpr uint64_t FNV_PRIME        = 0x100000001b3ULL;

    uint64_t hash = FNV_OFFSET_BASIS;
    for (size_t i = 0; i < size; i++) {
        hash ^= static_cast<uint64_t>(data[i]);
        hash *= FNV_PRIME;
    }
    return hash;
}

// ============================================================================
// ComputeTextureHash — primary cache key algorithm
// Combines format, address, width, height, and palette CRC into a 64-bit hash.
// ============================================================================

uint64_t TextureManager::ComputeTextureHash(uint32_t format, uintptr_t address,
                                             uint32_t width, uint32_t height,
                                             uint32_t paletteCRC) {
    constexpr uint64_t FNV_OFFSET_BASIS = 0xcbf29ce484222325ULL;
    constexpr uint64_t FNV_PRIME        = 0x100000001b3ULL;

    uint64_t hash = FNV_OFFSET_BASIS;

    // Mix in format
    hash ^= static_cast<uint64_t>(format);
    hash *= FNV_PRIME;

    // Mix in address (all 8 bytes on 64-bit)
    for (int i = 0; i < (int)sizeof(uintptr_t); i++) {
        hash ^= (address >> (i * 8)) & 0xFF;
        hash *= FNV_PRIME;
    }

    // Mix in width
    hash ^= static_cast<uint64_t>(width);
    hash *= FNV_PRIME;

    // Mix in height
    hash ^= static_cast<uint64_t>(height);
    hash *= FNV_PRIME;

    // Mix in palette CRC
    hash ^= static_cast<uint64_t>(paletteCRC);
    hash *= FNV_PRIME;

    return hash;
}

// ============================================================================
// AdvanceFrame — called once per frame for LRU tracking
// ============================================================================

void TextureManager::AdvanceFrame() {
    mCurrentFrame++;
}

// ============================================================================
// SetMaxCachedTextures
// ============================================================================

void TextureManager::SetMaxCachedTextures(uint32_t maxCached) {
    std::lock_guard<std::mutex> lock(mCacheMutex);
    mMaxCachedTextures = maxCached;
}

// ============================================================================
// Initialize (DX12Device path - creates own SRV heap)
// ============================================================================

bool TextureManager::Initialize(DX12Device* context) {
    if (mInitialized) {
        SPDLOG_WARN("[RTX] TextureManager already initialized");
        return true;
    }

    if (!context || !context->IsInitialized()) {
        SPDLOG_ERROR("[RTX] TextureManager::Initialize - invalid DX12Device context");
        return false;
    }

    mContext = context;
    mDevice = context->GetDevice();
    mRawDevice = context->GetDevice();
    mUsingExternalHeap = false;
    mMaxTextures = MAX_TEXTURES;

    auto* device = mContext->GetDevice();

    // Create the shader-visible SRV descriptor heap for bindless textures
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.NumDescriptors = MAX_TEXTURES;
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    heapDesc.NodeMask = 0;

    HRESULT hr = device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&mSrvHeap));
    if (FAILED(hr)) {
        SPDLOG_ERROR("[RTX] TextureManager: Failed to create SRV descriptor heap ({} descriptors): 0x{:08X}",
                     MAX_TEXTURES, static_cast<uint32_t>(hr));
        return false;
    }

    mSrvDescriptorSize = device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    mDescriptorSize = mSrvDescriptorSize;
    mHeapOffset = 0;

    // Create dedicated command allocator and command list for uploads
    hr = device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(&mUploadCmdAllocator));
    if (FAILED(hr)) {
        SPDLOG_ERROR("[RTX] TextureManager: Failed to create upload command allocator: 0x{:08X}",
                     static_cast<uint32_t>(hr));
        mSrvHeap.Reset();
        return false;
    }

    hr = device->CreateCommandList(
        0,
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        mUploadCmdAllocator.Get(),
        nullptr,
        IID_PPV_ARGS(&mUploadCmdList));
    if (FAILED(hr)) {
        SPDLOG_ERROR("[RTX] TextureManager: Failed to create upload command list: 0x{:08X}",
                     static_cast<uint32_t>(hr));
        mUploadCmdAllocator.Reset();
        mSrvHeap.Reset();
        return false;
    }

    // Close immediately - we reset before each upload operation
    mUploadCmdList->Close();

    mNextDescriptorIndex = 0;
    mInitialized = true;

    // Create default textures at reserved SRV indices (white, checkerboard, flat normal map)
    CreateDefaultTextures();

    SPDLOG_INFO("[RTX] TextureManager initialized (SRV heap: {} descriptors, descriptor size: {})",
                MAX_TEXTURES, mSrvDescriptorSize);
    return true;
}

// ============================================================================
// Initialize (explicit device/heap path)
// ============================================================================

bool TextureManager::Initialize(ID3D12Device* device, ID3D12DescriptorHeap* srvHeap, UINT heapOffset) {
    if (mInitialized) {
        SPDLOG_WARN("[RTX] TextureManager already initialized");
        return true;
    }

    if (!device) {
        SPDLOG_ERROR("[RTX] TextureManager::Initialize - null device");
        return false;
    }

    if (!srvHeap) {
        SPDLOG_ERROR("[RTX] TextureManager::Initialize - null SRV heap");
        return false;
    }

    mContext = nullptr;
    mRawDevice = device;
    mExternalSrvHeap = srvHeap;
    mUsingExternalHeap = true;
    mHeapOffset = heapOffset;

    mSrvDescriptorSize = device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    mDescriptorSize = mSrvDescriptorSize;
    mMaxTextures = MAX_TEXTURES;

    // Create command queue for uploads
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queueDesc.NodeMask = 0;

    HRESULT hr = device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&mOwnedCommandQueue));
    if (FAILED(hr)) {
        SPDLOG_ERROR("[RTX] TextureManager: Failed to create command queue: 0x{:08X}",
                     static_cast<uint32_t>(hr));
        return false;
    }

    // Create fence for upload synchronization
    hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&mUploadFence));
    if (FAILED(hr)) {
        SPDLOG_ERROR("[RTX] TextureManager: Failed to create upload fence: 0x{:08X}",
                     static_cast<uint32_t>(hr));
        mOwnedCommandQueue.Reset();
        return false;
    }

    mUploadFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!mUploadFenceEvent) {
        SPDLOG_ERROR("[RTX] TextureManager: Failed to create fence event");
        mUploadFence.Reset();
        mOwnedCommandQueue.Reset();
        return false;
    }

    mUploadFenceValue = 0;

    // Create dedicated command allocator and command list
    hr = device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(&mUploadCmdAllocator));
    if (FAILED(hr)) {
        SPDLOG_ERROR("[RTX] TextureManager: Failed to create upload command allocator: 0x{:08X}",
                     static_cast<uint32_t>(hr));
        CloseHandle(mUploadFenceEvent);
        mUploadFenceEvent = nullptr;
        mUploadFence.Reset();
        mOwnedCommandQueue.Reset();
        return false;
    }

    ComPtr<ID3D12GraphicsCommandList> baseCmdList;
    hr = device->CreateCommandList(
        0,
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        mUploadCmdAllocator.Get(),
        nullptr,
        IID_PPV_ARGS(&baseCmdList));
    if (FAILED(hr)) {
        SPDLOG_ERROR("[RTX] TextureManager: Failed to create upload command list: 0x{:08X}",
                     static_cast<uint32_t>(hr));
        mUploadCmdAllocator.Reset();
        CloseHandle(mUploadFenceEvent);
        mUploadFenceEvent = nullptr;
        mUploadFence.Reset();
        mOwnedCommandQueue.Reset();
        return false;
    }

    hr = baseCmdList.As(&mUploadCmdList);
    if (FAILED(hr)) {
        SPDLOG_ERROR("[RTX] TextureManager: Failed to query ID3D12GraphicsCommandList4: 0x{:08X}",
                     static_cast<uint32_t>(hr));
        mUploadCmdAllocator.Reset();
        CloseHandle(mUploadFenceEvent);
        mUploadFenceEvent = nullptr;
        mUploadFence.Reset();
        mOwnedCommandQueue.Reset();
        return false;
    }

    // Close immediately
    mUploadCmdList->Close();

    mNextDescriptorIndex = 0;
    mInitialized = true;

    // Create default textures (white, checkerboard, flat normal map)
    CreateDefaultTextures();

    SPDLOG_INFO("[RTX] TextureManager initialized (external heap, offset: {}, descriptor size: {})",
                heapOffset, mSrvDescriptorSize);
    return true;
}

// ============================================================================
// Initialize (ID3D12Device5 + SRV heap + heapStartIndex path)
// Task-spec Phase 6 exact signature: Initialize(ID3D12Device5*, ID3D12DescriptorHeap*, UINT)
// Uses the caller-provided SRV heap starting at heapStartIndex.
// ============================================================================

bool TextureManager::Initialize(ID3D12Device5* device, ID3D12DescriptorHeap* srvHeap, UINT heapStartIndex) {
    if (mInitialized) {
        SPDLOG_WARN("[RTX] TextureManager already initialized");
        return true;
    }

    if (!device) {
        SPDLOG_ERROR("[RTX] TextureManager::Initialize(Device5,heap,idx) - null device");
        return false;
    }

    if (!srvHeap) {
        SPDLOG_ERROR("[RTX] TextureManager::Initialize(Device5,heap,idx) - null SRV heap");
        return false;
    }

    mContext = nullptr;
    mDevice = device;
    mRawDevice = device;
    mExternalSrvHeap = srvHeap;
    mUsingExternalHeap = true;
    mHeapOffset = heapStartIndex;
    mMaxTextures = MAX_TEXTURES;

    mSrvDescriptorSize = device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    mDescriptorSize = mSrvDescriptorSize;

    // Create command queue for uploads
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queueDesc.NodeMask = 0;

    HRESULT hr = device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&mOwnedCommandQueue));
    if (FAILED(hr)) {
        SPDLOG_ERROR("[RTX] TextureManager: Failed to create command queue: 0x{:08X}",
                     static_cast<uint32_t>(hr));
        return false;
    }

    // Create fence for upload synchronization
    hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&mUploadFence));
    if (FAILED(hr)) {
        SPDLOG_ERROR("[RTX] TextureManager: Failed to create upload fence: 0x{:08X}",
                     static_cast<uint32_t>(hr));
        mOwnedCommandQueue.Reset();
        return false;
    }

    mUploadFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!mUploadFenceEvent) {
        SPDLOG_ERROR("[RTX] TextureManager: Failed to create fence event");
        mUploadFence.Reset();
        mOwnedCommandQueue.Reset();
        return false;
    }

    mUploadFenceValue = 0;

    // Create dedicated command allocator and command list
    hr = device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(&mUploadCmdAllocator));
    if (FAILED(hr)) {
        SPDLOG_ERROR("[RTX] TextureManager: Failed to create upload command allocator: 0x{:08X}",
                     static_cast<uint32_t>(hr));
        CloseHandle(mUploadFenceEvent);
        mUploadFenceEvent = nullptr;
        mUploadFence.Reset();
        mOwnedCommandQueue.Reset();
        return false;
    }

    ComPtr<ID3D12GraphicsCommandList> baseCmdList;
    hr = device->CreateCommandList(
        0,
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        mUploadCmdAllocator.Get(),
        nullptr,
        IID_PPV_ARGS(&baseCmdList));
    if (FAILED(hr)) {
        SPDLOG_ERROR("[RTX] TextureManager: Failed to create upload command list: 0x{:08X}",
                     static_cast<uint32_t>(hr));
        mUploadCmdAllocator.Reset();
        CloseHandle(mUploadFenceEvent);
        mUploadFenceEvent = nullptr;
        mUploadFence.Reset();
        mOwnedCommandQueue.Reset();
        return false;
    }

    hr = baseCmdList.As(&mUploadCmdList);
    if (FAILED(hr)) {
        SPDLOG_ERROR("[RTX] TextureManager: Failed to query ID3D12GraphicsCommandList4: 0x{:08X}",
                     static_cast<uint32_t>(hr));
        mUploadCmdAllocator.Reset();
        CloseHandle(mUploadFenceEvent);
        mUploadFenceEvent = nullptr;
        mUploadFence.Reset();
        mOwnedCommandQueue.Reset();
        return false;
    }

    // Close immediately
    mUploadCmdList->Close();

    // Create ring upload buffer for staging texture data
    mUploadBuffer = CreateUploadBuffer(UPLOAD_RING_BUFFER_SIZE);
    if (mUploadBuffer) {
        mUploadBufferSize = UPLOAD_RING_BUFFER_SIZE;
        mUploadBufferOffset = 0;
    } else {
        SPDLOG_WARN("[RTX] TextureManager: ring upload buffer allocation failed, will use per-texture buffers");
        mUploadBufferSize = 0;
        mUploadBufferOffset = 0;
    }

    mNextDescriptorIndex = 0;
    mInitialized = true;

    // Create default textures (white, checkerboard, flat normal map)
    CreateDefaultTextures();

    SPDLOG_INFO("[RTX] TextureManager initialized (Device5+SRV heap path, offset: {}, descriptor size: {})",
                heapStartIndex, mSrvDescriptorSize);
    return true;
}

// ============================================================================
// Initialize (ID3D12Device + maxTextures path) — Task-spec Phase 6 exact signature
// Creates a shader-visible SRV descriptor heap, command queue, fence, and
// dedicated command allocator/list for texture uploads.
// ============================================================================

bool TextureManager::Initialize(ID3D12Device* device, uint32_t maxTextures) {
    if (mInitialized) {
        SPDLOG_WARN("[RTX] TextureManager already initialized");
        return true;
    }

    if (!device) {
        SPDLOG_ERROR("[RTX] TextureManager::Initialize(ID3D12Device*) - null device");
        return false;
    }

    mContext = nullptr;
    mRawDevice = device;
    mUsingExternalHeap = false;
    mMaxTextures = maxTextures;

    // Try to query ID3D12Device5 for DXR-specific features (optional)
    ComPtr<ID3D12Device5> device5;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device5)))) {
        mDevice = device5.Get();
    } else {
        mDevice = nullptr;
    }

    // Create the shader-visible SRV descriptor heap
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.NumDescriptors = maxTextures;
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    heapDesc.NodeMask = 0;

    HRESULT hr = device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&mSrvHeap));
    if (FAILED(hr)) {
        SPDLOG_ERROR("[RTX] TextureManager: Failed to create SRV descriptor heap ({} descriptors): 0x{:08X}",
                     maxTextures, static_cast<uint32_t>(hr));
        return false;
    }

    mSrvDescriptorSize = device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    mDescriptorSize = mSrvDescriptorSize;
    mHeapOffset = 0;

    // Create command queue for uploads
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queueDesc.NodeMask = 0;

    hr = device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&mOwnedCommandQueue));
    if (FAILED(hr)) {
        SPDLOG_ERROR("[RTX] TextureManager: Failed to create command queue: 0x{:08X}",
                     static_cast<uint32_t>(hr));
        mSrvHeap.Reset();
        return false;
    }

    // Create fence for upload synchronization
    hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&mUploadFence));
    if (FAILED(hr)) {
        SPDLOG_ERROR("[RTX] TextureManager: Failed to create upload fence: 0x{:08X}",
                     static_cast<uint32_t>(hr));
        mOwnedCommandQueue.Reset();
        mSrvHeap.Reset();
        return false;
    }

    mUploadFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!mUploadFenceEvent) {
        SPDLOG_ERROR("[RTX] TextureManager: Failed to create fence event");
        mUploadFence.Reset();
        mOwnedCommandQueue.Reset();
        mSrvHeap.Reset();
        return false;
    }

    mUploadFenceValue = 0;

    // Create command allocator and command list
    hr = device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(&mUploadCmdAllocator));
    if (FAILED(hr)) {
        SPDLOG_ERROR("[RTX] TextureManager: Failed to create upload command allocator: 0x{:08X}",
                     static_cast<uint32_t>(hr));
        CloseHandle(mUploadFenceEvent);
        mUploadFenceEvent = nullptr;
        mUploadFence.Reset();
        mOwnedCommandQueue.Reset();
        mSrvHeap.Reset();
        return false;
    }

    ComPtr<ID3D12GraphicsCommandList> baseCmdList;
    hr = device->CreateCommandList(
        0,
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        mUploadCmdAllocator.Get(),
        nullptr,
        IID_PPV_ARGS(&baseCmdList));
    if (FAILED(hr)) {
        SPDLOG_ERROR("[RTX] TextureManager: Failed to create upload command list: 0x{:08X}",
                     static_cast<uint32_t>(hr));
        mUploadCmdAllocator.Reset();
        CloseHandle(mUploadFenceEvent);
        mUploadFenceEvent = nullptr;
        mUploadFence.Reset();
        mOwnedCommandQueue.Reset();
        mSrvHeap.Reset();
        return false;
    }

    hr = baseCmdList.As(&mUploadCmdList);
    if (FAILED(hr)) {
        SPDLOG_ERROR("[RTX] TextureManager: Failed to query ID3D12GraphicsCommandList4: 0x{:08X}",
                     static_cast<uint32_t>(hr));
        mUploadCmdAllocator.Reset();
        CloseHandle(mUploadFenceEvent);
        mUploadFenceEvent = nullptr;
        mUploadFence.Reset();
        mOwnedCommandQueue.Reset();
        mSrvHeap.Reset();
        return false;
    }

    mUploadCmdList->Close();

    mNextDescriptorIndex = 0;
    mInitialized = true;

    // Create default textures (white, checkerboard, flat normal map)
    CreateDefaultTextures();

    SPDLOG_INFO("[RTX] TextureManager initialized (ID3D12Device path, SRV heap: {} descriptors, descriptor size: {})",
                maxTextures, mSrvDescriptorSize);
    return true;
}

// ============================================================================
// Initialize (ID3D12Device5 + maxTextures path)
// ============================================================================

bool TextureManager::Initialize(ID3D12Device5* device, uint32_t maxTextures) {
    if (mInitialized) {
        SPDLOG_WARN("[RTX] TextureManager already initialized");
        return true;
    }

    if (!device) {
        SPDLOG_ERROR("[RTX] TextureManager::Initialize - null device");
        return false;
    }

    mContext = nullptr;
    mDevice = device;
    mRawDevice = device;
    mUsingExternalHeap = false;
    mMaxTextures = maxTextures;

    // Create the shader-visible SRV descriptor heap
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.NumDescriptors = maxTextures;
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    heapDesc.NodeMask = 0;

    HRESULT hr = device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&mSrvHeap));
    if (FAILED(hr)) {
        SPDLOG_ERROR("[RTX] TextureManager: Failed to create SRV descriptor heap ({} descriptors): 0x{:08X}",
                     maxTextures, static_cast<uint32_t>(hr));
        return false;
    }

    mSrvDescriptorSize = device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    mDescriptorSize = mSrvDescriptorSize;
    mHeapOffset = 0;

    // Create command queue for uploads
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queueDesc.NodeMask = 0;

    hr = device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&mOwnedCommandQueue));
    if (FAILED(hr)) {
        SPDLOG_ERROR("[RTX] TextureManager: Failed to create command queue: 0x{:08X}",
                     static_cast<uint32_t>(hr));
        mSrvHeap.Reset();
        return false;
    }

    // Create fence for upload synchronization
    hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&mUploadFence));
    if (FAILED(hr)) {
        SPDLOG_ERROR("[RTX] TextureManager: Failed to create upload fence: 0x{:08X}",
                     static_cast<uint32_t>(hr));
        mOwnedCommandQueue.Reset();
        mSrvHeap.Reset();
        return false;
    }

    mUploadFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!mUploadFenceEvent) {
        SPDLOG_ERROR("[RTX] TextureManager: Failed to create fence event");
        mUploadFence.Reset();
        mOwnedCommandQueue.Reset();
        mSrvHeap.Reset();
        return false;
    }

    mUploadFenceValue = 0;

    // Create command allocator and command list
    hr = device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(&mUploadCmdAllocator));
    if (FAILED(hr)) {
        SPDLOG_ERROR("[RTX] TextureManager: Failed to create upload command allocator: 0x{:08X}",
                     static_cast<uint32_t>(hr));
        CloseHandle(mUploadFenceEvent);
        mUploadFenceEvent = nullptr;
        mUploadFence.Reset();
        mOwnedCommandQueue.Reset();
        mSrvHeap.Reset();
        return false;
    }

    ComPtr<ID3D12GraphicsCommandList> baseCmdList;
    hr = device->CreateCommandList(
        0,
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        mUploadCmdAllocator.Get(),
        nullptr,
        IID_PPV_ARGS(&baseCmdList));
    if (FAILED(hr)) {
        SPDLOG_ERROR("[RTX] TextureManager: Failed to create upload command list: 0x{:08X}",
                     static_cast<uint32_t>(hr));
        mUploadCmdAllocator.Reset();
        CloseHandle(mUploadFenceEvent);
        mUploadFenceEvent = nullptr;
        mUploadFence.Reset();
        mOwnedCommandQueue.Reset();
        mSrvHeap.Reset();
        return false;
    }

    hr = baseCmdList.As(&mUploadCmdList);
    if (FAILED(hr)) {
        SPDLOG_ERROR("[RTX] TextureManager: Failed to query ID3D12GraphicsCommandList4: 0x{:08X}",
                     static_cast<uint32_t>(hr));
        mUploadCmdAllocator.Reset();
        CloseHandle(mUploadFenceEvent);
        mUploadFenceEvent = nullptr;
        mUploadFence.Reset();
        mOwnedCommandQueue.Reset();
        mSrvHeap.Reset();
        return false;
    }

    mUploadCmdList->Close();

    mNextDescriptorIndex = 0;
    mInitialized = true;

    // Create default textures (white, checkerboard, flat normal map)
    CreateDefaultTextures();

    SPDLOG_INFO("[RTX] TextureManager initialized (Device5 path, SRV heap: {} descriptors, descriptor size: {})",
                maxTextures, mSrvDescriptorSize);
    return true;
}

// ============================================================================
// Initialize (ID3D12Device5* + ID3D12GraphicsCommandList4*)
// ============================================================================

void TextureManager::Initialize(ID3D12Device5* device, ID3D12GraphicsCommandList4* cmdList) {
    if (mInitialized) {
        SPDLOG_WARN("[RTX] TextureManager already initialized");
        return;
    }

    if (!device) {
        SPDLOG_ERROR("[RTX] TextureManager::Initialize(Device5, cmdList) - null device");
        return;
    }

    mContext = nullptr;
    mDevice = device;
    mRawDevice = device;
    mExternalCmdList = cmdList;
    mUsingExternalHeap = false;
    mMaxTextures = MAX_TEXTURES;

    // Create the shader-visible SRV descriptor heap
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.NumDescriptors = MAX_TEXTURES;
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    heapDesc.NodeMask = 0;

    HRESULT hr = device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&mSrvHeap));
    if (FAILED(hr)) {
        SPDLOG_ERROR("[RTX] TextureManager: Failed to create SRV descriptor heap ({} descriptors): 0x{:08X}",
                     MAX_TEXTURES, static_cast<uint32_t>(hr));
        return;
    }

    mSrvDescriptorSize = device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    mDescriptorSize = mSrvDescriptorSize;
    mHeapOffset = 0;

    // Create command queue for uploads
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queueDesc.NodeMask = 0;

    hr = device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&mOwnedCommandQueue));
    if (FAILED(hr)) {
        SPDLOG_ERROR("[RTX] TextureManager: Failed to create command queue: 0x{:08X}",
                     static_cast<uint32_t>(hr));
        mSrvHeap.Reset();
        return;
    }

    // Create fence for upload synchronization
    hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&mUploadFence));
    if (FAILED(hr)) {
        SPDLOG_ERROR("[RTX] TextureManager: Failed to create upload fence: 0x{:08X}",
                     static_cast<uint32_t>(hr));
        mOwnedCommandQueue.Reset();
        mSrvHeap.Reset();
        return;
    }

    mUploadFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!mUploadFenceEvent) {
        SPDLOG_ERROR("[RTX] TextureManager: Failed to create fence event");
        mUploadFence.Reset();
        mOwnedCommandQueue.Reset();
        mSrvHeap.Reset();
        return;
    }

    mUploadFenceValue = 0;

    // Create command allocator and command list for internal uploads
    hr = device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(&mUploadCmdAllocator));
    if (FAILED(hr)) {
        SPDLOG_ERROR("[RTX] TextureManager: Failed to create upload command allocator: 0x{:08X}",
                     static_cast<uint32_t>(hr));
        CloseHandle(mUploadFenceEvent);
        mUploadFenceEvent = nullptr;
        mUploadFence.Reset();
        mOwnedCommandQueue.Reset();
        mSrvHeap.Reset();
        return;
    }

    ComPtr<ID3D12GraphicsCommandList> baseCmdList;
    hr = device->CreateCommandList(
        0,
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        mUploadCmdAllocator.Get(),
        nullptr,
        IID_PPV_ARGS(&baseCmdList));
    if (FAILED(hr)) {
        SPDLOG_ERROR("[RTX] TextureManager: Failed to create upload command list: 0x{:08X}",
                     static_cast<uint32_t>(hr));
        mUploadCmdAllocator.Reset();
        CloseHandle(mUploadFenceEvent);
        mUploadFenceEvent = nullptr;
        mUploadFence.Reset();
        mOwnedCommandQueue.Reset();
        mSrvHeap.Reset();
        return;
    }

    hr = baseCmdList.As(&mUploadCmdList);
    if (FAILED(hr)) {
        SPDLOG_ERROR("[RTX] TextureManager: Failed to query ID3D12GraphicsCommandList4: 0x{:08X}",
                     static_cast<uint32_t>(hr));
        mUploadCmdAllocator.Reset();
        CloseHandle(mUploadFenceEvent);
        mUploadFenceEvent = nullptr;
        mUploadFence.Reset();
        mOwnedCommandQueue.Reset();
        mSrvHeap.Reset();
        return;
    }

    mUploadCmdList->Close();

    mNextDescriptorIndex = 0;
    mInitialized = true;

    // Create default textures (white, checkerboard, flat normal map)
    CreateDefaultTextures();

    SPDLOG_INFO("[RTX] TextureManager initialized (Device5+CmdList path, SRV heap: {} descriptors, descriptor size: {})",
                MAX_TEXTURES, mSrvDescriptorSize);
}

// ============================================================================
// Shutdown
// ============================================================================

void TextureManager::Shutdown() {
    if (!mInitialized) return;

    // Wait for GPU if using DX12Device path
    if (mContext) {
        mContext->WaitForGPU();
    } else if (mOwnedCommandQueue && mUploadFence) {
        // Wait on our own fence
        mUploadFenceValue++;
        mOwnedCommandQueue->Signal(mUploadFence.Get(), mUploadFenceValue);
        if (mUploadFence->GetCompletedValue() < mUploadFenceValue) {
            mUploadFence->SetEventOnCompletion(mUploadFenceValue, mUploadFenceEvent);
            WaitForSingleObject(mUploadFenceEvent, INFINITE);
        }
    }

    {
        std::lock_guard<std::mutex> lock(mCacheMutex);

        // Clear LRU cache
        mLRUCache.clear();
        mLRUList.clear();
        mFreeSRVIndices.clear();

        // Clear Phase 6 resource cache
        mResourceCache.clear();

        // Clear Phase 6 RTXTextureEntry cache
        mTextureEntryCache.clear();

        // Clear Phase 6 TextureCacheEntry map
        mTextureCacheEntries.clear();

        // Clear legacy caches
        mTextureCache.clear();
        mCache.clear();
        mTextureResources.clear();
        mNextDescriptorIndex = 0;
    }

    // Release default textures
    mDefaultWhiteTexture.gpuTexture.Reset();
    mDefaultWhiteRTXTexture.resource.Reset();
    mDefaultWhiteEntry.gpuTexture.Reset();
    mDefaultWhiteCreated = false;

    mDefaultNormalMapTexture.gpuTexture.Reset();
    mDefaultNormalMapCreated = false;

    mCheckerboardFallback.gpuTexture.Reset();
    mCheckerboardCreated = false;

    mUploadCmdList.Reset();
    mUploadCmdAllocator.Reset();
    mSrvHeap.Reset();

    // Release ring upload buffer
    mUploadBuffer.Reset();
    mUploadBufferSize = 0;
    mUploadBufferOffset = 0;

    // Clean up explicit-device-path resources
    if (mUploadFenceEvent) {
        CloseHandle(mUploadFenceEvent);
        mUploadFenceEvent = nullptr;
    }
    mUploadFence.Reset();
    mOwnedCommandQueue.Reset();

    mSrvDescriptorSize = 0;
    mDescriptorSize = 0;
    mHeapOffset = 0;
    mMaxTextures = MAX_TEXTURES;
    mContext = nullptr;
    mDevice = nullptr;
    mRawDevice = nullptr;
    mExternalCmdList = nullptr;
    mExternalSrvHeap = nullptr;
    mUsingExternalHeap = false;
    mCurrentFrame = 0;
    mInitialized = false;

    SPDLOG_INFO("[RTX] TextureManager shut down");
}

// ============================================================================
// InvalidateCache
// ============================================================================

void TextureManager::InvalidateCache() {
    if (mContext) {
        mContext->WaitForGPU();
    }

    std::lock_guard<std::mutex> lock(mCacheMutex);

    size_t count = mLRUCache.size() + mResourceCache.size() + mTextureEntryCache.size() + mTextureCache.size() + mCache.size() + mTextureCacheEntries.size();

    // Clear LRU cache
    mLRUCache.clear();
    mLRUList.clear();
    mFreeSRVIndices.clear();

    // Clear Phase 6 resource cache
    mResourceCache.clear();

    // Clear Phase 6 RTXTextureEntry cache
    mTextureEntryCache.clear();

    // Clear Phase 6 TextureCacheEntry map
    mTextureCacheEntries.clear();

    // Clear legacy caches
    mTextureCache.clear();
    mCache.clear();
    mTextureResources.clear();

    // Reset SRV index past the reserved slots (0=white, 1=checkerboard, 2=flat normal)
    mNextDescriptorIndex = RESERVED_SRV_SLOTS;

    SPDLOG_INFO("[RTX] TextureManager: cache invalidated ({} textures released)", count);
}

// ============================================================================
// InvalidateAll — Task-spec Phase 6 alias for InvalidateCache
// ============================================================================

void TextureManager::InvalidateAll() {
    InvalidateCache();
}

// ============================================================================
// ReleaseAll
// ============================================================================

void TextureManager::ReleaseAll() {
    if (mContext) {
        mContext->WaitForGPU();
    }

    std::lock_guard<std::mutex> lock(mCacheMutex);

    mLRUCache.clear();
    mLRUList.clear();
    mFreeSRVIndices.clear();

    mResourceCache.clear();
    mTextureEntryCache.clear();
    mTextureCacheEntries.clear();
    mTextureCache.clear();
    mCache.clear();
    mTextureResources.clear();
    mNextDescriptorIndex = 0;

    // Default textures are also released
    mDefaultWhiteTexture.gpuTexture.Reset();
    mDefaultWhiteRTXTexture.resource.Reset();
    mDefaultWhiteEntry.gpuTexture.Reset();
    mDefaultWhiteCreated = false;

    mDefaultNormalMapTexture.gpuTexture.Reset();
    mDefaultNormalMapCreated = false;

    mCheckerboardFallback.gpuTexture.Reset();
    mCheckerboardCreated = false;

    SPDLOG_INFO("[RTX] TextureManager: released all textures");
}

// ============================================================================
// Descriptor heap access
// ============================================================================

ID3D12DescriptorHeap* TextureManager::GetSRVHeap() const {
    if (mUsingExternalHeap) {
        return mExternalSrvHeap;
    }
    return mSrvHeap.Get();
}

D3D12_GPU_DESCRIPTOR_HANDLE TextureManager::GetSRVTableStart() const {
    auto* heap = GetSRVHeap();
    if (!heap) {
        D3D12_GPU_DESCRIPTOR_HANDLE nullHandle = {};
        nullHandle.ptr = 0;
        return nullHandle;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE handle = heap->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<UINT64>(mHeapOffset) * mSrvDescriptorSize;
    return handle;
}

D3D12_GPU_DESCRIPTOR_HANDLE TextureManager::GetGPUHandle(uint32_t index) const {
    auto* heap = GetSRVHeap();
    if (!heap) {
        D3D12_GPU_DESCRIPTOR_HANDLE nullHandle = {};
        nullHandle.ptr = 0;
        return nullHandle;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE handle = heap->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<UINT64>(mHeapOffset + index) * mSrvDescriptorSize;
    return handle;
}

D3D12_CPU_DESCRIPTOR_HANDLE TextureManager::GetCPUHandle(uint32_t index) const {
    auto* heap = GetSRVHeap();
    if (!heap) {
        D3D12_CPU_DESCRIPTOR_HANDLE nullHandle = {};
        nullHandle.ptr = 0;
        return nullHandle;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE handle = heap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(mHeapOffset + index) * mSrvDescriptorSize;
    return handle;
}

uint32_t TextureManager::GetTextureCount() const {
    std::lock_guard<std::mutex> lock(mCacheMutex);
    return static_cast<uint32_t>(mLRUCache.size() + mResourceCache.size() + mTextureEntryCache.size() + mTextureCache.size() + mCache.size() + mTextureCacheEntries.size());
}

uint32_t TextureManager::GetNextSRVIndex() const {
    std::lock_guard<std::mutex> lock(mCacheMutex);
    return mNextDescriptorIndex;
}

RTXTextureInfo* TextureManager::FindTexture(uint64_t hash) {
    std::lock_guard<std::mutex> lock(mCacheMutex);
    auto it = mTextureCache.find(hash);
    if (it != mTextureCache.end()) {
        return &it->second;
    }
    return nullptr;
}

uint32_t TextureManager::GetSRVIndexForHash(uint64_t hash) const {
    std::lock_guard<std::mutex> lock(mCacheMutex);

    // Check LRU cache first
    auto lruIt = mLRUCache.find(hash);
    if (lruIt != mLRUCache.end()) {
        return lruIt->second.texture.srvIndex;
    }

    // Check legacy cache
    auto it = mTextureCache.find(hash);
    if (it != mTextureCache.end()) {
        return it->second.srvIndex;
    }
    return 0; // Default white texture at index 0
}

// ============================================================================
// LRU Eviction
// ============================================================================

uint32_t TextureManager::EvictLRUTexture() {
    // The back of the LRU list is the least recently used
    if (mLRUList.empty()) {
        return UINT32_MAX;
    }

    uint64_t evictKey = mLRUList.back();
    mLRUList.pop_back();

    auto it = mLRUCache.find(evictKey);
    if (it == mLRUCache.end()) {
        SPDLOG_ERROR("[RTX] TextureManager::EvictLRUTexture: LRU key not found in cache");
        return UINT32_MAX;
    }

    uint32_t freedIndex = it->second.texture.srvIndex;

    // Release the GPU resource
    it->second.texture.gpuTexture.Reset();

    // Remove from cache
    mLRUCache.erase(it);

    SPDLOG_DEBUG("[RTX] TextureManager: evicted LRU texture (hash: 0x{:016X}, SRV index: {})",
                 evictKey, freedIndex);

    return freedIndex;
}

uint32_t TextureManager::AllocateSRVIndex() {
    // First check if we have a recycled free index from eviction
    if (!mFreeSRVIndices.empty()) {
        uint32_t idx = mFreeSRVIndices.back();
        mFreeSRVIndices.pop_back();
        return idx;
    }

    // Next try to use a new index from the heap
    if (mNextDescriptorIndex < mMaxTextures) {
        uint32_t idx = mNextDescriptorIndex;
        mNextDescriptorIndex++;
        return idx;
    }

    // Heap is full, try to evict LRU texture
    uint32_t freedIdx = EvictLRUTexture();
    if (freedIdx != UINT32_MAX) {
        return freedIdx;
    }

    SPDLOG_ERROR("[RTX] TextureManager::AllocateSRVIndex: no SRV slots available (max: {})", mMaxTextures);
    return UINT32_MAX;
}

// ============================================================================
// ConvertN64Texture - static N64 format conversion to RGBA32
// ============================================================================

std::vector<uint8_t> TextureManager::ConvertN64Texture(
    const uint8_t* tmem,
    uint32_t fmt, uint32_t siz,
    uint32_t width, uint32_t height,
    const uint8_t* tlut,
    uint32_t tlutFormat)
{
    if (!tmem || width == 0 || height == 0) {
        return {};
    }

    switch (fmt) {
        case G_IM_FMT_RGBA:
            if (siz == G_IM_SIZ_16b) {
                return DecodeRGBA16(tmem, width, height);
            } else if (siz == G_IM_SIZ_32b) {
                return DecodeRGBA32(tmem, width, height);
            }
            break;

        case G_IM_FMT_CI:
            if (tlut) {
                if (siz == G_IM_SIZ_4b) {
                    return DecodeCI4(tmem, width, height, tlut, tlutFormat);
                } else if (siz == G_IM_SIZ_8b) {
                    return DecodeCI8(tmem, width, height, tlut, tlutFormat);
                }
            } else {
                // No palette provided for CI format: output magenta as debug indicator
                SPDLOG_WARN("[RTX] ConvertN64Texture: CI format without TLUT, outputting magenta debug color");
                std::vector<uint8_t> magenta(static_cast<size_t>(width) * height * 4);
                for (size_t i = 0; i < static_cast<size_t>(width) * height; i++) {
                    magenta[i * 4 + 0] = 255;  // R
                    magenta[i * 4 + 1] = 0;    // G
                    magenta[i * 4 + 2] = 255;  // B
                    magenta[i * 4 + 3] = 255;  // A
                }
                return magenta;
            }
            break;

        case G_IM_FMT_IA:
            if (siz == G_IM_SIZ_4b) {
                return DecodeIA4(tmem, width, height);
            } else if (siz == G_IM_SIZ_8b) {
                return DecodeIA8(tmem, width, height);
            } else if (siz == G_IM_SIZ_16b) {
                return DecodeIA16(tmem, width, height);
            }
            break;

        case G_IM_FMT_I:
            if (siz == G_IM_SIZ_4b) {
                return DecodeI4(tmem, width, height);
            } else if (siz == G_IM_SIZ_8b) {
                return DecodeI8(tmem, width, height);
            }
            break;

        case G_IM_FMT_YUV:
            // YUV is rare in OoT; fill with magenta debug color
            SPDLOG_WARN("[RTX] ConvertN64Texture: YUV format not supported, using magenta");
            {
                std::vector<uint8_t> out(static_cast<size_t>(width) * height * 4);
                for (size_t i = 0; i < static_cast<size_t>(width) * height; i++) {
                    out[i * 4 + 0] = 255;
                    out[i * 4 + 1] = 0;
                    out[i * 4 + 2] = 255;
                    out[i * 4 + 3] = 255;
                }
                return out;
            }

        default:
            break;
    }

    SPDLOG_WARN("[RTX] ConvertN64Texture: unsupported format (fmt={}, siz={})", fmt, siz);

    // Fill with magenta for visibility
    std::vector<uint8_t> out(static_cast<size_t>(width) * height * 4);
    for (size_t i = 0; i < static_cast<size_t>(width) * height; i++) {
        out[i * 4 + 0] = 255;
        out[i * 4 + 1] = 0;
        out[i * 4 + 2] = 255;
        out[i * 4 + 3] = 255;
    }
    return out;
}

// ============================================================================
// GetOrCreateTexture - Primary entry point returning RTXTextureHandle
// Uses hash key = format + address + width + height + paletteCRC
// With LRU eviction when cache is full.
// ============================================================================

RTXTextureHandle TextureManager::GetOrCreateTexture(
    const uint8_t* textureData,
    uint32_t format,
    uint32_t width, uint32_t height,
    const uint8_t* palette,
    uint32_t paletteCRC)
{
    RTXTextureHandle fallback = GetFallbackCheckerboardHandle();

    if (!mInitialized) {
        SPDLOG_ERROR("[RTX] TextureManager::GetOrCreateTexture called before Initialize");
        return fallback;
    }

    if (!textureData || width == 0 || height == 0) {
        SPDLOG_WARN("[RTX] TextureManager::GetOrCreateTexture: invalid texture data");
        return fallback;
    }

    if (width > 4096 || height > 4096) {
        SPDLOG_WARN("[RTX] TextureManager::GetOrCreateTexture: texture too large ({}x{})",
                     width, height);
        return fallback;
    }

    // Compute cache key from format + address + width + height + paletteCRC
    uint64_t hashKey = ComputeTextureHash(format, reinterpret_cast<uintptr_t>(textureData),
                                           width, height, paletteCRC);

    // Check LRU cache (with lock)
    {
        std::lock_guard<std::mutex> lock(mCacheMutex);
        auto it = mLRUCache.find(hashKey);
        if (it != mLRUCache.end()) {
            // Cache hit: move to front of LRU list and update last used frame
            it->second.texture.lastUsedFrame = mCurrentFrame;
            mLRUList.erase(it->second.lruIter);
            mLRUList.push_front(hashKey);
            it->second.lruIter = mLRUList.begin();

            RTXTextureHandle handle;
            handle.srvIndex = it->second.texture.srvIndex;
            handle.textureIndex = it->second.texture.srvIndex;
            handle.width = it->second.texture.width;
            handle.height = it->second.texture.height;
            handle.gpuHandle = it->second.texture.srvGpuHandle;
            return handle;
        }
    }

    // Cache miss: decode N64 texels into RGBA8
    uint32_t imgFmt = (format >> 4) & 0x0F;
    uint32_t imgSiz = format & 0x0F;

    // Handle alternative format encoding
    if (imgFmt > G_IM_FMT_I) {
        imgFmt = format / 4;
        imgSiz = format % 4;
    }

    std::vector<uint8_t> rgba8 = ConvertN64Texture(textureData, imgFmt, imgSiz,
                                                     width, height, palette, G_IM_SIZ_16b);
    if (rgba8.empty()) {
        SPDLOG_ERROR("[RTX] TextureManager::GetOrCreateTexture: decode failed (hash: 0x{:016X})", hashKey);
        return fallback;
    }

    // Allocate SRV index (may evict LRU if full)
    uint32_t srvIndex;
    {
        std::lock_guard<std::mutex> lock(mCacheMutex);

        // Check if LRU cache is at capacity - evict before allocating
        while (mLRUCache.size() >= mMaxCachedTextures) {
            uint32_t freedIdx = EvictLRUTexture();
            if (freedIdx != UINT32_MAX) {
                mFreeSRVIndices.push_back(freedIdx);
            } else {
                break;
            }
        }

        srvIndex = AllocateSRVIndex();
    }

    if (srvIndex == UINT32_MAX) {
        SPDLOG_ERROR("[RTX] TextureManager::GetOrCreateTexture: no SRV slots available");
        return fallback;
    }

    // Create GPU texture resource and upload
    CachedTexture cachedTex;
    cachedTex.hashKey = hashKey;
    cachedTex.width = width;
    cachedTex.height = height;
    cachedTex.srvIndex = srvIndex;
    cachedTex.dxgiFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    cachedTex.lastUsedFrame = mCurrentFrame;
    cachedTex.uploaded = false;

    if (!CreateGPUTextureAtIndex(rgba8.data(), width, height, srvIndex, cachedTex)) {
        SPDLOG_ERROR("[RTX] TextureManager::GetOrCreateTexture: GPU upload failed ({}x{}, hash: 0x{:016X})",
                     width, height, hashKey);
        // Return the freed SRV index to the pool
        std::lock_guard<std::mutex> lock(mCacheMutex);
        mFreeSRVIndices.push_back(srvIndex);
        return fallback;
    }

    // Build handle
    RTXTextureHandle handle;
    handle.srvIndex = srvIndex;
    handle.textureIndex = srvIndex;
    handle.width = width;
    handle.height = height;
    handle.gpuHandle = cachedTex.srvGpuHandle;

    // Insert into LRU cache
    {
        std::lock_guard<std::mutex> lock(mCacheMutex);
        mLRUList.push_front(hashKey);
        LRUCacheEntry entry;
        entry.texture = std::move(cachedTex);
        entry.lruIter = mLRUList.begin();
        mLRUCache.emplace(hashKey, std::move(entry));
    }

    SPDLOG_DEBUG("[RTX] TextureManager: loaded texture {}x{} fmt=0x{:02X} at SRV index {} (hash: 0x{:016X})",
                 width, height, format, srvIndex, hashKey);
    return handle;
}

// ============================================================================
// GetOrCreateTexture (Phase 6 task-spec: tmemData, tmemSize, format, size, w, h, palette)
// Returns RTXTextureResource* — the exact signature specified by Phase 6.
// ============================================================================

RTXTextureResource* TextureManager::GetOrCreateTexture(
    const uint8_t* tmemData, uint32_t tmemSize,
    uint32_t format, uint32_t size,
    uint32_t width, uint32_t height,
    const uint8_t* palette)
{
    if (!mInitialized) {
        SPDLOG_ERROR("[RTX] TextureManager::GetOrCreateTexture(tmem) called before Initialize");
        return nullptr;
    }

    if (!tmemData || tmemSize == 0 || width == 0 || height == 0) {
        SPDLOG_WARN("[RTX] TextureManager::GetOrCreateTexture(tmem): invalid texture data");
        return nullptr;
    }

    if (width > 4096 || height > 4096) {
        SPDLOG_WARN("[RTX] TextureManager::GetOrCreateTexture(tmem): texture too large ({}x{})",
                     width, height);
        return nullptr;
    }

    // Compute cache hash from TMEM data, format parameters, and optional palette
    // Uses FNV-1a over the raw data, then mixes in format/size/width/height
    uint64_t hashKey = HashTextureData(tmemData, tmemSize);

    // Mix in format parameters to distinguish same data at different decode settings
    constexpr uint64_t FNV_PRIME = 0x100000001b3ULL;
    hashKey ^= static_cast<uint64_t>(format);
    hashKey *= FNV_PRIME;
    hashKey ^= static_cast<uint64_t>(size);
    hashKey *= FNV_PRIME;
    hashKey ^= static_cast<uint64_t>(width);
    hashKey *= FNV_PRIME;
    hashKey ^= static_cast<uint64_t>(height);
    hashKey *= FNV_PRIME;

    // Mix in palette data if present (CI formats)
    if (palette && format == G_IM_FMT_CI) {
        size_t paletteBytes = (size == G_IM_SIZ_4b) ? 32 : 512; // 16 or 256 entries * 2 bytes
        uint64_t palHash = HashTextureData(palette, paletteBytes);
        hashKey ^= palHash;
        hashKey *= FNV_PRIME;
    }

    // Check resource cache (with lock)
    {
        std::lock_guard<std::mutex> lock(mCacheMutex);
        auto it = mResourceCache.find(hashKey);
        if (it != mResourceCache.end()) {
            return &it->second;
        }
    }

    // Cache miss: decode N64 texels into RGBA8
    std::vector<uint8_t> rgba8 = ConvertN64Texture(tmemData, format, size,
                                                     width, height, palette, G_IM_SIZ_16b);
    if (rgba8.empty()) {
        SPDLOG_ERROR("[RTX] TextureManager::GetOrCreateTexture(tmem): decode failed (hash: 0x{:016X})",
                     hashKey);
        return nullptr;
    }

    // Allocate SRV index (may evict LRU if full)
    uint32_t srvIndex;
    {
        std::lock_guard<std::mutex> lock(mCacheMutex);
        srvIndex = AllocateSRVIndex();
    }

    if (srvIndex == UINT32_MAX) {
        SPDLOG_ERROR("[RTX] TextureManager::GetOrCreateTexture(tmem): no SRV slots available");
        return nullptr;
    }

    // Create GPU texture resource
    ID3D12Device* device = mRawDevice;
    if (!device) {
        std::lock_guard<std::mutex> lock(mCacheMutex);
        mFreeSRVIndices.push_back(srvIndex);
        return nullptr;
    }

    // Step 1: Create the destination TEXTURE2D on the default heap
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
    texDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    D3D12_HEAP_PROPERTIES defaultHeap = {};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

    ComPtr<ID3D12Resource> textureResource;
    HRESULT hr = device->CreateCommittedResource(
        &defaultHeap,
        D3D12_HEAP_FLAG_NONE,
        &texDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(&textureResource));
    if (FAILED(hr)) {
        SPDLOG_ERROR("[RTX] TextureManager::GetOrCreateTexture(tmem): CreateCommittedResource failed ({}x{}): 0x{:08X}",
                     width, height, static_cast<uint32_t>(hr));
        std::lock_guard<std::mutex> lock(mCacheMutex);
        mFreeSRVIndices.push_back(srvIndex);
        return nullptr;
    }

    // Step 2: Query upload layout and create staging (upload) buffer
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT numRows = 0;
    UINT64 rowSizeInBytes = 0;
    UINT64 totalBytes = 0;

    device->GetCopyableFootprints(&texDesc, 0, 1, 0,
                                   &footprint, &numRows, &rowSizeInBytes, &totalBytes);

    ComPtr<ID3D12Resource> uploadBuffer = CreateUploadBuffer(totalBytes);
    if (!uploadBuffer) {
        SPDLOG_ERROR("[RTX] TextureManager::GetOrCreateTexture(tmem): staging buffer creation failed ({} bytes)",
                     totalBytes);
        std::lock_guard<std::mutex> lock(mCacheMutex);
        mFreeSRVIndices.push_back(srvIndex);
        return nullptr;
    }

    // Step 3: Map staging buffer and copy pixel data row-by-row
    void* mapped = nullptr;
    hr = uploadBuffer->Map(0, nullptr, &mapped);
    if (FAILED(hr)) {
        SPDLOG_ERROR("[RTX] TextureManager::GetOrCreateTexture(tmem): Map staging buffer failed: 0x{:08X}",
                     static_cast<uint32_t>(hr));
        std::lock_guard<std::mutex> lock(mCacheMutex);
        mFreeSRVIndices.push_back(srvIndex);
        return nullptr;
    }

    const uint32_t srcRowPitch = width * 4; // RGBA8 = 4 bytes per pixel
    const uint32_t dstRowPitch = footprint.Footprint.RowPitch;
    uint8_t* dstBase = static_cast<uint8_t*>(mapped) + footprint.Offset;

    for (uint32_t row = 0; row < height; row++) {
        const uint8_t* srcRow = rgba8.data() + row * srcRowPitch;
        uint8_t* dstRow = dstBase + row * dstRowPitch;
        memcpy(dstRow, srcRow, srcRowPitch);
    }

    uploadBuffer->Unmap(0, nullptr);

    // Step 4: Record copy command and resource barrier
    hr = mUploadCmdAllocator->Reset();
    if (FAILED(hr)) {
        SPDLOG_ERROR("[RTX] TextureManager::GetOrCreateTexture(tmem): Reset command allocator failed: 0x{:08X}",
                     static_cast<uint32_t>(hr));
        std::lock_guard<std::mutex> lock(mCacheMutex);
        mFreeSRVIndices.push_back(srvIndex);
        return nullptr;
    }

    hr = mUploadCmdList->Reset(mUploadCmdAllocator.Get(), nullptr);
    if (FAILED(hr)) {
        SPDLOG_ERROR("[RTX] TextureManager::GetOrCreateTexture(tmem): Reset command list failed: 0x{:08X}",
                     static_cast<uint32_t>(hr));
        std::lock_guard<std::mutex> lock(mCacheMutex);
        mFreeSRVIndices.push_back(srvIndex);
        return nullptr;
    }

    D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
    dstLoc.pResource = textureResource.Get();
    dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dstLoc.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
    srcLoc.pResource = uploadBuffer.Get();
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcLoc.PlacedFootprint = footprint;

    mUploadCmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

    // Transition from COPY_DEST to shader resource
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = textureResource.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    mUploadCmdList->ResourceBarrier(1, &barrier);

    // Execute and wait for GPU
    ExecuteUploadAndWait();

    // Step 5: Create SRV descriptor at the allocated index
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.PlaneSlice = 0;
    srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = GetCPUHandle(srvIndex);
    device->CreateShaderResourceView(textureResource.Get(), &srvDesc, cpuHandle);

    // Step 6: Build RTXTextureResource and insert into cache
    RTXTextureResource resource;
    resource.texture = std::move(textureResource);
    resource.uploadBuffer = std::move(uploadBuffer);
    resource.srvIndex = srvIndex;
    resource.width = width;
    resource.height = height;
    resource.format = DXGI_FORMAT_R8G8B8A8_UNORM;
    resource.hash = hashKey;

    {
        std::lock_guard<std::mutex> lock(mCacheMutex);
        auto result = mResourceCache.emplace(hashKey, std::move(resource));
        SPDLOG_DEBUG("[RTX] TextureManager::GetOrCreateTexture(tmem): loaded texture {}x{} fmt={} siz={} at SRV index {} (hash: 0x{:016X})",
                     width, height, format, size, srvIndex, hashKey);
        return &result.first->second;
    }
}

// ============================================================================
// GetTextureByHash - Phase 6 task-spec: look up RTXTextureResource by hash
// ============================================================================

RTXTextureResource* TextureManager::GetTextureByHash(uint64_t hash) {
    std::lock_guard<std::mutex> lock(mCacheMutex);
    auto it = mResourceCache.find(hash);
    if (it != mResourceCache.end()) {
        return &it->second;
    }
    return nullptr;
}

// ============================================================================
// GetOrConvertTexture - RTXTexture version (legacy entry point)
// ============================================================================

RTXTexture* TextureManager::GetOrConvertTexture(
    uint64_t hashKey, const uint8_t* rawData,
    uint32_t width, uint32_t height,
    uint32_t n64Format, uint32_t n64Size,
    const uint8_t* palette)
{
    if (!mInitialized) {
        SPDLOG_ERROR("[RTX] TextureManager::GetOrConvertTexture called before Initialize");
        return nullptr;
    }

    // Check cache first
    {
        std::lock_guard<std::mutex> lock(mCacheMutex);
        auto it = mCache.find(hashKey);
        if (it != mCache.end()) {
            return &it->second;
        }
    }

    // Validate input
    if (!rawData || width == 0 || height == 0) {
        SPDLOG_WARN("[RTX] TextureManager::GetOrConvertTexture: invalid texture data (hash: 0x{:016X})", hashKey);
        return nullptr;
    }

    if (width > 4096 || height > 4096) {
        SPDLOG_WARN("[RTX] TextureManager::GetOrConvertTexture: texture too large ({}x{}, hash: 0x{:016X})",
                     width, height, hashKey);
        return nullptr;
    }

    // Check capacity
    if (mNextDescriptorIndex >= mMaxTextures && mFreeSRVIndices.empty()) {
        SPDLOG_WARN("[RTX] TextureManager::GetOrConvertTexture: SRV heap full ({} textures)", mMaxTextures);
        return nullptr;
    }

    // Decode N64 texels into RGBA8 format
    std::vector<uint8_t> rgba8 = ConvertN64Texture(rawData, n64Format, n64Size, width, height, palette, G_IM_SIZ_16b);

    if (rgba8.empty()) {
        SPDLOG_ERROR("[RTX] TextureManager::GetOrConvertTexture: decode produced empty data (hash: 0x{:016X})", hashKey);
        return nullptr;
    }

    // Upload to GPU and create SRV
    RTXTexture texture;
    if (!CreateGPUTextureLegacy(rgba8.data(), width, height, texture)) {
        SPDLOG_ERROR("[RTX] TextureManager::GetOrConvertTexture: GPU upload failed ({}x{}, hash: 0x{:016X})",
                     width, height, hashKey);
        return nullptr;
    }
    texture.n64Hash = hashKey;

    // Insert into cache
    {
        std::lock_guard<std::mutex> lock(mCacheMutex);
        auto result = mCache.emplace(hashKey, std::move(texture));
        SPDLOG_DEBUG("[RTX] TextureManager::GetOrConvertTexture: loaded texture {}x{} fmt={} siz={} at SRV index {} (hash: 0x{:016X})",
                     width, height, n64Format, n64Size, result.first->second.descriptorIndex, hashKey);
        return &result.first->second;
    }
}

// ============================================================================
// GetOrCreateTextureInfo - main entry point (RTXTextureInfo version)
// ============================================================================

RTXTextureInfo* TextureManager::GetOrCreateTextureInfo(
    uint64_t hash,
    const uint8_t* tmem,
    uint32_t fmt, uint32_t siz,
    uint32_t width, uint32_t height,
    const uint8_t* tlut,
    uint32_t tlutFormat)
{
    if (!mInitialized) {
        SPDLOG_ERROR("[RTX] TextureManager::GetOrCreateTextureInfo called before Initialize");
        return nullptr;
    }

    // Check cache first
    {
        std::lock_guard<std::mutex> lock(mCacheMutex);
        auto it = mTextureCache.find(hash);
        if (it != mTextureCache.end()) {
            return &it->second;
        }
    }

    // Validate input
    if (!tmem || width == 0 || height == 0) {
        SPDLOG_WARN("[RTX] TextureManager: invalid texture data (hash: 0x{:016X})", hash);
        return nullptr;
    }

    if (width > 4096 || height > 4096) {
        SPDLOG_WARN("[RTX] TextureManager: texture too large ({}x{}, hash: 0x{:016X})",
                     width, height, hash);
        return nullptr;
    }

    // Check capacity
    if (mNextDescriptorIndex >= mMaxTextures && mFreeSRVIndices.empty()) {
        SPDLOG_WARN("[RTX] TextureManager: SRV heap full ({} textures)", mMaxTextures);
        return nullptr;
    }

    // Decode N64 texture data to RGBA8888
    std::vector<uint8_t> rgba8 = ConvertN64Texture(tmem, fmt, siz, width, height, tlut, tlutFormat);

    if (rgba8.empty()) {
        SPDLOG_ERROR("[RTX] TextureManager: decode produced empty data (hash: 0x{:016X})", hash);
        return nullptr;
    }

    // Upload to GPU and create SRV
    RTXTextureInfo texture;
    texture.hash = hash;
    texture.width = width;
    texture.height = height;
    texture.srvIndex = 0;
    texture.format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texture.isBound = false;
    texture.uploaded = false;

    if (!CreateGPUTexture(rgba8.data(), width, height, texture)) {
        SPDLOG_ERROR("[RTX] TextureManager: GPU upload failed ({}x{}, hash: 0x{:016X})",
                     width, height, hash);
        return nullptr;
    }

    // Insert into cache
    {
        std::lock_guard<std::mutex> lock(mCacheMutex);
        auto result = mTextureCache.emplace(hash, std::move(texture));
        SPDLOG_DEBUG("[RTX] TextureManager: loaded texture {}x{} fmt={} siz={} at SRV index {} (hash: 0x{:016X})",
                     width, height, fmt, siz, result.first->second.srvIndex, hash);
        return &result.first->second;
    }
}

// ============================================================================
// GetOrCreateTextureInfo - convenience overload (auto-hash, packed format)
// ============================================================================

RTXTextureInfo* TextureManager::GetOrCreateTextureInfo(
    const uint8_t* data, uint32_t size,
    uint32_t width, uint32_t height,
    uint32_t format,
    const uint8_t* palette)
{
    if (!mInitialized) {
        SPDLOG_ERROR("[RTX] TextureManager::GetOrCreateTextureInfo (convenience) called before Initialize");
        return nullptr;
    }

    if (!data || size == 0 || width == 0 || height == 0) {
        SPDLOG_WARN("[RTX] TextureManager: invalid texture data in convenience overload");
        return nullptr;
    }

    // Extract format and size from the packed format field
    uint32_t imgFmt = (format >> 4) & 0x0F;
    uint32_t imgSiz = format & 0x0F;

    // Compute hash from raw data (include palette data in hash if present)
    uint64_t hash = HashTextureData(data, size);

    // If palette is present, mix it into the hash
    if (palette) {
        size_t paletteSize = 0;
        if (imgFmt == G_IM_FMT_CI) {
            paletteSize = (imgSiz == G_IM_SIZ_4b) ? 32 : 512;
        }
        if (paletteSize > 0) {
            uint64_t paletteHash = HashTextureData(palette, paletteSize);
            hash ^= paletteHash * 0x9e3779b97f4a7c15ULL;
        }
    }

    // If format uses alternative encoding, normalize it
    if (imgFmt > G_IM_FMT_I) {
        imgFmt = format / 4;
        imgSiz = format % 4;
    }

    // Delegate to the main overload
    return GetOrCreateTextureInfo(hash, data, imgFmt, imgSiz, width, height, palette, G_IM_SIZ_16b);
}

// ============================================================================
// GetOrCreateTextureLegacy - backward-compatible entry point (packed format)
// ============================================================================

RTXTexture* TextureManager::GetOrCreateTextureLegacy(
    uint64_t hash,
    const uint8_t* data,
    uint32_t width, uint32_t height,
    uint32_t format,
    const uint8_t* tlut,
    uint32_t tlutFormat)
{
    if (!mInitialized) {
        SPDLOG_ERROR("[RTX] TextureManager::GetOrCreateTextureLegacy called before Initialize");
        return nullptr;
    }

    // Check legacy cache first
    {
        std::lock_guard<std::mutex> lock(mCacheMutex);
        auto it = mCache.find(hash);
        if (it != mCache.end()) {
            return &it->second;
        }
    }

    // Validate input
    if (!data || width == 0 || height == 0) {
        SPDLOG_WARN("[RTX] TextureManager: invalid texture data (hash: 0x{:016X})", hash);
        return nullptr;
    }

    if (width > 4096 || height > 4096) {
        SPDLOG_WARN("[RTX] TextureManager: texture too large ({}x{}, hash: 0x{:016X})",
                     width, height, hash);
        return nullptr;
    }

    // Check capacity
    if (mNextDescriptorIndex >= mMaxTextures && mFreeSRVIndices.empty()) {
        SPDLOG_WARN("[RTX] TextureManager: SRV heap full ({} textures)", mMaxTextures);
        return nullptr;
    }

    // Extract format and size
    uint32_t imgFmt = (format >> 4) & 0x0F;
    uint32_t imgSiz = format & 0x0F;

    if (imgFmt > G_IM_FMT_I) {
        imgFmt = format / 4;
        imgSiz = format % 4;
    }

    // Decode using the unified converter
    std::vector<uint8_t> rgba8 = ConvertN64Texture(data, imgFmt, imgSiz, width, height, tlut, tlutFormat);

    if (rgba8.empty()) {
        SPDLOG_ERROR("[RTX] TextureManager: decode produced empty data (hash: 0x{:016X})", hash);
        return nullptr;
    }

    // Upload to GPU and create SRV (legacy path)
    RTXTexture texture;
    if (!CreateGPUTextureLegacy(rgba8.data(), width, height, texture)) {
        SPDLOG_ERROR("[RTX] TextureManager: GPU upload failed ({}x{}, hash: 0x{:016X})",
                     width, height, hash);
        return nullptr;
    }
    texture.n64Hash = hash;

    // Insert into legacy cache
    {
        std::lock_guard<std::mutex> lock(mCacheMutex);
        auto result = mCache.emplace(hash, std::move(texture));
        SPDLOG_DEBUG("[RTX] TextureManager: loaded legacy texture {}x{} at SRV index {} (hash: 0x{:016X})",
                     width, height, result.first->second.descriptorIndex, hash);
        return &result.first->second;
    }
}

// ============================================================================
// UploadTexture / GetOrUploadTexture - RTXTextureHandle convenience API
// ============================================================================

RTXTextureHandle TextureManager::UploadTexture(const uint8_t* rgbaData, uint32_t width, uint32_t height, uint64_t hash) {
    RTXTextureHandle handle = {};
    handle.srvIndex = 0;
    handle.textureIndex = 0;
    handle.width = 0;
    handle.height = 0;
    handle.gpuHandle.ptr = 0;

    if (!mInitialized) {
        SPDLOG_ERROR("[RTX] TextureManager::UploadTexture called before Initialize");
        return handle;
    }

    if (!rgbaData || width == 0 || height == 0) {
        SPDLOG_WARN("[RTX] TextureManager::UploadTexture: invalid parameters");
        return handle;
    }

    // Check LRU cache first
    {
        std::lock_guard<std::mutex> lock(mCacheMutex);
        auto it = mLRUCache.find(hash);
        if (it != mLRUCache.end()) {
            it->second.texture.lastUsedFrame = mCurrentFrame;
            mLRUList.erase(it->second.lruIter);
            mLRUList.push_front(hash);
            it->second.lruIter = mLRUList.begin();

            handle.srvIndex = it->second.texture.srvIndex;
            handle.textureIndex = it->second.texture.srvIndex;
            handle.width = it->second.texture.width;
            handle.height = it->second.texture.height;
            handle.gpuHandle = it->second.texture.srvGpuHandle;
            return handle;
        }

        // Also check legacy cache
        auto legIt = mTextureCache.find(hash);
        if (legIt != mTextureCache.end()) {
            handle.srvIndex = legIt->second.srvIndex;
            handle.textureIndex = legIt->second.srvIndex;
            handle.width = legIt->second.width;
            handle.height = legIt->second.height;
            handle.gpuHandle = legIt->second.srvGpuHandle;
            return handle;
        }
    }

    // Allocate SRV index (may evict LRU if full)
    uint32_t srvIndex;
    {
        std::lock_guard<std::mutex> lock(mCacheMutex);

        while (mLRUCache.size() >= mMaxCachedTextures) {
            uint32_t freedIdx = EvictLRUTexture();
            if (freedIdx != UINT32_MAX) {
                mFreeSRVIndices.push_back(freedIdx);
            } else {
                break;
            }
        }

        srvIndex = AllocateSRVIndex();
    }

    if (srvIndex == UINT32_MAX) {
        SPDLOG_ERROR("[RTX] TextureManager::UploadTexture: no SRV slots available");
        return handle;
    }

    // Create GPU texture resource and upload
    CachedTexture cachedTex;
    cachedTex.hashKey = hash;
    cachedTex.width = width;
    cachedTex.height = height;
    cachedTex.srvIndex = srvIndex;
    cachedTex.dxgiFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    cachedTex.lastUsedFrame = mCurrentFrame;
    cachedTex.uploaded = false;

    if (!CreateGPUTextureAtIndex(rgbaData, width, height, srvIndex, cachedTex)) {
        SPDLOG_ERROR("[RTX] TextureManager::UploadTexture: GPU upload failed ({}x{}, hash: 0x{:016X})",
                     width, height, hash);
        std::lock_guard<std::mutex> lock(mCacheMutex);
        mFreeSRVIndices.push_back(srvIndex);
        return handle;
    }

    // Populate the return handle
    handle.srvIndex = srvIndex;
    handle.textureIndex = srvIndex;
    handle.width = width;
    handle.height = height;
    handle.gpuHandle = cachedTex.srvGpuHandle;

    // Insert into LRU cache
    {
        std::lock_guard<std::mutex> lock(mCacheMutex);
        mLRUList.push_front(hash);
        LRUCacheEntry entry;
        entry.texture = std::move(cachedTex);
        entry.lruIter = mLRUList.begin();
        mLRUCache.emplace(hash, std::move(entry));
    }

    SPDLOG_DEBUG("[RTX] TextureManager::UploadTexture: uploaded {}x{} at SRV index {} (hash: 0x{:016X})",
                 width, height, handle.srvIndex, hash);
    return handle;
}

RTXTextureHandle TextureManager::GetOrUploadTexture(const uint8_t* rgbaData, uint32_t w, uint32_t h, uint64_t hash) {
    return UploadTexture(rgbaData, w, h, hash);
}

// ============================================================================
// UploadToGPU - upload RGBA data for an RTXTextureInfo
// ============================================================================

void TextureManager::UploadToGPU(RTXTextureInfo& tex, const uint8_t* rgbaData,
                                  ID3D12GraphicsCommandList* cmdList) {
    if (!mInitialized || !rgbaData || tex.width == 0 || tex.height == 0) {
        SPDLOG_ERROR("[RTX] TextureManager::UploadToGPU: invalid parameters");
        return;
    }

    if (tex.uploaded) {
        SPDLOG_WARN("[RTX] TextureManager::UploadToGPU: texture already uploaded (hash: 0x{:016X})", tex.hash);
        return;
    }

    if (!CreateGPUTexture(rgbaData, tex.width, tex.height, tex)) {
        SPDLOG_ERROR("[RTX] TextureManager::UploadToGPU failed ({}x{}, hash: 0x{:016X})",
                     tex.width, tex.height, tex.hash);
    }
}

// ============================================================================
// GetDefaultTexture - returns GPU descriptor handle for the 1x1 white texture
// ============================================================================

D3D12_GPU_DESCRIPTOR_HANDLE TextureManager::GetDefaultTexture() {
    if (!mInitialized || !mDefaultWhiteCreated) {
        D3D12_GPU_DESCRIPTOR_HANDLE nullHandle = {};
        nullHandle.ptr = 0;
        return nullHandle;
    }
    return mDefaultWhiteTexture.srvGpuHandle;
}

// ============================================================================
// GetDefaultWhiteTexture
// ============================================================================

RTXTextureInfo* TextureManager::GetDefaultWhiteTexture() {
    if (!mInitialized) {
        SPDLOG_ERROR("[RTX] TextureManager::GetDefaultWhiteTexture called before Initialize");
        return nullptr;
    }

    if (!mDefaultWhiteCreated) {
        CreateDefaultWhiteTexture();
    }

    return mDefaultWhiteCreated ? &mDefaultWhiteTexture : nullptr;
}

// ============================================================================
// GetFallbackCheckerboardHandle / GetFallbackCheckerboard
// ============================================================================

RTXTextureHandle TextureManager::GetFallbackCheckerboardHandle() const {
    RTXTextureHandle handle;
    if (mCheckerboardCreated) {
        handle.srvIndex = mCheckerboardFallback.srvIndex;
        handle.textureIndex = mCheckerboardFallback.srvIndex;
        handle.width = mCheckerboardFallback.width;
        handle.height = mCheckerboardFallback.height;
        handle.gpuHandle = mCheckerboardFallback.srvGpuHandle;
    } else {
        // If checkerboard not yet created, fall back to white at index 0
        handle.srvIndex = 0;
        handle.textureIndex = 0;
        handle.width = 1;
        handle.height = 1;
        handle.gpuHandle.ptr = 0;
        if (mDefaultWhiteCreated) {
            handle.gpuHandle = mDefaultWhiteTexture.srvGpuHandle;
        }
    }
    return handle;
}

CachedTexture* TextureManager::GetFallbackCheckerboard() {
    if (!mInitialized) return nullptr;
    if (!mCheckerboardCreated) {
        CreateCheckerboardFallbackTexture();
    }
    return mCheckerboardCreated ? &mCheckerboardFallback : nullptr;
}

// ============================================================================
// CreateDefaultWhiteTexture - 1x1 white RGBA texture at SRV index 0
// ============================================================================

void TextureManager::CreateDefaultWhiteTexture() {
    if (mDefaultWhiteCreated) return;

    // 1x1 white pixel: RGBA = (255, 255, 255, 255)
    uint8_t whitePixel[4] = { 255, 255, 255, 255 };

    mDefaultWhiteTexture.hash = 0;
    mDefaultWhiteTexture.width = 1;
    mDefaultWhiteTexture.height = 1;
    mDefaultWhiteTexture.srvIndex = 0;
    mDefaultWhiteTexture.format = DXGI_FORMAT_R8G8B8A8_UNORM;
    mDefaultWhiteTexture.isBound = false;
    mDefaultWhiteTexture.uploaded = false;

    if (CreateGPUTexture(whitePixel, 1, 1, mDefaultWhiteTexture)) {
        mDefaultWhiteCreated = true;
        SPDLOG_INFO("[RTX] TextureManager: default 1x1 white texture created at SRV index 0");
    } else {
        SPDLOG_ERROR("[RTX] TextureManager: failed to create default white texture");
    }
}

// ============================================================================
// CreateCheckerboardFallbackTexture - 8x8 magenta/black checkerboard at SRV index 1
// ============================================================================

void TextureManager::CreateCheckerboardFallbackTexture() {
    if (mCheckerboardCreated) return;

    constexpr uint32_t CHECKER_SIZE = 8;
    uint8_t checkerPixels[CHECKER_SIZE * CHECKER_SIZE * 4];

    for (uint32_t y = 0; y < CHECKER_SIZE; y++) {
        for (uint32_t x = 0; x < CHECKER_SIZE; x++) {
            uint32_t idx = (y * CHECKER_SIZE + x) * 4;
            // Alternate between magenta (255,0,255) and black (0,0,0) in a 2x2 pattern
            bool isMagenta = ((x / 2) + (y / 2)) % 2 == 0;
            checkerPixels[idx + 0] = isMagenta ? 255 : 0;   // R
            checkerPixels[idx + 1] = 0;                       // G
            checkerPixels[idx + 2] = isMagenta ? 255 : 0;   // B
            checkerPixels[idx + 3] = 255;                     // A
        }
    }

    // Allocate SRV index 1 for the checkerboard
    uint32_t srvIndex;
    {
        std::lock_guard<std::mutex> lock(mCacheMutex);
        srvIndex = mNextDescriptorIndex;
        // Ensure it gets index 1 (after white at 0)
        if (srvIndex < 1) {
            srvIndex = 1;
            if (mNextDescriptorIndex <= 1) {
                mNextDescriptorIndex = 1;
            }
        }
    }

    mCheckerboardFallback.hashKey = 0xDEADBEEFDEADBEEFULL;
    mCheckerboardFallback.width = CHECKER_SIZE;
    mCheckerboardFallback.height = CHECKER_SIZE;
    mCheckerboardFallback.srvIndex = srvIndex;
    mCheckerboardFallback.dxgiFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    mCheckerboardFallback.lastUsedFrame = 0;
    mCheckerboardFallback.uploaded = false;

    if (CreateGPUTextureAtIndex(checkerPixels, CHECKER_SIZE, CHECKER_SIZE, srvIndex, mCheckerboardFallback)) {
        mCheckerboardCreated = true;
        // Advance past the reserved index
        {
            std::lock_guard<std::mutex> lock(mCacheMutex);
            if (mNextDescriptorIndex <= srvIndex) {
                mNextDescriptorIndex = srvIndex + 1;
            }
        }
        SPDLOG_INFO("[RTX] TextureManager: 8x8 magenta/black checkerboard fallback texture created at SRV index {}", srvIndex);
    } else {
        SPDLOG_ERROR("[RTX] TextureManager: failed to create checkerboard fallback texture");
    }
}

// ============================================================================
// CreateGPUTextureAtIndex - create ID3D12Resource, upload data, create SRV
// at a specific SRV index. Used by the LRU cache path.
// ============================================================================

bool TextureManager::CreateGPUTextureAtIndex(const uint8_t* rgbaData, uint32_t w, uint32_t h,
                                              uint32_t srvIndex, CachedTexture& outTexture) {
    if (!rgbaData || w == 0 || h == 0) {
        return false;
    }

    ID3D12Device* device = mRawDevice;
    if (!device) {
        return false;
    }

    // Step 1: Create the destination TEXTURE2D on the default heap
    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Alignment = 0;
    texDesc.Width = w;
    texDesc.Height = h;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.SampleDesc.Quality = 0;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    D3D12_HEAP_PROPERTIES defaultHeap = {};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

    ComPtr<ID3D12Resource> textureResource;
    HRESULT hr = device->CreateCommittedResource(
        &defaultHeap,
        D3D12_HEAP_FLAG_NONE,
        &texDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(&textureResource));
    if (FAILED(hr)) {
        SPDLOG_ERROR("[RTX] TextureManager: CreateCommittedResource failed ({}x{}): 0x{:08X}",
                     w, h, static_cast<uint32_t>(hr));
        return false;
    }

    // Step 2: Query upload layout and create staging buffer
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT numRows = 0;
    UINT64 rowSizeInBytes = 0;
    UINT64 totalBytes = 0;

    device->GetCopyableFootprints(&texDesc, 0, 1, 0,
                                   &footprint, &numRows, &rowSizeInBytes, &totalBytes);

    ComPtr<ID3D12Resource> stagingBuffer = CreateUploadBuffer(totalBytes);
    if (!stagingBuffer) {
        SPDLOG_ERROR("[RTX] TextureManager: staging buffer creation failed ({} bytes)", totalBytes);
        return false;
    }

    // Step 3: Map staging buffer and copy pixel data row-by-row
    void* mapped = nullptr;
    hr = stagingBuffer->Map(0, nullptr, &mapped);
    if (FAILED(hr)) {
        SPDLOG_ERROR("[RTX] TextureManager: Map staging buffer failed: 0x{:08X}",
                     static_cast<uint32_t>(hr));
        return false;
    }

    const uint32_t srcRowPitch = w * 4; // RGBA8 = 4 bytes per pixel
    const uint32_t dstRowPitch = footprint.Footprint.RowPitch;
    uint8_t* dstBase = static_cast<uint8_t*>(mapped) + footprint.Offset;

    for (uint32_t row = 0; row < h; row++) {
        const uint8_t* srcRow = rgbaData + row * srcRowPitch;
        uint8_t* dstRow = dstBase + row * dstRowPitch;
        memcpy(dstRow, srcRow, srcRowPitch);
    }

    stagingBuffer->Unmap(0, nullptr);

    // Step 4: Record copy command and resource barrier
    hr = mUploadCmdAllocator->Reset();
    if (FAILED(hr)) {
        SPDLOG_ERROR("[RTX] TextureManager: Reset command allocator failed: 0x{:08X}",
                     static_cast<uint32_t>(hr));
        return false;
    }

    hr = mUploadCmdList->Reset(mUploadCmdAllocator.Get(), nullptr);
    if (FAILED(hr)) {
        SPDLOG_ERROR("[RTX] TextureManager: Reset command list failed: 0x{:08X}",
                     static_cast<uint32_t>(hr));
        return false;
    }

    D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
    dstLoc.pResource = textureResource.Get();
    dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dstLoc.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
    srcLoc.pResource = stagingBuffer.Get();
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcLoc.PlacedFootprint = footprint;

    mUploadCmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

    // Transition from COPY_DEST to shader resource
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = textureResource.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    mUploadCmdList->ResourceBarrier(1, &barrier);

    // Execute and wait for GPU
    ExecuteUploadAndWait();

    // Step 5: Create SRV descriptor
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.PlaneSlice = 0;
    srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = GetCPUHandle(srvIndex);
    device->CreateShaderResourceView(textureResource.Get(), &srvDesc, cpuHandle);

    // Populate output texture struct
    outTexture.gpuTexture = textureResource;
    outTexture.srvGpuHandle = GetGPUHandle(srvIndex);
    outTexture.srvCpuHandle = cpuHandle;
    outTexture.srvIndex = srvIndex;
    outTexture.width = w;
    outTexture.height = h;
    outTexture.dxgiFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    outTexture.uploaded = true;

    // Track resource for lifetime management
    {
        std::lock_guard<std::mutex> lock(mCacheMutex);
        mTextureResources.push_back(std::move(textureResource));
    }

    return true;
}

// ============================================================================
// CreateGPUTexture - create ID3D12Resource, upload data, create SRV (RTXTextureInfo)
// Allocates next SRV descriptor index automatically.
// ============================================================================

bool TextureManager::CreateGPUTexture(const uint8_t* rgbaData, uint32_t w, uint32_t h,
                                       RTXTextureInfo& outTexture) {
    if (!rgbaData || w == 0 || h == 0) {
        return false;
    }

    ID3D12Device* device = mRawDevice;
    if (!device) {
        return false;
    }

    // Determine the SRV index to use (under lock)
    uint32_t srvIndex;
    {
        std::lock_guard<std::mutex> lock(mCacheMutex);
        if (mNextDescriptorIndex >= mMaxTextures) {
            return false;
        }
        srvIndex = mNextDescriptorIndex;
    }

    // Step 1: Create the destination TEXTURE2D on the default heap
    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Alignment = 0;
    texDesc.Width = w;
    texDesc.Height = h;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.SampleDesc.Quality = 0;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    D3D12_HEAP_PROPERTIES defaultHeap = {};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

    ComPtr<ID3D12Resource> textureResource;
    HRESULT hr = device->CreateCommittedResource(
        &defaultHeap,
        D3D12_HEAP_FLAG_NONE,
        &texDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(&textureResource));
    if (FAILED(hr)) {
        SPDLOG_ERROR("[RTX] TextureManager: CreateCommittedResource failed ({}x{}): 0x{:08X}",
                     w, h, static_cast<uint32_t>(hr));
        return false;
    }

    // Step 2: Query upload layout and create staging buffer
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT numRows = 0;
    UINT64 rowSizeInBytes = 0;
    UINT64 totalBytes = 0;

    device->GetCopyableFootprints(&texDesc, 0, 1, 0,
                                   &footprint, &numRows, &rowSizeInBytes, &totalBytes);

    ComPtr<ID3D12Resource> stagingBuffer = CreateUploadBuffer(totalBytes);
    if (!stagingBuffer) {
        SPDLOG_ERROR("[RTX] TextureManager: staging buffer creation failed ({} bytes)", totalBytes);
        return false;
    }

    // Step 3: Map staging buffer and copy pixel data row-by-row
    void* mapped = nullptr;
    hr = stagingBuffer->Map(0, nullptr, &mapped);
    if (FAILED(hr)) {
        SPDLOG_ERROR("[RTX] TextureManager: Map staging buffer failed: 0x{:08X}",
                     static_cast<uint32_t>(hr));
        return false;
    }

    const uint32_t srcRowPitch = w * 4;
    const uint32_t dstRowPitch = footprint.Footprint.RowPitch;
    uint8_t* dstBase = static_cast<uint8_t*>(mapped) + footprint.Offset;

    for (uint32_t row = 0; row < h; row++) {
        const uint8_t* srcRow = rgbaData + row * srcRowPitch;
        uint8_t* dstRow = dstBase + row * dstRowPitch;
        memcpy(dstRow, srcRow, srcRowPitch);
    }

    stagingBuffer->Unmap(0, nullptr);

    // Step 4: Record copy command and resource barrier
    hr = mUploadCmdAllocator->Reset();
    if (FAILED(hr)) {
        SPDLOG_ERROR("[RTX] TextureManager: Reset command allocator failed: 0x{:08X}",
                     static_cast<uint32_t>(hr));
        return false;
    }

    hr = mUploadCmdList->Reset(mUploadCmdAllocator.Get(), nullptr);
    if (FAILED(hr)) {
        SPDLOG_ERROR("[RTX] TextureManager: Reset command list failed: 0x{:08X}",
                     static_cast<uint32_t>(hr));
        return false;
    }

    D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
    dstLoc.pResource = textureResource.Get();
    dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dstLoc.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
    srcLoc.pResource = stagingBuffer.Get();
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcLoc.PlacedFootprint = footprint;

    mUploadCmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

    // Transition from COPY_DEST to shader resource
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = textureResource.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    mUploadCmdList->ResourceBarrier(1, &barrier);

    // Execute and wait for GPU
    ExecuteUploadAndWait();

    // Step 5: Create SRV descriptor
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.PlaneSlice = 0;
    srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = GetCPUHandle(srvIndex);
    device->CreateShaderResourceView(textureResource.Get(), &srvDesc, cpuHandle);

    // Populate output texture struct
    outTexture.gpuTexture = textureResource;
    outTexture.srvGpuHandle = GetGPUHandle(srvIndex);
    outTexture.srvIndex = srvIndex;
    outTexture.width = w;
    outTexture.height = h;
    outTexture.format = DXGI_FORMAT_R8G8B8A8_UNORM;
    outTexture.isBound = false;
    outTexture.uploaded = true;

    // Advance SRV index and track resource (under lock)
    {
        std::lock_guard<std::mutex> lock(mCacheMutex);
        mTextureResources.push_back(std::move(textureResource));
        mNextDescriptorIndex++;
    }

    return true;
}

// ============================================================================
// CreateGPUTextureLegacy - same as CreateGPUTexture but for RTXTexture struct
// ============================================================================

bool TextureManager::CreateGPUTextureLegacy(const uint8_t* rgbaData, uint32_t w, uint32_t h,
                                             RTXTexture& outTexture) {
    RTXTextureInfo info;
    info.hash = 0;
    info.width = w;
    info.height = h;
    info.srvIndex = 0;
    info.format = DXGI_FORMAT_R8G8B8A8_UNORM;
    info.isBound = false;
    info.uploaded = false;

    if (!CreateGPUTexture(rgbaData, w, h, info)) {
        return false;
    }

    outTexture.resource = std::move(info.gpuTexture);
    outTexture.descriptorIndex = info.srvIndex;
    outTexture.srvCPU = GetCPUHandle(info.srvIndex);
    outTexture.srvHandle = info.srvGpuHandle;
    outTexture.width = info.width;
    outTexture.height = info.height;
    outTexture.format = info.format;
    outTexture.n64Hash = info.hash;
    return true;
}

// ============================================================================
// Helper: create an upload (staging) buffer
// ============================================================================

ComPtr<ID3D12Resource> TextureManager::CreateUploadBuffer(size_t size) {
    if (size == 0 || !mRawDevice) {
        return nullptr;
    }

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC bufferDesc = {};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Alignment = 0;
    bufferDesc.Width = size;
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.SampleDesc.Quality = 0;
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bufferDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    ComPtr<ID3D12Resource> buffer;
    HRESULT hr = mRawDevice->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &bufferDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&buffer));
    if (FAILED(hr)) {
        SPDLOG_ERROR("[RTX] TextureManager::CreateUploadBuffer failed (size: {}): 0x{:08X}",
                     size, static_cast<uint32_t>(hr));
        return nullptr;
    }
    return buffer;
}

// ============================================================================
// UploadTextureData — Task-spec Phase 6: upload decoded RGBA8 data to a
// D3D12 texture resource via staging buffer and copy command.
// The destination resource must be in D3D12_RESOURCE_STATE_COPY_DEST.
// Transitions the resource to PIXEL_SHADER_RESOURCE | NON_PIXEL_SHADER_RESOURCE.
// ============================================================================

bool TextureManager::UploadTextureData(ID3D12Resource* destResource, const uint8_t* rgbaData,
                                        uint32_t width, uint32_t height) {
    if (!destResource || !rgbaData || width == 0 || height == 0) {
        SPDLOG_ERROR("[RTX] TextureManager::UploadTextureData: invalid parameters");
        return false;
    }

    ID3D12Device* device = mRawDevice;
    if (!device) {
        SPDLOG_ERROR("[RTX] TextureManager::UploadTextureData: no device available");
        return false;
    }

    // Query the upload layout for the destination texture
    D3D12_RESOURCE_DESC texDesc = destResource->GetDesc();

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT numRows = 0;
    UINT64 rowSizeInBytes = 0;
    UINT64 totalBytes = 0;

    device->GetCopyableFootprints(&texDesc, 0, 1, 0,
                                   &footprint, &numRows, &rowSizeInBytes, &totalBytes);

    // Try to use the ring upload buffer if it has enough space
    ComPtr<ID3D12Resource> perTextureStagingBuffer;
    ID3D12Resource* stagingBuffer = nullptr;
    UINT64 stagingOffset = 0;

    if (mUploadBuffer && mUploadBufferSize > 0) {
        // Align offset to D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT (512)
        size_t alignedOffset = (mUploadBufferOffset + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1)
                               & ~(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
        if (alignedOffset + totalBytes <= mUploadBufferSize) {
            stagingBuffer = mUploadBuffer.Get();
            stagingOffset = static_cast<UINT64>(alignedOffset);
            mUploadBufferOffset = alignedOffset + static_cast<size_t>(totalBytes);
        } else {
            // Ring buffer exhausted, reset and try again
            mUploadBufferOffset = 0;
            size_t resetAligned = 0; // Already aligned at 0
            if (totalBytes <= mUploadBufferSize) {
                stagingBuffer = mUploadBuffer.Get();
                stagingOffset = 0;
                mUploadBufferOffset = static_cast<size_t>(totalBytes);
            }
        }
    }

    // Fallback: allocate a per-texture upload buffer
    if (!stagingBuffer) {
        perTextureStagingBuffer = CreateUploadBuffer(totalBytes);
        if (!perTextureStagingBuffer) {
            SPDLOG_ERROR("[RTX] TextureManager::UploadTextureData: staging buffer creation failed ({} bytes)",
                         totalBytes);
            return false;
        }
        stagingBuffer = perTextureStagingBuffer.Get();
        stagingOffset = 0;
    }

    // Map staging buffer and copy pixel data row-by-row
    void* mapped = nullptr;
    HRESULT hr = stagingBuffer->Map(0, nullptr, &mapped);
    if (FAILED(hr)) {
        SPDLOG_ERROR("[RTX] TextureManager::UploadTextureData: Map staging buffer failed: 0x{:08X}",
                     static_cast<uint32_t>(hr));
        return false;
    }

    const uint32_t srcRowPitch = width * 4; // RGBA8 = 4 bytes per pixel
    const uint32_t dstRowPitch = footprint.Footprint.RowPitch;
    uint8_t* dstBase = static_cast<uint8_t*>(mapped) + stagingOffset + footprint.Offset;

    for (uint32_t row = 0; row < height; row++) {
        const uint8_t* srcRow = rgbaData + row * srcRowPitch;
        uint8_t* dstRow = dstBase + row * dstRowPitch;
        memcpy(dstRow, srcRow, srcRowPitch);
    }

    stagingBuffer->Unmap(0, nullptr);

    // Record copy command and resource barrier
    hr = mUploadCmdAllocator->Reset();
    if (FAILED(hr)) {
        SPDLOG_ERROR("[RTX] TextureManager::UploadTextureData: Reset command allocator failed: 0x{:08X}",
                     static_cast<uint32_t>(hr));
        return false;
    }

    hr = mUploadCmdList->Reset(mUploadCmdAllocator.Get(), nullptr);
    if (FAILED(hr)) {
        SPDLOG_ERROR("[RTX] TextureManager::UploadTextureData: Reset command list failed: 0x{:08X}",
                     static_cast<uint32_t>(hr));
        return false;
    }

    D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
    dstLoc.pResource = destResource;
    dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dstLoc.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
    srcLoc.pResource = stagingBuffer;
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcLoc.PlacedFootprint = footprint;
    srcLoc.PlacedFootprint.Offset = stagingOffset;

    mUploadCmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

    // Transition from COPY_DEST to shader resource
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = destResource;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    mUploadCmdList->ResourceBarrier(1, &barrier);

    // Execute and wait for GPU
    ExecuteUploadAndWait();

    return true;
}

// ============================================================================
// Helper: execute upload command list and wait
// ============================================================================

void TextureManager::ExecuteUploadAndWait() {
    HRESULT hr = mUploadCmdList->Close();
    if (FAILED(hr)) {
        SPDLOG_ERROR("[RTX] TextureManager::ExecuteUploadAndWait: Close failed: 0x{:08X}",
                     static_cast<uint32_t>(hr));
        return;
    }

    ID3D12CommandList* ppCmdLists[] = { mUploadCmdList.Get() };

    if (mContext) {
        // DX12Device path: use the context's command queue and wait
        mContext->GetCommandQueue()->ExecuteCommandLists(1, ppCmdLists);
        mContext->WaitForGPU();
    } else if (mOwnedCommandQueue) {
        // Explicit device path: use our own command queue and fence
        mOwnedCommandQueue->ExecuteCommandLists(1, ppCmdLists);

        mUploadFenceValue++;
        hr = mOwnedCommandQueue->Signal(mUploadFence.Get(), mUploadFenceValue);
        if (FAILED(hr)) {
            SPDLOG_ERROR("[RTX] TextureManager::ExecuteUploadAndWait: Signal failed: 0x{:08X}",
                         static_cast<uint32_t>(hr));
            return;
        }

        if (mUploadFence->GetCompletedValue() < mUploadFenceValue) {
            hr = mUploadFence->SetEventOnCompletion(mUploadFenceValue, mUploadFenceEvent);
            if (FAILED(hr)) {
                SPDLOG_ERROR("[RTX] TextureManager::ExecuteUploadAndWait: SetEventOnCompletion failed: 0x{:08X}",
                             static_cast<uint32_t>(hr));
                return;
            }
            WaitForSingleObject(mUploadFenceEvent, INFINITE);
        }
    } else {
        SPDLOG_ERROR("[RTX] TextureManager::ExecuteUploadAndWait: no command queue available");
    }
}

// ============================================================================
// N64 texture format decoders
// ============================================================================

// Helper: expand an RGBA16 (5-5-5-1) big-endian 16-bit value to RGBA8
static inline void ExpandRGBA16(uint16_t pixel, uint8_t* out) {
    uint8_t r5 = (pixel >> 11) & 0x1F;
    uint8_t g5 = (pixel >> 6)  & 0x1F;
    uint8_t b5 = (pixel >> 1)  & 0x1F;
    uint8_t a1 = pixel & 0x01;

    // Expand 5-bit to 8-bit: (val << 3) | (val >> 2)
    out[0] = static_cast<uint8_t>((r5 << 3) | (r5 >> 2));
    out[1] = static_cast<uint8_t>((g5 << 3) | (g5 >> 2));
    out[2] = static_cast<uint8_t>((b5 << 3) | (b5 >> 2));
    out[3] = a1 ? 255 : 0;
}

// Helper: expand an IA16 (8-bit I, 8-bit A) big-endian 16-bit palette entry to RGBA8
static inline void ExpandIA16Palette(uint16_t pixel, uint8_t* out) {
    uint8_t intensity = (pixel >> 8) & 0xFF;
    uint8_t alpha     = pixel & 0xFF;
    out[0] = intensity;
    out[1] = intensity;
    out[2] = intensity;
    out[3] = alpha;
}

// Helper: expand a TLUT entry to RGBA8, dispatching by TLUT format
static inline void ExpandTLUTEntry(uint16_t paletteEntry, uint32_t tlutFormat, uint8_t* out) {
    if (tlutFormat == RTX::G_IM_SIZ_16b) {
        // RGBA16 palette (most common)
        ExpandRGBA16(paletteEntry, out);
    } else {
        // IA16 palette
        ExpandIA16Palette(paletteEntry, out);
    }
}

// Helper: read a big-endian 16-bit value from a byte pointer
static inline uint16_t ReadBE16(const uint8_t* p) {
    return (static_cast<uint16_t>(p[0]) << 8) | p[1];
}

// RGBA16: 5-5-5-1, 2 bytes per pixel, big-endian
std::vector<uint8_t> TextureManager::DecodeRGBA16(const uint8_t* src, uint32_t width, uint32_t height) {
    const uint32_t pixelCount = width * height;
    std::vector<uint8_t> out(pixelCount * 4);

    for (uint32_t i = 0; i < pixelCount; i++) {
        uint16_t pixel = ReadBE16(&src[i * 2]);
        ExpandRGBA16(pixel, &out[i * 4]);
    }
    return out;
}

// RGBA32: 8-8-8-8, 4 bytes per pixel (R,G,B,A byte order)
std::vector<uint8_t> TextureManager::DecodeRGBA32(const uint8_t* src, uint32_t width, uint32_t height) {
    const size_t byteCount = static_cast<size_t>(width) * height * 4;
    std::vector<uint8_t> out(byteCount);
    memcpy(out.data(), src, byteCount);
    return out;
}

// IA4: 4 bits per pixel (3-bit I, 1-bit A), 2 pixels per byte
std::vector<uint8_t> TextureManager::DecodeIA4(const uint8_t* src, uint32_t width, uint32_t height) {
    const uint32_t pixelCount = width * height;
    std::vector<uint8_t> out(pixelCount * 4);

    for (uint32_t i = 0; i < pixelCount; i++) {
        uint8_t byteVal = src[i / 2];
        uint8_t nibble;
        if ((i & 1) == 0) {
            nibble = (byteVal >> 4) & 0x0F;
        } else {
            nibble = byteVal & 0x0F;
        }

        // Upper 3 bits = intensity, lower 1 bit = alpha
        uint8_t i3 = (nibble >> 1) & 0x07;
        uint8_t a1 = nibble & 0x01;

        // Expand 3-bit to 8-bit by replicating bits: (i3<<5)|(i3<<2)|(i3>>1)
        uint8_t intensity = static_cast<uint8_t>((i3 << 5) | (i3 << 2) | (i3 >> 1));

        out[i * 4 + 0] = intensity;
        out[i * 4 + 1] = intensity;
        out[i * 4 + 2] = intensity;
        out[i * 4 + 3] = a1 ? 255 : 0;
    }
    return out;
}

// IA8: 8 bits per pixel (4-bit I, 4-bit A)
std::vector<uint8_t> TextureManager::DecodeIA8(const uint8_t* src, uint32_t width, uint32_t height) {
    const uint32_t pixelCount = width * height;
    std::vector<uint8_t> out(pixelCount * 4);

    for (uint32_t i = 0; i < pixelCount; i++) {
        uint8_t byteVal = src[i];

        // Upper nibble = intensity, lower nibble = alpha
        uint8_t i4 = (byteVal >> 4) & 0x0F;
        uint8_t a4 = byteVal & 0x0F;

        // Expand 4-bit to 8-bit: (val << 4) | val
        uint8_t intensity = static_cast<uint8_t>((i4 << 4) | i4);
        uint8_t alpha     = static_cast<uint8_t>((a4 << 4) | a4);

        out[i * 4 + 0] = intensity;
        out[i * 4 + 1] = intensity;
        out[i * 4 + 2] = intensity;
        out[i * 4 + 3] = alpha;
    }
    return out;
}

// IA16: 16 bits per pixel (8-bit I, 8-bit A), big-endian
std::vector<uint8_t> TextureManager::DecodeIA16(const uint8_t* src, uint32_t width, uint32_t height) {
    const uint32_t pixelCount = width * height;
    std::vector<uint8_t> out(pixelCount * 4);

    for (uint32_t i = 0; i < pixelCount; i++) {
        // Big-endian: first byte = intensity, second byte = alpha
        uint8_t intensity = src[i * 2];
        uint8_t alpha     = src[i * 2 + 1];

        out[i * 4 + 0] = intensity;
        out[i * 4 + 1] = intensity;
        out[i * 4 + 2] = intensity;
        out[i * 4 + 3] = alpha;
    }
    return out;
}

// CI4: 4-bit color index (2 pixels per byte), TLUT is RGBA16 or IA16 palette
std::vector<uint8_t> TextureManager::DecodeCI4(const uint8_t* src, uint32_t width, uint32_t height,
                                                const uint8_t* tlut, uint32_t tlutFormat) {
    const uint32_t pixelCount = width * height;
    std::vector<uint8_t> out(pixelCount * 4);

    for (uint32_t i = 0; i < pixelCount; i++) {
        uint8_t byteVal = src[i / 2];
        uint8_t index;
        if ((i & 1) == 0) {
            index = (byteVal >> 4) & 0x0F;
        } else {
            index = byteVal & 0x0F;
        }

        // Clamp to 4-bit range (max 16 palette entries)
        if (index > 15) {
            index = 0;
        }

        // TLUT entries are 2 bytes each, big-endian; format depends on tlutFormat
        uint16_t paletteEntry = ReadBE16(&tlut[index * 2]);
        ExpandTLUTEntry(paletteEntry, tlutFormat, &out[i * 4]);
    }
    return out;
}

// CI8: 8-bit color index (1 pixel per byte), TLUT is RGBA16 or IA16 palette
std::vector<uint8_t> TextureManager::DecodeCI8(const uint8_t* src, uint32_t width, uint32_t height,
                                                const uint8_t* tlut, uint32_t tlutFormat) {
    const uint32_t pixelCount = width * height;
    std::vector<uint8_t> out(pixelCount * 4);

    for (uint32_t i = 0; i < pixelCount; i++) {
        uint8_t index = src[i];

        // TLUT entries are 2 bytes each, big-endian; format depends on tlutFormat
        // CI8 supports up to 256 entries
        uint16_t paletteEntry = ReadBE16(&tlut[index * 2]);
        ExpandTLUTEntry(paletteEntry, tlutFormat, &out[i * 4]);
    }
    return out;
}

// I4: 4-bit intensity (2 pixels per byte), intensity replicated to RGB, alpha=255
std::vector<uint8_t> TextureManager::DecodeI4(const uint8_t* src, uint32_t width, uint32_t height) {
    const uint32_t pixelCount = width * height;
    std::vector<uint8_t> out(pixelCount * 4);

    for (uint32_t i = 0; i < pixelCount; i++) {
        uint8_t byteVal = src[i / 2];
        uint8_t i4;
        if ((i & 1) == 0) {
            i4 = (byteVal >> 4) & 0x0F;
        } else {
            i4 = byteVal & 0x0F;
        }

        // Expand 4-bit to 8-bit: (val << 4) | val
        uint8_t intensity = static_cast<uint8_t>((i4 << 4) | i4);

        // I format: replicate intensity to RGB, alpha=255
        out[i * 4 + 0] = intensity;
        out[i * 4 + 1] = intensity;
        out[i * 4 + 2] = intensity;
        out[i * 4 + 3] = 255;
    }
    return out;
}

// I8: 8-bit intensity (1 pixel per byte), intensity replicated to RGB, alpha=255
std::vector<uint8_t> TextureManager::DecodeI8(const uint8_t* src, uint32_t width, uint32_t height) {
    const uint32_t pixelCount = width * height;
    std::vector<uint8_t> out(pixelCount * 4);

    for (uint32_t i = 0; i < pixelCount; i++) {
        uint8_t intensity = src[i];

        // I format: replicate intensity to RGB, alpha=255
        out[i * 4 + 0] = intensity;
        out[i * 4 + 1] = intensity;
        out[i * 4 + 2] = intensity;
        out[i * 4 + 3] = 255;
    }
    return out;
}

// ============================================================================
// GetOrCreateTexture (task-spec primary entry: data, width, height, format, palette)
// Hashes input data + format + dimensions, returns cached or creates new RTXTexture*.
// ============================================================================

RTXTexture* TextureManager::GetOrCreateTexture(
    const uint8_t* data, uint32_t width, uint32_t height,
    uint32_t format, const uint8_t* palette)
{
    if (!mInitialized) {
        SPDLOG_ERROR("[RTX] TextureManager::GetOrCreateTexture(data,...) called before Initialize");
        return nullptr;
    }

    if (!data || width == 0 || height == 0) {
        SPDLOG_WARN("[RTX] TextureManager::GetOrCreateTexture: invalid texture data");
        return nullptr;
    }

    if (width > 4096 || height > 4096) {
        SPDLOG_WARN("[RTX] TextureManager::GetOrCreateTexture: texture too large ({}x{})",
                     width, height);
        return nullptr;
    }

    // Extract N64 format and size from the packed format field
    uint32_t imgFmt = (format >> 4) & 0x0F;
    uint32_t imgSiz = format & 0x0F;

    // Handle alternative format encoding (if packed value exceeds valid range)
    if (imgFmt > G_IM_FMT_I) {
        imgFmt = format / 4;
        imgSiz = format % 4;
    }

    // Compute data size in bytes for hashing
    size_t dataSize = 0;
    switch (imgSiz) {
        case G_IM_SIZ_4b:  dataSize = (static_cast<size_t>(width) * height + 1) / 2; break;
        case G_IM_SIZ_8b:  dataSize = static_cast<size_t>(width) * height; break;
        case G_IM_SIZ_16b: dataSize = static_cast<size_t>(width) * height * 2; break;
        case G_IM_SIZ_32b: dataSize = static_cast<size_t>(width) * height * 4; break;
        default:           dataSize = static_cast<size_t>(width) * height; break;
    }

    // Compute hash using FNV-1a over raw texture data + format + dimensions
    uint64_t hash = HashTextureData(data, dataSize);

    constexpr uint64_t FNV_PRIME = 0x100000001b3ULL;
    hash ^= static_cast<uint64_t>(format);
    hash *= FNV_PRIME;
    hash ^= static_cast<uint64_t>(width);
    hash *= FNV_PRIME;
    hash ^= static_cast<uint64_t>(height);
    hash *= FNV_PRIME;

    // Mix in palette data if present (CI formats)
    if (palette && imgFmt == G_IM_FMT_CI) {
        size_t paletteBytes = (imgSiz == G_IM_SIZ_4b) ? 32 : 512; // 16 or 256 entries * 2 bytes
        uint64_t palHash = HashTextureData(palette, paletteBytes);
        hash ^= palHash;
        hash *= FNV_PRIME;
    }

    // Check cache first
    {
        std::lock_guard<std::mutex> lock(mCacheMutex);
        auto it = mCache.find(hash);
        if (it != mCache.end()) {
            SPDLOG_DEBUG("[RTX] TextureManager: cache hit for texture {}x{} (hash: 0x{:016X})",
                         width, height, hash);
            return &it->second;
        }
    }

    // Check capacity
    if (mNextDescriptorIndex >= mMaxTextures && mFreeSRVIndices.empty()) {
        SPDLOG_WARN("[RTX] TextureManager::GetOrCreateTexture: SRV heap full ({} textures)", mMaxTextures);
        return nullptr;
    }

    // Decode N64 texels into RGBA8
    std::vector<uint8_t> rgba8 = ConvertN64Texture(data, imgFmt, imgSiz, width, height, palette, G_IM_SIZ_16b);

    if (rgba8.empty()) {
        SPDLOG_ERROR("[RTX] TextureManager::GetOrCreateTexture: decode produced empty data (hash: 0x{:016X})", hash);
        return nullptr;
    }

    // Upload to GPU and create SRV (legacy path)
    RTXTexture texture;
    if (!CreateGPUTextureLegacy(rgba8.data(), width, height, texture)) {
        SPDLOG_ERROR("[RTX] TextureManager::GetOrCreateTexture: GPU upload failed ({}x{}, hash: 0x{:016X})",
                     width, height, hash);
        return nullptr;
    }
    texture.n64Hash = hash;

    // Insert into cache
    {
        std::lock_guard<std::mutex> lock(mCacheMutex);
        auto result = mCache.emplace(hash, std::move(texture));
        SPDLOG_INFO("[RTX] TextureManager: created texture {}x{} fmt=0x{:02X} at SRV index {} (hash: 0x{:016X})",
                     width, height, format, result.first->second.descriptorIndex, hash);
        return &result.first->second;
    }
}

// ============================================================================
// GetOrCreateTexture (overload with pre-computed hash: hash, texData, width, height, format)
// Returns RTXTexture*.
// ============================================================================

RTXTexture* TextureManager::GetOrCreateTexture(
    uint64_t hash, const uint8_t* texData,
    uint32_t width, uint32_t height,
    uint32_t format)
{
    if (!mInitialized) {
        SPDLOG_ERROR("[RTX] TextureManager::GetOrCreateTexture(hash,texData,...) called before Initialize");
        return nullptr;
    }

    if (!texData || width == 0 || height == 0) {
        SPDLOG_WARN("[RTX] TextureManager::GetOrCreateTexture: invalid texture data (hash: 0x{:016X})", hash);
        return nullptr;
    }

    // Check cache first
    {
        std::lock_guard<std::mutex> lock(mCacheMutex);
        auto it = mCache.find(hash);
        if (it != mCache.end()) {
            return &it->second;
        }
    }

    if (width > 4096 || height > 4096) {
        SPDLOG_WARN("[RTX] TextureManager::GetOrCreateTexture: texture too large ({}x{}, hash: 0x{:016X})",
                     width, height, hash);
        return nullptr;
    }

    // Check capacity
    if (mNextDescriptorIndex >= mMaxTextures && mFreeSRVIndices.empty()) {
        SPDLOG_WARN("[RTX] TextureManager::GetOrCreateTexture: SRV heap full ({} textures)", mMaxTextures);
        return nullptr;
    }

    // Extract N64 format and size from the packed format field
    uint32_t imgFmt = (format >> 4) & 0x0F;
    uint32_t imgSiz = format & 0x0F;

    // Handle alternative format encoding (if packed value exceeds valid range)
    if (imgFmt > G_IM_FMT_I) {
        imgFmt = format / 4;
        imgSiz = format % 4;
    }

    // Decode N64 texels into RGBA8
    std::vector<uint8_t> rgba8 = ConvertN64Texture(texData, imgFmt, imgSiz, width, height, nullptr, G_IM_SIZ_16b);

    if (rgba8.empty()) {
        SPDLOG_ERROR("[RTX] TextureManager::GetOrCreateTexture: decode produced empty data (hash: 0x{:016X})", hash);
        return nullptr;
    }

    // Upload to GPU and create SRV (legacy path)
    RTXTexture texture;
    if (!CreateGPUTextureLegacy(rgba8.data(), width, height, texture)) {
        SPDLOG_ERROR("[RTX] TextureManager::GetOrCreateTexture: GPU upload failed ({}x{}, hash: 0x{:016X})",
                     width, height, hash);
        return nullptr;
    }
    texture.n64Hash = hash;

    // Insert into cache
    {
        std::lock_guard<std::mutex> lock(mCacheMutex);
        auto result = mCache.emplace(hash, std::move(texture));
        SPDLOG_DEBUG("[RTX] TextureManager::GetOrCreateTexture: loaded texture {}x{} fmt=0x{:02X} at SRV index {} (hash: 0x{:016X})",
                     width, height, format, result.first->second.descriptorIndex, hash);
        return &result.first->second;
    }
}

// ============================================================================
// GetOrCreateTexture (Task-spec Phase 6 exact signature:
//   hash, data, width, height, n64Format, n64Size)
// Returns RTXTexture*. This is the primary entry point specified by the task.
// Decodes N64 texture data to RGBA8, uploads to GPU DEFAULT heap via
// UPLOAD heap staging buffer, creates SRV, and caches.
// ============================================================================

RTXTexture* TextureManager::GetOrCreateTexture(
    uint64_t hash, const uint8_t* data,
    uint32_t width, uint32_t height,
    uint32_t n64Format, uint32_t n64Size)
{
    if (!mInitialized) {
        SPDLOG_ERROR("[RTX] TextureManager::GetOrCreateTexture(hash,data,w,h,fmt,siz) called before Initialize");
        return nullptr;
    }

    if (!data || width == 0 || height == 0) {
        SPDLOG_WARN("[RTX] TextureManager::GetOrCreateTexture: invalid texture data (hash: 0x{:016X})", hash);
        return nullptr;
    }

    // Check cache first
    {
        std::lock_guard<std::mutex> lock(mCacheMutex);
        auto it = mCache.find(hash);
        if (it != mCache.end()) {
            return &it->second;
        }
    }

    if (width > 4096 || height > 4096) {
        SPDLOG_WARN("[RTX] TextureManager::GetOrCreateTexture: texture too large ({}x{}, hash: 0x{:016X})",
                     width, height, hash);
        return nullptr;
    }

    // Check capacity
    if (mNextDescriptorIndex >= mMaxTextures && mFreeSRVIndices.empty()) {
        SPDLOG_WARN("[RTX] TextureManager::GetOrCreateTexture: SRV heap full ({} textures)", mMaxTextures);
        return nullptr;
    }

    // Decode N64 texels into RGBA8 using the provided format/size directly
    std::vector<uint8_t> rgba8 = ConvertN64Texture(data, n64Format, n64Size,
                                                    width, height, nullptr, G_IM_SIZ_16b);

    if (rgba8.empty()) {
        SPDLOG_ERROR("[RTX] TextureManager::GetOrCreateTexture: decode failed "
                     "(hash: 0x{:016X}, fmt={}, siz={})", hash, n64Format, n64Size);
        return nullptr;
    }

    // Upload to GPU and create SRV (legacy path)
    RTXTexture texture;
    if (!CreateGPUTextureLegacy(rgba8.data(), width, height, texture)) {
        SPDLOG_ERROR("[RTX] TextureManager::GetOrCreateTexture: GPU upload failed "
                     "({}x{}, hash: 0x{:016X})", width, height, hash);
        return nullptr;
    }
    texture.n64Hash = hash;

    // Insert into cache
    {
        std::lock_guard<std::mutex> lock(mCacheMutex);
        auto result = mCache.emplace(hash, std::move(texture));
        SPDLOG_DEBUG("[RTX] TextureManager::GetOrCreateTexture: loaded texture {}x{} "
                     "fmt={} siz={} at SRV index {} (hash: 0x{:016X})",
                     width, height, n64Format, n64Size,
                     result.first->second.descriptorIndex, hash);
        return &result.first->second;
    }
}

// ============================================================================
// GetDefaultWhiteTextureRTX — returns RTXTexture* for the 1x1 white fallback
// This is the task-spec Phase 6 return type version.
// ============================================================================

RTXTexture* TextureManager::GetDefaultWhiteTextureRTX() {
    if (!mInitialized) {
        SPDLOG_ERROR("[RTX] TextureManager::GetDefaultWhiteTextureRTX called before Initialize");
        return nullptr;
    }

    if (!mDefaultWhiteCreated) {
        CreateDefaultWhiteTexture();
    }

    if (!mDefaultWhiteCreated) {
        return nullptr;
    }

    // Populate RTXTexture version from the RTXTextureInfo version if not yet done
    if (mDefaultWhiteRTXTexture.srvHandle.ptr == 0 && mDefaultWhiteTexture.srvGpuHandle.ptr != 0) {
        mDefaultWhiteRTXTexture.n64Hash = mDefaultWhiteTexture.hash;
        mDefaultWhiteRTXTexture.resource = mDefaultWhiteTexture.gpuTexture;
        mDefaultWhiteRTXTexture.srvCPU = GetCPUHandle(mDefaultWhiteTexture.srvIndex);
        mDefaultWhiteRTXTexture.srvHandle = mDefaultWhiteTexture.srvGpuHandle;
        mDefaultWhiteRTXTexture.width = mDefaultWhiteTexture.width;
        mDefaultWhiteRTXTexture.height = mDefaultWhiteTexture.height;
        mDefaultWhiteRTXTexture.format = mDefaultWhiteTexture.format;
        mDefaultWhiteRTXTexture.descriptorIndex = mDefaultWhiteTexture.srvIndex;
    }

    return &mDefaultWhiteRTXTexture;
}

// ============================================================================
// GetDefaultWhiteTextureEntry — returns RTXTextureEntry* for the 1x1 white fallback
// This is the task-spec Phase 6 exact return type version.
// ============================================================================

RTXTextureEntry* TextureManager::GetDefaultWhiteTextureEntry() {
    if (!mInitialized) {
        SPDLOG_ERROR("[RTX] TextureManager::GetDefaultWhiteTextureEntry called before Initialize");
        return nullptr;
    }

    if (!mDefaultWhiteCreated) {
        CreateDefaultWhiteTexture();
    }

    if (!mDefaultWhiteCreated) {
        return nullptr;
    }

    // Populate RTXTextureEntry version from the RTXTextureInfo version if not yet done
    if (mDefaultWhiteEntry.srvGPU.ptr == 0 && mDefaultWhiteTexture.srvGpuHandle.ptr != 0) {
        mDefaultWhiteEntry.hash = mDefaultWhiteTexture.hash;
        mDefaultWhiteEntry.gpuTexture = mDefaultWhiteTexture.gpuTexture;
        mDefaultWhiteEntry.srvCPU = GetCPUHandle(mDefaultWhiteTexture.srvIndex);
        mDefaultWhiteEntry.srvGPU = mDefaultWhiteTexture.srvGpuHandle;
        mDefaultWhiteEntry.width = mDefaultWhiteTexture.width;
        mDefaultWhiteEntry.height = mDefaultWhiteTexture.height;
        mDefaultWhiteEntry.format = mDefaultWhiteTexture.format;
        mDefaultWhiteEntry.srvIndex = mDefaultWhiteTexture.srvIndex;
        mDefaultWhiteEntry.lastUsedFrame = 0;
    }

    return &mDefaultWhiteEntry;
}

// ============================================================================
// GetBindlessHandle - get GPU descriptor handle for a cached texture by hash
// ============================================================================

D3D12_GPU_DESCRIPTOR_HANDLE TextureManager::GetBindlessHandle(uint64_t hash) {
    std::lock_guard<std::mutex> lock(mCacheMutex);

    // Check LRU cache
    auto lruIt = mLRUCache.find(hash);
    if (lruIt != mLRUCache.end()) {
        return lruIt->second.texture.srvGpuHandle;
    }

    // Check legacy RTXTexture cache
    auto cacheIt = mCache.find(hash);
    if (cacheIt != mCache.end()) {
        return cacheIt->second.srvHandle;
    }

    // Check legacy RTXTextureInfo cache
    auto infoIt = mTextureCache.find(hash);
    if (infoIt != mTextureCache.end()) {
        return infoIt->second.srvGpuHandle;
    }

    // Not found — return a null handle
    D3D12_GPU_DESCRIPTOR_HANDLE nullHandle = {};
    nullHandle.ptr = 0;
    return nullHandle;
}

// ============================================================================
// CreateSRVHeap - create (or recreate) the shader-visible SRV descriptor heap
// ============================================================================

void TextureManager::CreateSRVHeap(uint32_t maxTextures) {
    if (!mRawDevice && !mDevice) {
        SPDLOG_ERROR("[RTX] TextureManager::CreateSRVHeap: no device available");
        return;
    }

    ID3D12Device* device = mRawDevice ? mRawDevice : static_cast<ID3D12Device*>(mDevice);

    // Release existing heap if present
    if (mSrvHeap) {
        mSrvHeap.Reset();
    }

    mMaxTextures = maxTextures;

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.NumDescriptors = maxTextures;
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    heapDesc.NodeMask = 0;

    HRESULT hr = device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&mSrvHeap));
    if (FAILED(hr)) {
        SPDLOG_ERROR("[RTX] TextureManager::CreateSRVHeap: failed to create SRV descriptor heap ({} descriptors): 0x{:08X}",
                     maxTextures, static_cast<uint32_t>(hr));
        return;
    }

    mSrvDescriptorSize = device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    mDescriptorSize = mSrvDescriptorSize;
    mUsingExternalHeap = false;

    SPDLOG_INFO("[RTX] TextureManager::CreateSRVHeap: created SRV heap ({} descriptors, descriptor size: {})",
                maxTextures, mSrvDescriptorSize);
}

// ============================================================================
// DecodeN64Texture - task-spec alias for ConvertN64Texture
// ============================================================================

std::vector<uint8_t> TextureManager::DecodeN64Texture(
    const uint8_t* tmem,
    uint32_t fmt, uint32_t siz,
    uint32_t width, uint32_t height,
    const uint8_t* tlut,
    uint32_t tlutFormat)
{
    return ConvertN64Texture(tmem, fmt, siz, width, height, tlut, tlutFormat);
}

// ============================================================================
// N64TextureFormatToFmtSiz — convert N64TextureFormat enum to (imgFmt, imgSiz) pair
// ============================================================================

void TextureManager::N64TextureFormatToFmtSiz(N64TextureFormat format, uint32_t& outFmt, uint32_t& outSiz) {
    switch (format) {
        case N64TextureFormat::RGBA16:
            outFmt = G_IM_FMT_RGBA;
            outSiz = G_IM_SIZ_16b;
            break;
        case N64TextureFormat::RGBA32:
            outFmt = G_IM_FMT_RGBA;
            outSiz = G_IM_SIZ_32b;
            break;
        case N64TextureFormat::CI4:
            outFmt = G_IM_FMT_CI;
            outSiz = G_IM_SIZ_4b;
            break;
        case N64TextureFormat::CI8:
            outFmt = G_IM_FMT_CI;
            outSiz = G_IM_SIZ_8b;
            break;
        case N64TextureFormat::IA4:
            outFmt = G_IM_FMT_IA;
            outSiz = G_IM_SIZ_4b;
            break;
        case N64TextureFormat::IA8:
            outFmt = G_IM_FMT_IA;
            outSiz = G_IM_SIZ_8b;
            break;
        case N64TextureFormat::IA16:
            outFmt = G_IM_FMT_IA;
            outSiz = G_IM_SIZ_16b;
            break;
        case N64TextureFormat::I4:
            outFmt = G_IM_FMT_I;
            outSiz = G_IM_SIZ_4b;
            break;
        case N64TextureFormat::I8:
            outFmt = G_IM_FMT_I;
            outSiz = G_IM_SIZ_8b;
            break;
        default:
            // Fallback: treat as RGBA16
            outFmt = G_IM_FMT_RGBA;
            outSiz = G_IM_SIZ_16b;
            SPDLOG_WARN("[RTX] N64TextureFormatToFmtSiz: unknown format {}, defaulting to RGBA16",
                         static_cast<uint32_t>(format));
            break;
    }
}

// ============================================================================
// GetOrCreateTexture — Phase 6 primary entry point using N64TextureFormat enum.
// Returns D3D12_GPU_DESCRIPTOR_HANDLE compatible with RTXMaterial::diffuseTextureSRV
// and RTXMaterial::normalMapSRV.
// ============================================================================

D3D12_GPU_DESCRIPTOR_HANDLE TextureManager::GetOrCreateTexture(
    uint64_t tmemAddr,
    N64TextureFormat format,
    uint32_t width, uint32_t height,
    const uint8_t* rawData, size_t dataSize,
    const uint8_t* palette)
{
    D3D12_GPU_DESCRIPTOR_HANDLE defaultHandle = GetDefaultTexture();

    if (!mInitialized) {
        SPDLOG_ERROR("[RTX] TextureManager::GetOrCreateTexture(N64TextureFormat) called before Initialize");
        return defaultHandle;
    }

    if (!rawData || dataSize == 0 || width == 0 || height == 0) {
        SPDLOG_WARN("[RTX] TextureManager::GetOrCreateTexture(N64TextureFormat): invalid texture data");
        return defaultHandle;
    }

    if (width > 4096 || height > 4096) {
        SPDLOG_WARN("[RTX] TextureManager::GetOrCreateTexture(N64TextureFormat): texture too large ({}x{})",
                     width, height);
        return defaultHandle;
    }

    // Compute cache key from texture data hash + format + dimensions + tmemAddr
    uint64_t hashKey = HashTextureData(rawData, dataSize);

    constexpr uint64_t FNV_PRIME = 0x100000001b3ULL;
    hashKey ^= static_cast<uint64_t>(format);
    hashKey *= FNV_PRIME;
    hashKey ^= tmemAddr;
    hashKey *= FNV_PRIME;
    hashKey ^= static_cast<uint64_t>(width);
    hashKey *= FNV_PRIME;
    hashKey ^= static_cast<uint64_t>(height);
    hashKey *= FNV_PRIME;

    // Mix in palette data if present (CI formats)
    if (palette && (format == N64TextureFormat::CI4 || format == N64TextureFormat::CI8)) {
        size_t paletteBytes = (format == N64TextureFormat::CI4) ? 32 : 512;
        uint64_t palHash = HashTextureData(palette, paletteBytes);
        hashKey ^= palHash;
        hashKey *= FNV_PRIME;
    }

    // Check TextureCacheEntry cache (with lock)
    {
        std::lock_guard<std::mutex> lock(mCacheMutex);
        auto it = mTextureCacheEntries.find(hashKey);
        if (it != mTextureCacheEntries.end()) {
            // Cache hit: update LRU frame
            it->second.lastUsedFrame = mCurrentFrame;
            return it->second.srvHandle;
        }
    }

    // Cache miss: convert N64TextureFormat to (imgFmt, imgSiz) and decode
    uint32_t imgFmt = 0, imgSiz = 0;
    N64TextureFormatToFmtSiz(format, imgFmt, imgSiz);

    std::vector<uint8_t> rgba8 = ConvertN64Texture(rawData, imgFmt, imgSiz,
                                                     width, height, palette, G_IM_SIZ_16b);
    if (rgba8.empty()) {
        SPDLOG_ERROR("[RTX] TextureManager::GetOrCreateTexture(N64TextureFormat): decode failed (hash: 0x{:016X})",
                     hashKey);
        return defaultHandle;
    }

    // Allocate SRV index (may evict LRU if full)
    uint32_t srvIndex;
    {
        std::lock_guard<std::mutex> lock(mCacheMutex);

        // Evict from TextureCacheEntry map if over capacity
        while (mTextureCacheEntries.size() >= mMaxCachedTextures) {
            // Find the entry with the smallest lastUsedFrame (LRU eviction)
            uint64_t oldestKey = 0;
            uint64_t oldestFrame = UINT64_MAX;
            for (auto& kv : mTextureCacheEntries) {
                if (kv.second.lastUsedFrame < oldestFrame) {
                    oldestFrame = kv.second.lastUsedFrame;
                    oldestKey = kv.first;
                }
            }
            if (oldestFrame < UINT64_MAX) {
                mTextureCacheEntries.erase(oldestKey);
                // Note: SRV index is not reclaimed here since we're in a separate cache.
                // The LRU-based AllocateSRVIndex handles main heap slot recycling.
            } else {
                break;
            }
        }

        srvIndex = AllocateSRVIndex();
    }

    if (srvIndex == UINT32_MAX) {
        SPDLOG_ERROR("[RTX] TextureManager::GetOrCreateTexture(N64TextureFormat): no SRV slots available");
        return defaultHandle;
    }

    // Create GPU texture resource and upload
    CachedTexture cachedTex;
    cachedTex.hashKey = hashKey;
    cachedTex.width = width;
    cachedTex.height = height;
    cachedTex.srvIndex = srvIndex;
    cachedTex.dxgiFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    cachedTex.lastUsedFrame = mCurrentFrame;
    cachedTex.uploaded = false;

    if (!CreateGPUTextureAtIndex(rgba8.data(), width, height, srvIndex, cachedTex)) {
        SPDLOG_ERROR("[RTX] TextureManager::GetOrCreateTexture(N64TextureFormat): GPU upload failed ({}x{}, hash: 0x{:016X})",
                     width, height, hashKey);
        std::lock_guard<std::mutex> lock(mCacheMutex);
        mFreeSRVIndices.push_back(srvIndex);
        return defaultHandle;
    }

    D3D12_GPU_DESCRIPTOR_HANDLE resultHandle = cachedTex.srvGpuHandle;

    // Insert into TextureCacheEntry map
    {
        std::lock_guard<std::mutex> lock(mCacheMutex);

        TextureCacheEntry entry;
        entry.resource = cachedTex.gpuTexture;
        entry.srvHandle = cachedTex.srvGpuHandle;
        entry.width = width;
        entry.height = height;
        entry.lastUsedFrame = mCurrentFrame;
        mTextureCacheEntries.emplace(hashKey, std::move(entry));

        // Also insert into LRU cache for unified management
        mLRUList.push_front(hashKey);
        LRUCacheEntry lruEntry;
        lruEntry.texture = std::move(cachedTex);
        lruEntry.lruIter = mLRUList.begin();
        mLRUCache.emplace(hashKey, std::move(lruEntry));
    }

    SPDLOG_DEBUG("[RTX] TextureManager: loaded texture {}x{} N64TextureFormat={} at SRV index {} (hash: 0x{:016X})",
                 width, height, static_cast<uint32_t>(format), srvIndex, hashKey);
    return resultHandle;
}

// ============================================================================
// GetOrCreateTexture — Phase 6 task-spec: hash, tmemData, width, height,
// n64Format, n64Size, palette. Returns RTXTextureEntry*.
// ============================================================================

RTXTextureEntry* TextureManager::GetOrCreateTexture(
    uint64_t hash,
    const uint8_t* tmemData,
    uint32_t width, uint32_t height,
    uint32_t n64Format, uint32_t n64Size,
    const uint8_t* palette)
{
    if (!mInitialized) {
        SPDLOG_ERROR("[RTX] TextureManager::GetOrCreateTexture(RTXTextureEntry) called before Initialize");
        return nullptr;
    }

    if (!tmemData || width == 0 || height == 0) {
        SPDLOG_WARN("[RTX] TextureManager::GetOrCreateTexture(RTXTextureEntry): invalid texture data");
        return nullptr;
    }

    if (width > 4096 || height > 4096) {
        SPDLOG_WARN("[RTX] TextureManager::GetOrCreateTexture(RTXTextureEntry): texture too large ({}x{})",
                     width, height);
        return nullptr;
    }

    // Check RTXTextureEntry cache first (with lock)
    {
        std::lock_guard<std::mutex> lock(mCacheMutex);
        auto it = mTextureEntryCache.find(hash);
        if (it != mTextureEntryCache.end()) {
            // Cache hit: update last used frame
            it->second.lastUsedFrame = mCurrentFrame;
            return &it->second;
        }
    }

    // Cache miss: decode N64 texels into RGBA8
    std::vector<uint8_t> rgba8 = ConvertN64Texture(tmemData, n64Format, n64Size,
                                                      width, height, palette, G_IM_SIZ_16b);
    if (rgba8.empty()) {
        SPDLOG_ERROR("[RTX] TextureManager::GetOrCreateTexture(RTXTextureEntry): decode failed "
                     "(hash: 0x{:016X}, fmt={}, siz={})", hash, n64Format, n64Size);
        return nullptr;
    }

    // Allocate SRV index (may evict LRU if full)
    uint32_t srvIndex;
    {
        std::lock_guard<std::mutex> lock(mCacheMutex);
        srvIndex = AllocateSRVIndex();
    }

    if (srvIndex == UINT32_MAX) {
        SPDLOG_ERROR("[RTX] TextureManager::GetOrCreateTexture(RTXTextureEntry): no SRV slots available");
        return nullptr;
    }

    // Create GPU texture resource
    ID3D12Device* device = mRawDevice;
    if (!device) {
        std::lock_guard<std::mutex> lock(mCacheMutex);
        mFreeSRVIndices.push_back(srvIndex);
        return nullptr;
    }

    // Step 1: Create the destination TEXTURE2D on the default heap
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
    texDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    D3D12_HEAP_PROPERTIES defaultHeap = {};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

    ComPtr<ID3D12Resource> textureResource;
    HRESULT hr = device->CreateCommittedResource(
        &defaultHeap,
        D3D12_HEAP_FLAG_NONE,
        &texDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(&textureResource));
    if (FAILED(hr)) {
        SPDLOG_ERROR("[RTX] TextureManager::GetOrCreateTexture(RTXTextureEntry): "
                     "CreateCommittedResource failed ({}x{}): 0x{:08X}",
                     width, height, static_cast<uint32_t>(hr));
        std::lock_guard<std::mutex> lock(mCacheMutex);
        mFreeSRVIndices.push_back(srvIndex);
        return nullptr;
    }

    // Step 2: Query upload layout and create staging (upload) buffer
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT numRows = 0;
    UINT64 rowSizeInBytes = 0;
    UINT64 totalBytes = 0;

    device->GetCopyableFootprints(&texDesc, 0, 1, 0,
                                   &footprint, &numRows, &rowSizeInBytes, &totalBytes);

    ComPtr<ID3D12Resource> uploadBuffer = CreateUploadBuffer(totalBytes);
    if (!uploadBuffer) {
        SPDLOG_ERROR("[RTX] TextureManager::GetOrCreateTexture(RTXTextureEntry): "
                     "staging buffer creation failed ({} bytes)", totalBytes);
        std::lock_guard<std::mutex> lock(mCacheMutex);
        mFreeSRVIndices.push_back(srvIndex);
        return nullptr;
    }

    // Step 3: Map staging buffer and copy pixel data row-by-row
    void* mapped = nullptr;
    hr = uploadBuffer->Map(0, nullptr, &mapped);
    if (FAILED(hr)) {
        SPDLOG_ERROR("[RTX] TextureManager::GetOrCreateTexture(RTXTextureEntry): "
                     "Map staging buffer failed: 0x{:08X}", static_cast<uint32_t>(hr));
        std::lock_guard<std::mutex> lock(mCacheMutex);
        mFreeSRVIndices.push_back(srvIndex);
        return nullptr;
    }

    const uint32_t srcRowPitch = width * 4; // RGBA8 = 4 bytes per pixel
    const uint32_t dstRowPitch = footprint.Footprint.RowPitch;
    uint8_t* dstBase = static_cast<uint8_t*>(mapped) + footprint.Offset;

    for (uint32_t row = 0; row < height; row++) {
        const uint8_t* srcRow = rgba8.data() + row * srcRowPitch;
        uint8_t* dstRow = dstBase + row * dstRowPitch;
        memcpy(dstRow, srcRow, srcRowPitch);
    }

    uploadBuffer->Unmap(0, nullptr);

    // Step 4: Record copy command and resource barrier
    hr = mUploadCmdAllocator->Reset();
    if (FAILED(hr)) {
        SPDLOG_ERROR("[RTX] TextureManager::GetOrCreateTexture(RTXTextureEntry): "
                     "Reset command allocator failed: 0x{:08X}", static_cast<uint32_t>(hr));
        std::lock_guard<std::mutex> lock(mCacheMutex);
        mFreeSRVIndices.push_back(srvIndex);
        return nullptr;
    }

    hr = mUploadCmdList->Reset(mUploadCmdAllocator.Get(), nullptr);
    if (FAILED(hr)) {
        SPDLOG_ERROR("[RTX] TextureManager::GetOrCreateTexture(RTXTextureEntry): "
                     "Reset command list failed: 0x{:08X}", static_cast<uint32_t>(hr));
        std::lock_guard<std::mutex> lock(mCacheMutex);
        mFreeSRVIndices.push_back(srvIndex);
        return nullptr;
    }

    D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
    dstLoc.pResource = textureResource.Get();
    dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dstLoc.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
    srcLoc.pResource = uploadBuffer.Get();
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcLoc.PlacedFootprint = footprint;

    mUploadCmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

    // Transition from COPY_DEST to shader resource
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = textureResource.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    mUploadCmdList->ResourceBarrier(1, &barrier);

    // Execute and wait for GPU
    ExecuteUploadAndWait();

    // Step 5: Create SRV descriptor at the allocated index
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.PlaneSlice = 0;
    srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = GetCPUHandle(srvIndex);
    device->CreateShaderResourceView(textureResource.Get(), &srvDesc, cpuHandle);

    D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = GetGPUHandle(srvIndex);

    // Step 6: Build RTXTextureEntry and insert into cache
    RTXTextureEntry entry;
    entry.hash = hash;
    entry.width = width;
    entry.height = height;
    entry.format = DXGI_FORMAT_R8G8B8A8_UNORM;
    entry.gpuTexture = std::move(textureResource);
    entry.srvCPU = cpuHandle;
    entry.srvGPU = gpuHandle;
    entry.srvIndex = srvIndex;
    entry.lastUsedFrame = mCurrentFrame;

    {
        std::lock_guard<std::mutex> lock(mCacheMutex);
        // Track the upload buffer for lifetime management
        mTextureResources.push_back(std::move(uploadBuffer));

        auto result = mTextureEntryCache.emplace(hash, std::move(entry));
        SPDLOG_DEBUG("[RTX] TextureManager::GetOrCreateTexture(RTXTextureEntry): loaded texture "
                     "{}x{} fmt={} siz={} at SRV index {} (hash: 0x{:016X})",
                     width, height, n64Format, n64Size, srvIndex, hash);
        return &result.first->second;
    }
}

// ============================================================================
// GetSRVHandle — Phase 6 task-spec: returns GPU descriptor handle for a
// cached texture by hash. Searches all caches.
// Returns the default white texture handle if not found.
// ============================================================================

D3D12_GPU_DESCRIPTOR_HANDLE TextureManager::GetSRVHandle(uint64_t hash) {
    std::lock_guard<std::mutex> lock(mCacheMutex);

    // Check RTXTextureEntry cache first (Phase 6 primary cache)
    auto entryIt = mTextureEntryCache.find(hash);
    if (entryIt != mTextureEntryCache.end()) {
        entryIt->second.lastUsedFrame = mCurrentFrame;
        return entryIt->second.srvGPU;
    }

    // Check LRU cache
    auto lruIt = mLRUCache.find(hash);
    if (lruIt != mLRUCache.end()) {
        lruIt->second.texture.lastUsedFrame = mCurrentFrame;
        return lruIt->second.texture.srvGpuHandle;
    }

    // Check Phase 6 resource cache
    auto resIt = mResourceCache.find(hash);
    if (resIt != mResourceCache.end()) {
        return GetGPUHandle(resIt->second.srvIndex);
    }

    // Check legacy RTXTexture cache
    auto cacheIt = mCache.find(hash);
    if (cacheIt != mCache.end()) {
        return cacheIt->second.srvHandle;
    }

    // Check legacy RTXTextureInfo cache
    auto infoIt = mTextureCache.find(hash);
    if (infoIt != mTextureCache.end()) {
        return infoIt->second.srvGpuHandle;
    }

    // Check TextureCacheEntry cache
    auto tceIt = mTextureCacheEntries.find(hash);
    if (tceIt != mTextureCacheEntries.end()) {
        return tceIt->second.srvHandle;
    }

    // Not found — return the default white texture handle
    if (mDefaultWhiteCreated) {
        return mDefaultWhiteTexture.srvGpuHandle;
    }

    // Ultimate fallback: return null handle
    D3D12_GPU_DESCRIPTOR_HANDLE nullHandle = {};
    nullHandle.ptr = 0;
    return nullHandle;
}

// ============================================================================
// EvictUnusedTextures — Phase 6 task-spec: evict textures whose lastUsedFrame
// is older than (currentFrame - maxAge). Iterates all caches.
// ============================================================================

void TextureManager::EvictUnusedTextures(uint64_t currentFrame, uint64_t maxAge) {
    std::lock_guard<std::mutex> lock(mCacheMutex);

    uint64_t threshold = (currentFrame > maxAge) ? (currentFrame - maxAge) : 0;
    uint32_t evictedCount = 0;

    // Evict from RTXTextureEntry cache
    for (auto it = mTextureEntryCache.begin(); it != mTextureEntryCache.end(); ) {
        if (it->second.lastUsedFrame < threshold) {
            uint32_t freedIndex = it->second.srvIndex;

            // Don't reclaim reserved SRV indices (0=white, 1=checkerboard, 2=flat normal)
            if (freedIndex >= RESERVED_SRV_SLOTS) {
                mFreeSRVIndices.push_back(freedIndex);
            }

            SPDLOG_DEBUG("[RTX] TextureManager::EvictUnusedTextures: evicting RTXTextureEntry "
                         "(hash: 0x{:016X}, SRV index: {}, last used frame: {})",
                         it->second.hash, freedIndex, it->second.lastUsedFrame);

            it = mTextureEntryCache.erase(it);
            evictedCount++;
        } else {
            ++it;
        }
    }

    // Evict from LRU cache
    for (auto it = mLRUCache.begin(); it != mLRUCache.end(); ) {
        if (it->second.texture.lastUsedFrame < threshold) {
            uint32_t freedIndex = it->second.texture.srvIndex;

            if (freedIndex >= RESERVED_SRV_SLOTS) {
                mFreeSRVIndices.push_back(freedIndex);
            }

            // Remove from LRU list
            mLRUList.erase(it->second.lruIter);

            SPDLOG_DEBUG("[RTX] TextureManager::EvictUnusedTextures: evicting LRU texture "
                         "(hash: 0x{:016X}, SRV index: {}, last used frame: {})",
                         it->second.texture.hashKey, freedIndex, it->second.texture.lastUsedFrame);

            it = mLRUCache.erase(it);
            evictedCount++;
        } else {
            ++it;
        }
    }

    // Evict from TextureCacheEntry cache
    for (auto it = mTextureCacheEntries.begin(); it != mTextureCacheEntries.end(); ) {
        if (it->second.lastUsedFrame < threshold) {
            it = mTextureCacheEntries.erase(it);
            evictedCount++;
        } else {
            ++it;
        }
    }

    if (evictedCount > 0) {
        SPDLOG_INFO("[RTX] TextureManager::EvictUnusedTextures: evicted {} textures "
                     "(currentFrame: {}, maxAge: {}, threshold: {})",
                     evictedCount, currentFrame, maxAge, threshold);
    }
}

// ============================================================================
// CreateDefaultTextures — creates all default textures:
//   - 1x1 white texture (SRV index 0)
//   - 8x8 checkerboard fallback (SRV index 1)
//   - 1x1 flat normal map (SRV index 2): RGBA=(128,128,255,255)
// ============================================================================

void TextureManager::CreateDefaultTextures() {
    CreateDefaultWhiteTexture();
    CreateCheckerboardFallbackTexture();
    CreateDefaultNormalMapTexture();
}

// ============================================================================
// CreateDefaultNormalMapTexture — 1x1 flat normal map at SRV index 2
// RGBA = (128, 128, 255, 255) which decodes to tangent-space normal (0, 0, 1)
// Compatible with RTXMaterial::normalMapSRV
// ============================================================================

void TextureManager::CreateDefaultNormalMapTexture() {
    if (mDefaultNormalMapCreated) return;

    // 1x1 flat normal pixel: (128, 128, 255, 255) -> tangent-space (0, 0, 1)
    uint8_t normalPixel[4] = { 128, 128, 255, 255 };

    mDefaultNormalMapTexture.hash = 0xFFFFFFFFFFFFFFFFULL;  // Special hash for default normal
    mDefaultNormalMapTexture.width = 1;
    mDefaultNormalMapTexture.height = 1;
    mDefaultNormalMapTexture.srvIndex = 0; // Will be assigned by CreateGPUTexture
    mDefaultNormalMapTexture.format = DXGI_FORMAT_R8G8B8A8_UNORM;
    mDefaultNormalMapTexture.isBound = false;
    mDefaultNormalMapTexture.uploaded = false;

    if (CreateGPUTexture(normalPixel, 1, 1, mDefaultNormalMapTexture)) {
        mDefaultNormalMapCreated = true;
        SPDLOG_INFO("[RTX] TextureManager: default 1x1 flat normal map created at SRV index {}",
                    mDefaultNormalMapTexture.srvIndex);
    } else {
        SPDLOG_ERROR("[RTX] TextureManager: failed to create default normal map texture");
    }
}

// ============================================================================
// GetDefaultNormalMap — returns GPU descriptor handle for the 1x1 flat normal map
// ============================================================================

D3D12_GPU_DESCRIPTOR_HANDLE TextureManager::GetDefaultNormalMap() {
    if (!mInitialized || !mDefaultNormalMapCreated) {
        D3D12_GPU_DESCRIPTOR_HANDLE nullHandle = {};
        nullHandle.ptr = 0;
        return nullHandle;
    }
    return mDefaultNormalMapTexture.srvGpuHandle;
}

// ============================================================================
// GetDefaultNormalMapTexture
// ============================================================================

RTXTextureInfo* TextureManager::GetDefaultNormalMapTexture() {
    if (!mInitialized) {
        SPDLOG_ERROR("[RTX] TextureManager::GetDefaultNormalMapTexture called before Initialize");
        return nullptr;
    }

    if (!mDefaultNormalMapCreated) {
        CreateDefaultNormalMapTexture();
    }

    return mDefaultNormalMapCreated ? &mDefaultNormalMapTexture : nullptr;
}

// ============================================================================
// GetOrCreateTextureGPUHandle — Task-spec Phase 6 primary API.
// Accepts pre-decoded RGBA32 data and a hash key.
// Returns D3D12_GPU_DESCRIPTOR_HANDLE for the texture SRV.
// Wraps GetOrCreateTexture(hash, rgbaData, width, height) -> RTXTextureInfo*.
// ============================================================================

D3D12_GPU_DESCRIPTOR_HANDLE TextureManager::GetOrCreateTextureGPUHandle(
    uint64_t hash, const uint8_t* rgba32Data,
    uint32_t width, uint32_t height)
{
    D3D12_GPU_DESCRIPTOR_HANDLE nullHandle = {};
    nullHandle.ptr = 0;

    RTXTextureInfo* info = GetOrCreateTexture(hash, rgba32Data, width, height);
    if (info) {
        return info->srvGpuHandle;
    }

    // Fallback to default white texture if available
    if (mDefaultWhiteCreated) {
        return mDefaultWhiteTexture.srvGpuHandle;
    }

    return nullHandle;
}

// ============================================================================
// InvalidateTexture — Task-spec Phase 6: Invalidate a single cached texture
// by hash. Removes from all caches and reclaims the SRV descriptor slot.
// ============================================================================

void TextureManager::InvalidateTexture(uint64_t hash) {
    std::lock_guard<std::mutex> lock(mCacheMutex);

    bool found = false;

    // Remove from RTXTextureEntry cache
    {
        auto it = mTextureEntryCache.find(hash);
        if (it != mTextureEntryCache.end()) {
            uint32_t freedIndex = it->second.srvIndex;
            if (freedIndex >= RESERVED_SRV_SLOTS) {
                mFreeSRVIndices.push_back(freedIndex);
            }
            mTextureEntryCache.erase(it);
            found = true;
        }
    }

    // Remove from LRU cache
    {
        auto it = mLRUCache.find(hash);
        if (it != mLRUCache.end()) {
            uint32_t freedIndex = it->second.texture.srvIndex;
            if (freedIndex >= RESERVED_SRV_SLOTS) {
                mFreeSRVIndices.push_back(freedIndex);
            }
            mLRUList.erase(it->second.lruIter);
            mLRUCache.erase(it);
            found = true;
        }
    }

    // Remove from Phase 6 resource cache
    {
        auto it = mResourceCache.find(hash);
        if (it != mResourceCache.end()) {
            uint32_t freedIndex = it->second.srvIndex;
            if (freedIndex >= RESERVED_SRV_SLOTS) {
                mFreeSRVIndices.push_back(freedIndex);
            }
            mResourceCache.erase(it);
            found = true;
        }
    }

    // Remove from legacy RTXTexture cache
    {
        auto it = mCache.find(hash);
        if (it != mCache.end()) {
            uint32_t freedIndex = it->second.descriptorIndex;
            if (freedIndex >= RESERVED_SRV_SLOTS) {
                mFreeSRVIndices.push_back(freedIndex);
            }
            mCache.erase(it);
            found = true;
        }
    }

    // Remove from legacy RTXTextureInfo cache
    {
        auto it = mTextureCache.find(hash);
        if (it != mTextureCache.end()) {
            uint32_t freedIndex = it->second.srvIndex;
            if (freedIndex >= RESERVED_SRV_SLOTS) {
                mFreeSRVIndices.push_back(freedIndex);
            }
            mTextureCache.erase(it);
            found = true;
        }
    }

    // Remove from TextureCacheEntry cache
    {
        auto it = mTextureCacheEntries.find(hash);
        if (it != mTextureCacheEntries.end()) {
            mTextureCacheEntries.erase(it);
            found = true;
        }
    }

    if (found) {
        SPDLOG_DEBUG("[RTX] TextureManager::InvalidateTexture: invalidated texture (hash: 0x{:016X})", hash);
    } else {
        SPDLOG_DEBUG("[RTX] TextureManager::InvalidateTexture: texture not found in cache (hash: 0x{:016X})", hash);
    }
}

// ============================================================================
// GetOrCreateTexture — Task-spec Phase 6 primary entry point.
// Accepts pre-decoded RGBA8 data and a hash key. Returns RTXTextureInfo*.
// If the hash is already cached, returns the cached entry. Otherwise creates
// a committed D3D12 resource on the default heap, uploads via staging buffer,
// creates SRV, and caches.
// ============================================================================

RTXTextureInfo* TextureManager::GetOrCreateTexture(
    uint64_t hash, const uint8_t* rgbaData,
    uint32_t width, uint32_t height)
{
    if (!mInitialized) {
        SPDLOG_ERROR("[RTX] TextureManager::GetOrCreateTexture(hash,rgba) called before Initialize");
        return nullptr;
    }

    if (!rgbaData || width == 0 || height == 0) {
        SPDLOG_WARN("[RTX] TextureManager::GetOrCreateTexture(hash,rgba): invalid parameters");
        return nullptr;
    }

    if (width > 4096 || height > 4096) {
        SPDLOG_WARN("[RTX] TextureManager::GetOrCreateTexture(hash,rgba): texture too large ({}x{})",
                     width, height);
        return nullptr;
    }

    // Check RTXTextureInfo cache (mTextureCache) first
    {
        std::lock_guard<std::mutex> lock(mCacheMutex);
        auto it = mTextureCache.find(hash);
        if (it != mTextureCache.end()) {
            // Cache hit: mark as bound and return
            it->second.isBound = true;
            return &it->second;
        }
    }

    // Cache miss: create GPU texture resource and upload RGBA data
    RTXTextureInfo texture;
    texture.hash = hash;
    texture.width = width;
    texture.height = height;
    texture.srvIndex = 0;
    texture.format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texture.isBound = true;
    texture.uploaded = false;

    if (!CreateGPUTexture(rgbaData, width, height, texture)) {
        SPDLOG_ERROR("[RTX] TextureManager::GetOrCreateTexture(hash,rgba): GPU upload failed "
                     "({}x{}, hash: 0x{:016X})", width, height, hash);
        return nullptr;
    }

    // Insert into RTXTextureInfo cache
    {
        std::lock_guard<std::mutex> lock(mCacheMutex);
        auto result = mTextureCache.emplace(hash, std::move(texture));
        SPDLOG_DEBUG("[RTX] TextureManager::GetOrCreateTexture(hash,rgba): loaded texture "
                     "{}x{} at SRV index {} (hash: 0x{:016X})",
                     width, height, result.first->second.srvIndex, hash);
        return &result.first->second;
    }
}

// ============================================================================
// GetDefaultWhiteTextureSRV — Task-spec Phase 6: fallback 1x1 white texture SRV
// ============================================================================

D3D12_GPU_DESCRIPTOR_HANDLE TextureManager::GetDefaultWhiteTextureSRV() {
    if (!mInitialized || !mDefaultWhiteCreated) {
        D3D12_GPU_DESCRIPTOR_HANDLE nullHandle = {};
        nullHandle.ptr = 0;
        return nullHandle;
    }
    return mDefaultWhiteTexture.srvGpuHandle;
}

// ============================================================================
// EvictUnusedTextures (no-arg) — Task-spec Phase 6: cache management.
// Uses the internal frame counter and a default max age of 300 frames.
// Iterates the RTXTextureInfo cache and removes entries where isBound == false
// for longer than maxAge frames.
// ============================================================================

void TextureManager::EvictUnusedTextures() {
    // Default eviction parameters
    constexpr uint64_t DEFAULT_MAX_AGE = 300; // ~5 seconds at 60fps

    std::lock_guard<std::mutex> lock(mCacheMutex);

    uint64_t threshold = (mCurrentFrame > DEFAULT_MAX_AGE) ? (mCurrentFrame - DEFAULT_MAX_AGE) : 0;
    uint32_t evictedCount = 0;

    // Evict from the RTXTextureInfo cache (mTextureCache)
    // Remove entries where isBound == false and haven't been used recently
    for (auto it = mTextureCache.begin(); it != mTextureCache.end(); ) {
        if (!it->second.isBound) {
            // Check if we have frame tracking in the LRU or entry caches for this hash
            // Since RTXTextureInfo doesn't have a lastUsedFrame field, we use isBound as
            // the primary eviction criterion: if not bound for DEFAULT_MAX_AGE frames
            // (tracked via the frame counter when isBound was last set to false).
            // For simplicity, we evict all unbound textures during this call.
            uint32_t freedIndex = it->second.srvIndex;
            if (freedIndex >= RESERVED_SRV_SLOTS) {
                mFreeSRVIndices.push_back(freedIndex);
            }

            SPDLOG_DEBUG("[RTX] TextureManager::EvictUnusedTextures: evicting texture "
                         "(hash: 0x{:016X}, SRV index: {})", it->second.hash, freedIndex);

            it = mTextureCache.erase(it);
            evictedCount++;
        } else {
            // Mark as not bound for next frame's eviction pass.
            // Textures that are actively used will have isBound set back to true
            // by GetOrCreateTexture when they are accessed again.
            it->second.isBound = false;
            ++it;
        }
    }

    // Also delegate to the frame-based eviction for other caches
    // (RTXTextureEntry cache, LRU cache, TextureCacheEntry cache)
    // These use lastUsedFrame-based eviction.
    if (mCurrentFrame > DEFAULT_MAX_AGE) {
        // Evict from RTXTextureEntry cache
        for (auto it = mTextureEntryCache.begin(); it != mTextureEntryCache.end(); ) {
            if (it->second.lastUsedFrame < threshold) {
                uint32_t freedIndex = it->second.srvIndex;
                if (freedIndex >= RESERVED_SRV_SLOTS) {
                    mFreeSRVIndices.push_back(freedIndex);
                }
                it = mTextureEntryCache.erase(it);
                evictedCount++;
            } else {
                ++it;
            }
        }

        // Evict from LRU cache
        for (auto it = mLRUCache.begin(); it != mLRUCache.end(); ) {
            if (it->second.texture.lastUsedFrame < threshold) {
                uint32_t freedIndex = it->second.texture.srvIndex;
                if (freedIndex >= RESERVED_SRV_SLOTS) {
                    mFreeSRVIndices.push_back(freedIndex);
                }
                mLRUList.erase(it->second.lruIter);
                it = mLRUCache.erase(it);
                evictedCount++;
            } else {
                ++it;
            }
        }

        // Evict from TextureCacheEntry cache
        for (auto it = mTextureCacheEntries.begin(); it != mTextureCacheEntries.end(); ) {
            if (it->second.lastUsedFrame < threshold) {
                it = mTextureCacheEntries.erase(it);
                evictedCount++;
            } else {
                ++it;
            }
        }
    }

    if (evictedCount > 0) {
        SPDLOG_INFO("[RTX] TextureManager::EvictUnusedTextures: evicted {} textures "
                     "(frame: {}, max age: {})", evictedCount, mCurrentFrame, DEFAULT_MAX_AGE);
    }
}

} // namespace RTX

#endif // ENABLE_DX12_RTX
