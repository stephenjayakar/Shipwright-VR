#pragma once
#ifndef TEXTURE_MANAGER_H
#define TEXTURE_MANAGER_H

#ifdef ENABLE_DX12_RTX

#include "RTXTypes.h"
#include "DX12Context.h"
#include "DX12Device.h"
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdint>
#include <cstring>
#include <vector>
#include <list>
#include <unordered_map>
#include <mutex>

using Microsoft::WRL::ComPtr;

namespace RTX {

// ---- Task-spec Phase 6 RTXTextureEntry ----
// The exact struct specified by RTX_PLAN.md Phase 6 task spec.
// Contains the GPU texture resource, SRV descriptor handles, dimensions,
// format, hash key, and LRU frame tracking.
struct RTXTextureEntry {
    // --- Task-spec Phase 6 required fields ---
    // ComPtr<ID3D12Resource> texture — accessed via gpuTexture member (same underlying storage)
    // D3D12_GPU_DESCRIPTOR_HANDLE srvGpuHandle — accessed via srvGPU member (same underlying storage)
    // uint32_t width, height — direct members
    // uint64_t n64Hash — accessed via hash member (same underlying storage)
    // bool isDirty — direct member

    uint64_t                    hash;           // FNV-1a hash of source texel data (also serves as n64Hash)
    uint32_t                    width;          // Texture width in texels
    uint32_t                    height;         // Texture height in texels
    DXGI_FORMAT                 format;         // Texture format (DXGI_FORMAT_R8G8B8A8_UNORM for decoded N64 textures)
    ComPtr<ID3D12Resource>      gpuTexture;     // GPU texture resource (default heap) — also serves as task-spec "texture"
    D3D12_CPU_DESCRIPTOR_HANDLE srvCPU;         // CPU descriptor handle for SRV creation
    D3D12_GPU_DESCRIPTOR_HANDLE srvGPU;         // GPU descriptor handle for shader binding — also serves as task-spec "srvGpuHandle"
    uint32_t                    srvIndex;       // Index into the SRV descriptor heap
    uint64_t                    lastUsedFrame;  // Frame number when last accessed (for eviction)
    bool                        isDirty;        // True if texture data needs re-upload — task-spec Phase 6 field

    RTXTextureEntry()
        : hash(0), width(0), height(0), format(DXGI_FORMAT_UNKNOWN)
        , srvIndex(0), lastUsedFrame(0), isDirty(false) {
        srvCPU.ptr = 0;
        srvGPU.ptr = 0;
    }

    // --- Task-spec Phase 6 accessors (canonical names) ---
    ComPtr<ID3D12Resource>&             texture()       { return gpuTexture; }
    const ComPtr<ID3D12Resource>&       texture() const { return gpuTexture; }
    D3D12_GPU_DESCRIPTOR_HANDLE&        srvGpuHandle()       { return srvGPU; }
    const D3D12_GPU_DESCRIPTOR_HANDLE&  srvGpuHandle() const { return srvGPU; }
    uint64_t&                           n64Hash()       { return hash; }
    const uint64_t&                     n64Hash() const { return hash; }

    // --- Convenience accessors ---
    ID3D12Resource* GetTexture() const { return gpuTexture.Get(); }
    D3D12_GPU_DESCRIPTOR_HANDLE GetSrvHandle() const { return srvGPU; }
};

// ---- Task-spec Phase 6 RTXTextureResource ----
// The canonical texture resource structure specified by RTX_PLAN.md Phase 6.
// Contains the GPU texture resource, optional upload buffer, SRV index,
// dimensions, format, and cache hash.
struct RTXTextureResource {
    ComPtr<ID3D12Resource> texture;       // GPU texture resource (default heap)
    ComPtr<ID3D12Resource> uploadBuffer;  // Upload heap staging buffer (may be released after upload)
    uint32_t               srvIndex;      // Index into the SRV descriptor heap
    uint32_t               width;         // Texture width in texels
    uint32_t               height;        // Texture height in texels
    DXGI_FORMAT            format;        // Texture format (DXGI_FORMAT_R8G8B8A8_UNORM for decoded N64 textures)
    uint64_t               hash;          // FNV-1a hash of source texel data + format parameters
};

// ---- Task-spec lightweight texture handle ----
// Returned by GetOrCreateTexture / UploadTexture / GetOrUploadTexture.
// Contains the SRV GPU descriptor handle and texture index needed for shader binding.
struct RTXTextureHandle {
    uint32_t                    srvIndex;       // Index into the SRV descriptor heap
    uint32_t                    textureIndex;   // Alias for srvIndex (task-spec name)
    uint32_t                    width;          // Texture width in texels
    uint32_t                    height;         // Texture height in texels
    D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle;      // GPU descriptor handle for shader binding

    bool IsValid() const { return gpuHandle.ptr != 0; }
};

// N64 texture image format constants (G_IM_FMT_*)
constexpr uint32_t G_IM_FMT_RGBA = 0;
constexpr uint32_t G_IM_FMT_YUV  = 1;
constexpr uint32_t G_IM_FMT_CI   = 2;
constexpr uint32_t G_IM_FMT_IA   = 3;
constexpr uint32_t G_IM_FMT_I    = 4;

// N64 texture size constants (G_IM_SIZ_*)
constexpr uint32_t G_IM_SIZ_4b   = 0;
constexpr uint32_t G_IM_SIZ_8b   = 1;
constexpr uint32_t G_IM_SIZ_16b  = 2;
constexpr uint32_t G_IM_SIZ_32b  = 3;

// CachedTexture — full texture entry with DX12 resources and LRU tracking.
// Stored in the texture cache and evicted when capacity is reached.
struct CachedTexture {
    uint64_t                     hashKey;        // Cache key (format + address + width + height + palette CRC)
    uint32_t                     width;
    uint32_t                     height;
    uint32_t                     srvIndex;       // Index into the SRV descriptor heap
    DXGI_FORMAT                  dxgiFormat;     // Always DXGI_FORMAT_R8G8B8A8_UNORM for decoded textures
    ComPtr<ID3D12Resource>       gpuTexture;     // The GPU texture resource (default heap)
    D3D12_GPU_DESCRIPTOR_HANDLE  srvGpuHandle;   // GPU descriptor handle for shader binding
    D3D12_CPU_DESCRIPTOR_HANDLE  srvCpuHandle;   // CPU descriptor handle for SRV creation
    uint64_t                     lastUsedFrame;  // Frame number when last accessed (for LRU eviction)
    bool                         uploaded;        // True once data has been uploaded to GPU
};

// Extended texture info struct with hash tracking and upload state.
// Task-spec Phase 6 fields: hash, width, height, format, gpuTexture, srvGpuHandle, isBound.
// Also provides backward-compatible srvHandle accessor and uploaded flag.
struct RTXTextureInfo {
    uint64_t                     hash;          // FNV-1a hash of source texel data
    uint32_t                     width;
    uint32_t                     height;
    uint32_t                     srvIndex;      // Index into the SRV descriptor heap (for Material::textureIndex)
    DXGI_FORMAT                  format;        // Always DXGI_FORMAT_R8G8B8A8_UNORM for decoded textures
    ComPtr<ID3D12Resource>       gpuTexture;    // The GPU texture resource (default heap)
    D3D12_GPU_DESCRIPTOR_HANDLE  srvGpuHandle;  // GPU descriptor handle for shader binding (task-spec Phase 6 name)
    bool                         isBound;       // True if texture is currently bound for rendering (task-spec Phase 6)
    bool                         uploaded;       // True once data has been uploaded to GPU

    // Default constructor
    RTXTextureInfo()
        : hash(0), width(0), height(0), srvIndex(0)
        , format(DXGI_FORMAT_UNKNOWN), isBound(false), uploaded(false) {
        srvGpuHandle.ptr = 0;
    }

    // Copy constructor (ComPtr handles ref counting)
    RTXTextureInfo(const RTXTextureInfo& other)
        : hash(other.hash), width(other.width), height(other.height)
        , srvIndex(other.srvIndex), format(other.format)
        , gpuTexture(other.gpuTexture), srvGpuHandle(other.srvGpuHandle)
        , isBound(other.isBound), uploaded(other.uploaded) {}

    // Move constructor
    RTXTextureInfo(RTXTextureInfo&& other) noexcept
        : hash(other.hash), width(other.width), height(other.height)
        , srvIndex(other.srvIndex), format(other.format)
        , gpuTexture(std::move(other.gpuTexture)), srvGpuHandle(other.srvGpuHandle)
        , isBound(other.isBound), uploaded(other.uploaded) {}

    // Copy assignment
    RTXTextureInfo& operator=(const RTXTextureInfo& other) {
        if (this != &other) {
            hash = other.hash; width = other.width; height = other.height;
            srvIndex = other.srvIndex; format = other.format;
            gpuTexture = other.gpuTexture; srvGpuHandle = other.srvGpuHandle;
            isBound = other.isBound; uploaded = other.uploaded;
        }
        return *this;
    }

    // Move assignment
    RTXTextureInfo& operator=(RTXTextureInfo&& other) noexcept {
        if (this != &other) {
            hash = other.hash; width = other.width; height = other.height;
            srvIndex = other.srvIndex; format = other.format;
            gpuTexture = std::move(other.gpuTexture); srvGpuHandle = other.srvGpuHandle;
            isBound = other.isBound; uploaded = other.uploaded;
        }
        return *this;
    }

    // Backward compatibility accessors: srvHandle is an alias for srvGpuHandle
    D3D12_GPU_DESCRIPTOR_HANDLE GetSrvHandle() const { return srvGpuHandle; }
    void SetSrvHandle(D3D12_GPU_DESCRIPTOR_HANDLE h) { srvGpuHandle = h; }
};

// TextureCacheEntry — Phase 6 task-spec struct for texture cache entries.
// Holds the D3D12 resource, SRV handle, dimensions, and LRU frame tracking.
// The returned SRV handles are compatible with RTXMaterial::diffuseTextureSRV
// and RTXMaterial::normalMapSRV fields.
struct TextureCacheEntry {
    ComPtr<ID3D12Resource>       resource;       // GPU texture resource (default heap)
    D3D12_GPU_DESCRIPTOR_HANDLE  srvHandle;      // GPU descriptor handle for shader binding
    uint32_t                     width;          // Texture width in texels
    uint32_t                     height;         // Texture height in texels
    uint64_t                     lastUsedFrame;  // Frame number when last accessed (for LRU eviction)

    TextureCacheEntry()
        : width(0), height(0), lastUsedFrame(0) {
        srvHandle.ptr = 0;
    }
};

// RTXTexture — matches the task spec fields exactly.
// Contains the GPU texture resource, SRV descriptor handles, dimensions, format, and hash.
// Used by the primary GetOrCreateTexture API and GetDefaultWhiteTexture.
//
// Task-spec Phase 6 canonical fields:
//   resource, srvHandle, width, height, format, n64Hash
struct RTXTexture {
    ComPtr<ID3D12Resource>       resource;      // The GPU texture resource (ID3D12Resource*)
    D3D12_GPU_DESCRIPTOR_HANDLE  srvHandle;     // GPU descriptor handle for shader binding (task-spec Phase 6 name)
    uint32_t                     width;
    uint32_t                     height;
    DXGI_FORMAT                  format;        // Always DXGI_FORMAT_R8G8B8A8_UNORM for decoded textures
    uint64_t                     n64Hash;       // FNV-1a hash of source texel data (task-spec Phase 6 name)

    // Additional fields for internal use
    D3D12_CPU_DESCRIPTOR_HANDLE  srvCPU;        // CPU descriptor handle for SRV creation
    uint32_t                     descriptorIndex; // Index into the SRV descriptor heap

    RTXTexture()
        : width(0), height(0), format(DXGI_FORMAT_UNKNOWN), n64Hash(0), descriptorIndex(0) {
        srvHandle.ptr = 0;
        srvCPU.ptr = 0;
    }

    // Copy constructor (ComPtr handles ref counting)
    RTXTexture(const RTXTexture& other)
        : resource(other.resource)
        , srvHandle(other.srvHandle), srvCPU(other.srvCPU)
        , width(other.width), height(other.height)
        , format(other.format), n64Hash(other.n64Hash)
        , descriptorIndex(other.descriptorIndex) {}

    // Move constructor
    RTXTexture(RTXTexture&& other) noexcept
        : resource(std::move(other.resource))
        , srvHandle(other.srvHandle), srvCPU(other.srvCPU)
        , width(other.width), height(other.height)
        , format(other.format), n64Hash(other.n64Hash)
        , descriptorIndex(other.descriptorIndex) {}

    // Copy assignment
    RTXTexture& operator=(const RTXTexture& other) {
        if (this != &other) {
            n64Hash = other.n64Hash; resource = other.resource;
            srvCPU = other.srvCPU; srvHandle = other.srvHandle;
            width = other.width; height = other.height;
            format = other.format; descriptorIndex = other.descriptorIndex;
        }
        return *this;
    }

    // Move assignment
    RTXTexture& operator=(RTXTexture&& other) noexcept {
        if (this != &other) {
            n64Hash = other.n64Hash; resource = std::move(other.resource);
            srvCPU = other.srvCPU; srvHandle = other.srvHandle;
            width = other.width; height = other.height;
            format = other.format; descriptorIndex = other.descriptorIndex;
        }
        return *this;
    }

    // Backward-compatible accessor: GetSrvHandle returns srvHandle
    D3D12_GPU_DESCRIPTOR_HANDLE GetSrvHandle() const { return srvHandle; }
};

class TextureManager {
public:
    // Singleton accessor (primary name)
    static TextureManager& GetInstance();

    // Singleton accessor (task-spec Phase 6 name)
    static TextureManager& Instance() { return GetInstance(); }

    // Singleton accessor (task-spec alias for GetInstance)
    static TextureManager& Get() { return GetInstance(); }

    // Default max textures in the SRV descriptor heap.
    // Uses the shared MAX_BINDLESS_TEXTURES constant from RTXTypes.h to stay
    // in sync with the DXRPipeline root signature descriptor range.
    static constexpr uint32_t MAX_TEXTURES = MAX_BINDLESS_TEXTURES;

    // Default LRU cache capacity (configurable via SetMaxCachedTextures)
    static constexpr uint32_t DEFAULT_MAX_CACHED_TEXTURES = 1024;

    TextureManager();
    ~TextureManager();

    // ---- Lifecycle ----

    // Initialize using DX12Device context (creates internal SRV heap, command list).
    // Must be called after DX12Device is initialized.
    // Returns true on success, false on failure.
    bool Initialize(DX12Device* context);

    // Initialize with explicit device, heap, and heap offset (alternative entry point).
    // Allows caller to provide an existing SRV descriptor heap and starting offset.
    bool Initialize(ID3D12Device* device, ID3D12DescriptorHeap* srvHeap, UINT heapOffset);

    // Initialize with ID3D12Device and max textures (task-spec Phase 6 entry point).
    // Creates an internal shader-visible SRV descriptor heap of size maxTextures,
    // a dedicated command queue/list for texture uploads, and an upload fence.
    // This is the exact signature specified by the Phase 6 task spec.
    bool Initialize(ID3D12Device* device, uint32_t maxTextures);

    // Initialize with ID3D12Device5 and max textures (DXR-specific entry point).
    // Creates an internal SRV heap of size maxTextures, command list, and upload fence.
    bool Initialize(ID3D12Device5* device, uint32_t maxTextures = 4096);

    // Initialize with ID3D12Device5 and an existing SRV descriptor heap (task-spec Phase 6 signature).
    // Uses the caller-provided SRV heap starting at heapStartIndex.
    // Does NOT create an internal heap.
    bool Initialize(ID3D12Device5* device, ID3D12DescriptorHeap* srvHeap, UINT heapStartIndex);

    // Initialize with ID3D12Device5 and an external command list (task-spec signature).
    // Creates an internal SRV heap (~4096), stores device and command list pointers.
    void Initialize(ID3D12Device5* device, ID3D12GraphicsCommandList4* cmdList);

    // Shut down and release all GPU resources (heap, command list, cache).
    void Shutdown();

    // ---- Task-spec Phase 6 primary API (RTXTextureInfo) ----

    // Task-spec Phase 6 main entry point: look up or create a texture from pre-decoded RGBA data.
    // If the hash is already in the cache, returns the cached entry.
    // If not, creates a committed resource (D3D12_HEAP_TYPE_DEFAULT), uploads RGBA data
    // via a staging upload buffer, creates an SRV, and stores in cache.
    //
    // Parameters:
    //   hash     - Pre-computed hash key for cache lookup
    //   rgbaData - Pre-decoded RGBA8 pixel data (4 bytes per pixel)
    //   width    - Texture width in texels
    //   height   - Texture height in texels
    //
    // Returns RTXTextureInfo* on success, nullptr on failure.
    RTXTextureInfo* GetOrCreateTexture(uint64_t hash, const uint8_t* rgbaData,
                                       uint32_t width, uint32_t height);

    // Task-spec Phase 6 primary API: look up or create a texture from pre-decoded RGBA32 data.
    // Returns D3D12_GPU_DESCRIPTOR_HANDLE for the texture SRV.
    // If the hash is already in the cache, returns the cached handle.
    // If not, creates a committed resource, uploads RGBA data, creates SRV, caches, and returns handle.
    // On failure returns the default white texture handle, or a null handle (ptr==0).
    //
    // This wraps GetOrCreateTexture(hash, rgba, w, h) -> RTXTextureInfo* and extracts the handle.
    // Named differently to avoid C++ overload-by-return-type ambiguity.
    D3D12_GPU_DESCRIPTOR_HANDLE GetOrCreateTextureGPUHandle(uint64_t hash, const uint8_t* rgba32Data,
                                                             uint32_t width, uint32_t height);

    // Task-spec Phase 6: Invalidate a single cached texture by hash.
    // Removes the texture from all caches and reclaims the SRV descriptor slot.
    void InvalidateTexture(uint64_t hash);

    // Task-spec Phase 6: Get the default 1x1 white texture SRV GPU descriptor handle.
    // Returns a valid handle if initialized, or a null handle (ptr==0) otherwise.
    D3D12_GPU_DESCRIPTOR_HANDLE GetDefaultWhiteTextureSRV();

    // Task-spec Phase 6: Evict unused textures (no-arg version).
    // Uses the internal frame counter (advanced by AdvanceFrame()) and a default
    // max age of 300 frames (~5 seconds at 60fps). Textures where isBound==false
    // for longer than maxAge frames are removed from the cache.
    void EvictUnusedTextures();

    // ---- Phase 6 Task-Spec API (N64TextureFormat enum, D3D12_GPU_DESCRIPTOR_HANDLE return) ----

    // Note: Initialize(DX12Context*) is available because DX12Context is a type alias
    // for DX12Device (see DX12Context.h). Call Initialize(DX12Device*) directly.

    // Primary Phase 6 entry point: look up or create a texture from raw N64 data.
    // Uses the N64TextureFormat enum for cleaner format specification.
    //
    // Parameters:
    //   tmemAddr  - Address/identifier for the texture in N64 TMEM (used in cache key)
    //   format    - N64TextureFormat enum value (RGBA16, RGBA32, CI4, CI8, IA4, IA8, I4, I8)
    //   width     - Texture width in texels
    //   height    - Texture height in texels
    //   rawData   - Raw N64-format texel data
    //   dataSize  - Size of rawData in bytes
    //   palette   - Palette data for CI4/CI8 formats (RGBA16 entries), nullptr otherwise
    //
    // Returns D3D12_GPU_DESCRIPTOR_HANDLE for the texture SRV.
    // The returned handle is compatible with RTXMaterial::diffuseTextureSRV and
    // RTXMaterial::normalMapSRV fields.
    // On failure, returns the default white texture handle.
    D3D12_GPU_DESCRIPTOR_HANDLE GetOrCreateTexture(uint64_t tmemAddr,
                                                    N64TextureFormat format,
                                                    uint32_t width, uint32_t height,
                                                    const uint8_t* rawData, size_t dataSize,
                                                    const uint8_t* palette = nullptr);

    // Create both default textures: 1x1 white (at SRV index 0) and
    // 1x1 flat normal map (128,128,255,255) for surfaces without a normal map.
    // Called automatically during Initialize, but can be called manually.
    void CreateDefaultTextures();

    // Get the default 1x1 flat normal map GPU descriptor handle.
    // Normal map stores (128,128,255,255) which decodes to (0,0,1) in tangent space.
    // Compatible with RTXMaterial::normalMapSRV.
    D3D12_GPU_DESCRIPTOR_HANDLE GetDefaultNormalMap();

    // Get the default 1x1 flat normal map texture info.
    RTXTextureInfo* GetDefaultNormalMapTexture();

    // Convert N64TextureFormat enum to (imgFmt, imgSiz) pair for use with ConvertN64Texture.
    static void N64TextureFormatToFmtSiz(N64TextureFormat format, uint32_t& outFmt, uint32_t& outSiz);

    // ---- N64 Texture Conversion ----

    // Convert raw N64 texture data to RGBA32 (8 bits per channel).
    // Parameters:
    //   tmem   - Raw N64-format texel data (from TMEM)
    //   fmt    - N64 image format (G_IM_FMT_RGBA, G_IM_FMT_CI, etc.)
    //   siz    - N64 image size (G_IM_SIZ_4b, G_IM_SIZ_8b, G_IM_SIZ_16b, G_IM_SIZ_32b)
    //   width  - Texture width in texels
    //   height - Texture height in texels
    //   tlut       - Palette data for CI4/CI8 formats (RGBA16 entries), nullptr otherwise
    //   tlutFormat - TLUT pixel format (typically G_IM_SIZ_16b for RGBA16 palette entries)
    // Returns RGBA32 pixel data (4 bytes per pixel: R, G, B, A).
    static std::vector<uint8_t> ConvertN64Texture(const uint8_t* tmem,
                                                   uint32_t fmt, uint32_t siz,
                                                   uint32_t width, uint32_t height,
                                                   const uint8_t* tlut = nullptr,
                                                   uint32_t tlutFormat = G_IM_SIZ_16b);

    // ---- Primary Texture Cache API (returns RTXTextureHandle) ----

    // Task-spec primary entry point: look up or create a texture from raw N64 data.
    // Uses a hash key composed of (format + address + width + height + paletteCRC).
    //
    // Parameters:
    //   textureData - Raw N64-format texel data
    //   format      - Packed N64 format: (G_IM_FMT << 4) | G_IM_SIZ
    //   width       - Texture width in texels
    //   height      - Texture height in texels
    //   palette     - Palette data for CI4/CI8 formats, nullptr otherwise
    //   paletteCRC  - CRC of the palette data (used in cache key), 0 if no palette
    //
    // Returns RTXTextureHandle with SRV GPU descriptor handle and texture index.
    // On failure, returns a handle pointing to the fallback checkerboard texture.
    RTXTextureHandle GetOrCreateTexture(const uint8_t* textureData,
                                        uint32_t format,
                                        uint32_t width, uint32_t height,
                                        const uint8_t* palette = nullptr,
                                        uint32_t paletteCRC = 0);

    // ---- Phase 6 Task-spec API (RTXTextureResource) ----

    // Look up or create a texture from raw N64 TMEM data. Returns a pointer to the
    // cached RTXTextureResource, or nullptr on failure. The returned pointer is valid
    // until InvalidateCache() is called or the TextureManager is destroyed.
    //
    // Parameters:
    //   tmemData  - Raw N64-format texel data (from TMEM)
    //   tmemSize  - Size of tmemData in bytes
    //   format    - N64 image format (G_IM_FMT_RGBA, G_IM_FMT_CI, etc.)
    //   size      - N64 image size (G_IM_SIZ_4b, G_IM_SIZ_8b, G_IM_SIZ_16b, G_IM_SIZ_32b)
    //   width     - Texture width in texels
    //   height    - Texture height in texels
    //   palette   - Palette data for CI4/CI8 formats (RGBA16 entries), nullptr otherwise
    RTXTextureResource* GetOrCreateTexture(const uint8_t* tmemData, uint32_t tmemSize,
                                           uint32_t format, uint32_t size,
                                           uint32_t width, uint32_t height,
                                           const uint8_t* palette);

    // Get the SRV descriptor heap (task-spec Phase 6 naming convention).
    // Returns the shader-visible descriptor heap used for bindless texture access.
    ID3D12DescriptorHeap* GetDescriptorHeap() const { return GetSRVHeap(); }

    // Look up a cached texture by its hash key.
    // Returns a pointer to the RTXTextureResource if found, nullptr otherwise.
    RTXTextureResource* GetTextureByHash(uint64_t hash);

    // ---- Additional Cache Entry Points ----

    // Task-spec entry point: check cache by hashKey, if miss decode N64 texels into RGBA8,
    // create DEFAULT heap texture, upload via upload heap, create SRV, cache and return.
    RTXTexture* GetOrConvertTexture(uint64_t hashKey, const uint8_t* rawData,
                                    uint32_t width, uint32_t height,
                                    uint32_t n64Format, uint32_t n64Size,
                                    const uint8_t* palette = nullptr);

    // Main entry point (RTXTextureInfo version): check cache by hash, decode N64 data if
    // missing, upload to GPU.
    RTXTextureInfo* GetOrCreateTextureInfo(uint64_t hash,
                                           const uint8_t* tmem,
                                           uint32_t fmt, uint32_t siz,
                                           uint32_t width, uint32_t height,
                                           const uint8_t* tlut = nullptr,
                                           uint32_t tlutFormat = G_IM_SIZ_16b);

    // Task-spec convenience overload: auto-computes hash from raw data, extracts
    // format/size from a packed format field ((G_IM_FMT << 4) | G_IM_SIZ).
    RTXTextureInfo* GetOrCreateTextureInfo(const uint8_t* data, uint32_t size,
                                           uint32_t width, uint32_t height,
                                           uint32_t format,
                                           const uint8_t* palette = nullptr);

    // Legacy entry point (RTXTexture version, packed format): for backward compatibility.
    RTXTexture* GetOrCreateTextureLegacy(uint64_t hash,
                                         const uint8_t* data,
                                         uint32_t width, uint32_t height,
                                         uint32_t format,
                                         const uint8_t* tlut = nullptr,
                                         uint32_t tlutFormat = G_IM_SIZ_16b);

    // Upload a texture's RGBA data to the GPU and create its SRV descriptor.
    void UploadToGPU(RTXTextureInfo& tex, const uint8_t* rgbaData,
                     ID3D12GraphicsCommandList* cmdList = nullptr);

    // ---- Task-spec convenience API (RTXTextureHandle) ----

    // Upload pre-decoded RGBA8 data to the GPU and return a lightweight handle.
    RTXTextureHandle UploadTexture(const uint8_t* rgbaData, uint32_t width, uint32_t height, uint64_t hash);

    // Check the cache for a texture with the given hash; if not found, upload it.
    RTXTextureHandle GetOrUploadTexture(const uint8_t* rgbaData, uint32_t w, uint32_t h, uint64_t hash);

    // Get the GPU descriptor heap (alias for GetSRVHeap, matches task spec naming).
    ID3D12DescriptorHeap* GetGPUDescriptorHeap() const { return GetSRVHeap(); }

    // ---- Task-spec Phase 6 primary entry point: GetOrCreateTexture returning RTXTexture* ----
    // Look up or create a texture from raw N64 data using pre-computed hash key.
    // This is the exact signature specified by the Phase 6 task spec.
    // Parameters:
    //   hash       - Pre-computed hash key for the texture (cache lookup key)
    //   data       - Raw N64-format texel data
    //   width      - Texture width in texels
    //   height     - Texture height in texels
    //   n64Format  - N64 image format (G_IM_FMT_RGBA=0, G_IM_FMT_CI=2, G_IM_FMT_IA=3, G_IM_FMT_I=4)
    //   n64Size    - N64 image size (G_IM_SIZ_4b=0, G_IM_SIZ_8b=1, G_IM_SIZ_16b=2, G_IM_SIZ_32b=3)
    // Returns RTXTexture* on success, nullptr on failure.
    RTXTexture* GetOrCreateTexture(uint64_t hash, const uint8_t* data,
                                   uint32_t width, uint32_t height,
                                   uint32_t n64Format, uint32_t n64Size);

    // Look up or create a texture from raw N64 data. Hashes input data, returns
    // cached texture or creates a new one.
    // Parameters:
    //   data    - Raw N64-format texel data
    //   width   - Texture width in texels
    //   height  - Texture height in texels
    //   format  - Packed N64 format: (G_IM_FMT << 4) | G_IM_SIZ
    //   palette - Palette data for CI4/CI8 formats (RGBA16 entries), nullptr otherwise
    // Returns RTXTexture* on success, nullptr on failure.
    RTXTexture* GetOrCreateTexture(const uint8_t* data, uint32_t width, uint32_t height,
                                   uint32_t format, const uint8_t* palette = nullptr);

    // Overload with pre-computed hash key and packed format.
    RTXTexture* GetOrCreateTexture(uint64_t hash, const uint8_t* texData,
                                   uint32_t width, uint32_t height,
                                   uint32_t format);

    // ---- Task-spec: GetSRVHandle ----
    // Get the GPU descriptor handle for a cached texture by hash.
    // Searches all caches (RTXTextureEntry, LRU, legacy).
    // Returns the default white texture handle if not found.
    D3D12_GPU_DESCRIPTOR_HANDLE GetSRVHandle(uint64_t hash);

    // ---- Task-spec: GetBindlessHandle ----
    // Get the GPU descriptor handle for a cached texture by hash.
    // Returns a null handle (ptr==0) if the texture is not found.
    D3D12_GPU_DESCRIPTOR_HANDLE GetBindlessHandle(uint64_t hash);

    // ---- Task-spec: CreateSRVHeap ----
    // Create (or recreate) the shader-visible SRV descriptor heap with the given capacity.
    // Typically called from Initialize; can also be called standalone before texture loading.
    void CreateSRVHeap(uint32_t maxTextures);

    // ---- Task-spec: DecodeN64Texture ----
    // Decode raw N64 texture data to RGBA8 pixels. This is an alias for ConvertN64Texture
    // using the task-spec naming convention.
    // Parameters:
    //   tmem   - Raw N64-format texel data
    //   fmt    - N64 image format (G_IM_FMT_RGBA, G_IM_FMT_CI, etc.)
    //   siz    - N64 image size (G_IM_SIZ_4b, G_IM_SIZ_8b, G_IM_SIZ_16b, G_IM_SIZ_32b)
    //   width  - Texture width in texels
    //   height - Texture height in texels
    //   tlut       - Palette data for CI4/CI8 formats (RGBA16 entries), nullptr otherwise
    //   tlutFormat - TLUT pixel format (G_IM_SIZ_16b for RGBA16 palette entries)
    // Returns RGBA8 pixel data (4 bytes per pixel: R, G, B, A).
    static std::vector<uint8_t> DecodeN64Texture(const uint8_t* tmem,
                                                  uint32_t fmt, uint32_t siz,
                                                  uint32_t width, uint32_t height,
                                                  const uint8_t* tlut = nullptr,
                                                  uint32_t tlutFormat = G_IM_SIZ_16b);

    // ---- Default / Fallback Textures ----

    // Get the default 1x1 white texture GPU descriptor handle.
    // Returns the SRV GPU handle for the default white texture (task-spec method).
    D3D12_GPU_DESCRIPTOR_HANDLE GetDefaultTexture();

    // Get the default 1x1 white texture as RTXTextureInfo (created during Initialize).
    RTXTextureInfo* GetDefaultWhiteTexture();

    // Get the default 1x1 white texture as RTXTexture* (task-spec Phase 6 return type).
    // Returns RTXTexture* on success, nullptr if not initialized.
    RTXTexture* GetDefaultWhiteTextureRTX();

    // Get the default 1x1 white texture as RTXTextureEntry* (task-spec Phase 6 exact return type).
    // Returns RTXTextureEntry* on success, nullptr if not initialized.
    RTXTextureEntry* GetDefaultWhiteTextureEntry();

    // Get the default 8x8 magenta/black checkerboard fallback texture (created during Initialize).
    // Used when a texture fails to decode or is missing.
    RTXTextureHandle GetFallbackCheckerboardHandle() const;
    CachedTexture* GetFallbackCheckerboard();

    // ---- Task-spec Phase 6: RTXTextureEntry-based API ----

    // Look up or create a texture from raw N64 TMEM data. Returns a pointer to the
    // cached RTXTextureEntry, or nullptr on failure. The returned pointer is valid
    // until the texture is evicted or the TextureManager is destroyed.
    //
    // This is the exact signature specified by the Phase 6 task spec.
    //
    // Parameters:
    //   hash       - Pre-computed hash key for the texture (cache lookup key)
    //   tmemData   - Raw N64-format texel data (from TMEM)
    //   width      - Texture width in texels
    //   height     - Texture height in texels
    //   n64Format  - N64 image format (G_IM_FMT_RGBA, G_IM_FMT_CI, etc.)
    //   n64Size    - N64 image size (G_IM_SIZ_4b, G_IM_SIZ_8b, G_IM_SIZ_16b, G_IM_SIZ_32b)
    //   palette    - Palette data for CI4/CI8 formats (RGBA16 entries), nullptr otherwise
    RTXTextureEntry* GetOrCreateTexture(uint64_t hash,
                                        const uint8_t* tmemData,
                                        uint32_t width, uint32_t height,
                                        uint32_t n64Format, uint32_t n64Size,
                                        const uint8_t* palette);

    // ---- Task-spec Phase 6: Eviction API ----

    // Evict textures that have not been used for maxAge frames.
    // Iterates the RTXTextureEntry cache and removes entries whose
    // lastUsedFrame is older than (currentFrame - maxAge).
    // Also evicts from the LRU cache for unified management.
    void EvictUnusedTextures(uint64_t currentFrame, uint64_t maxAge);

    // ---- Cache Management ----

    // Set the maximum number of cached textures for LRU eviction.
    // Default is DEFAULT_MAX_CACHED_TEXTURES (1024).
    void SetMaxCachedTextures(uint32_t maxCached);

    // Get the current LRU cache capacity.
    uint32_t GetMaxCachedTextures() const { return mMaxCachedTextures; }

    // Invalidate and release all cached textures (keeps heap and default textures).
    void InvalidateCache();

    // Task-spec Phase 6: Invalidate all cached textures and release GPU resources.
    // Alias for InvalidateCache() — specified in the task spec as InvalidateAll().
    void InvalidateAll();

    // Release all cached textures and reset the SRV index counter.
    void ReleaseAll();

    // Alias for InvalidateCache().
    void ReleaseAllTextures() { InvalidateCache(); }

    // Reset all state: release textures, reset SRV index, keep heap alive.
    void Reset() { ReleaseAll(); }

    // Called once per frame to advance the internal frame counter (for LRU tracking).
    void AdvanceFrame();

    // ---- Hashing ----

    // Compute FNV-1a 64-bit hash of raw texture data.
    static uint64_t HashTextureData(const uint8_t* data, size_t size);

    // Compute a cache key from format, address (pointer), width, height, and palette CRC.
    // This is the primary cache key algorithm specified by the task.
    static uint64_t ComputeTextureHash(uint32_t format, uintptr_t address,
                                       uint32_t width, uint32_t height,
                                       uint32_t paletteCRC);

    // ---- Descriptor Heap Access ----

    // Get the SRV descriptor heap (shader-visible, for binding during raytracing dispatch).
    ID3D12DescriptorHeap* GetSRVHeap() const;

    // Get the GPU descriptor handle for the start of the SRV table.
    D3D12_GPU_DESCRIPTOR_HANDLE GetSRVTableStart() const;

    // Get the GPU handle at a specific SRV index.
    D3D12_GPU_DESCRIPTOR_HANDLE GetGPUHandle(uint32_t index) const;

    // Get the CPU handle at a specific SRV index.
    D3D12_CPU_DESCRIPTOR_HANDLE GetCPUHandle(uint32_t index) const;

    // Get the current texture count in the LRU cache.
    uint32_t GetTextureCount() const;

    // Get the next available SRV index (useful for pre-allocation checks).
    uint32_t GetNextSRVIndex() const;

    // Look up a cached texture by hash. Returns nullptr if not found.
    RTXTextureInfo* FindTexture(uint64_t hash);

    // Get SRV index for a cached texture by hash. Returns 0 (default white) if not found.
    uint32_t GetSRVIndexForHash(uint64_t hash) const;

    // Get the bindless texture index for a cached texture by hash.
    uint32_t GetTextureIndex(uint64_t hash) const { return GetSRVIndexForHash(hash); }

private:
    // ---- N64 format decoders: each converts raw N64 texel data to RGBA8888 ----
    static std::vector<uint8_t> DecodeRGBA16(const uint8_t* src, uint32_t width, uint32_t height);
    static std::vector<uint8_t> DecodeRGBA32(const uint8_t* src, uint32_t width, uint32_t height);
    static std::vector<uint8_t> DecodeIA4(const uint8_t* src, uint32_t width, uint32_t height);
    static std::vector<uint8_t> DecodeIA8(const uint8_t* src, uint32_t width, uint32_t height);
    static std::vector<uint8_t> DecodeIA16(const uint8_t* src, uint32_t width, uint32_t height);
    static std::vector<uint8_t> DecodeCI4(const uint8_t* src, uint32_t width, uint32_t height,
                                          const uint8_t* tlut, uint32_t tlutFormat);
    static std::vector<uint8_t> DecodeCI8(const uint8_t* src, uint32_t width, uint32_t height,
                                          const uint8_t* tlut, uint32_t tlutFormat);
    static std::vector<uint8_t> DecodeI4(const uint8_t* src, uint32_t width, uint32_t height);
    static std::vector<uint8_t> DecodeI8(const uint8_t* src, uint32_t width, uint32_t height);

    // Create a GPU texture resource from decoded RGBA8 data, create SRV at given srvIndex.
    // Returns true on success and populates the CachedTexture fields.
    bool CreateGPUTextureAtIndex(const uint8_t* rgbaData, uint32_t w, uint32_t h,
                                 uint32_t srvIndex, CachedTexture& outTexture);

    // Create a GPU texture resource from decoded RGBA8 data, create SRV at mNextDescriptorIndex.
    // Returns true on success and populates outTexture.
    bool CreateGPUTexture(const uint8_t* rgbaData, uint32_t w, uint32_t h, RTXTextureInfo& outTexture);

    // Legacy overload for RTXTexture
    bool CreateGPUTextureLegacy(const uint8_t* rgbaData, uint32_t w, uint32_t h, RTXTexture& outTexture);

    // Task-spec Phase 6: Upload decoded RGBA8 data to an existing GPU texture resource
    // via the ring upload buffer. Records copy commands on the upload command list and
    // transitions the resource to PIXEL_SHADER_RESOURCE | NON_PIXEL_SHADER_RESOURCE.
    // Parameters:
    //   destResource - The destination D3D12 texture resource (must be in COPY_DEST state)
    //   rgbaData     - Pre-decoded RGBA8 pixel data (4 bytes per pixel)
    //   width        - Texture width in texels
    //   height       - Texture height in texels
    // Returns true on success.
    bool UploadTextureData(ID3D12Resource* destResource, const uint8_t* rgbaData,
                           uint32_t width, uint32_t height);

    // Helper: create a committed buffer resource on upload heap.
    ComPtr<ID3D12Resource> CreateUploadBuffer(size_t size);

    // Helper: execute upload command list and wait for GPU.
    void ExecuteUploadAndWait();

    // Create the default 1x1 white texture at SRV index 0.
    void CreateDefaultWhiteTexture();

    // Create the 1x1 flat normal map texture (128,128,255,255) at SRV index 2.
    void CreateDefaultNormalMapTexture();

    // Create the 8x8 magenta/black checkerboard fallback texture at SRV index 1.
    void CreateCheckerboardFallbackTexture();

    // ---- LRU Eviction ----

    // Evict the least recently used texture from the cache to free an SRV slot.
    // Returns the freed SRV index, or UINT32_MAX if eviction failed.
    uint32_t EvictLRUTexture();

    // Allocate an SRV index: either use next free, or evict LRU to reclaim one.
    // Returns allocated SRV index, or UINT32_MAX on failure.
    uint32_t AllocateSRVIndex();

    // ---- Members ----

    // Singleton instance
    static TextureManager* s_instance;

    DX12Device*                                    mContext = nullptr;

    // Task-spec member: ID3D12Device5* (set when using the Device5 init path)
    ID3D12Device5*                                 mDevice = nullptr;

    // External command list (set via Initialize(Device5*, cmdList) task-spec path)
    ID3D12GraphicsCommandList4*                    mExternalCmdList = nullptr;

    // When initialized with explicit device/heap (non-DX12Device path)
    ID3D12Device*                                  mRawDevice = nullptr;
    ID3D12DescriptorHeap*                          mExternalSrvHeap = nullptr;

    ComPtr<ID3D12DescriptorHeap>                   mSrvHeap;         // Owned SRV heap
    uint32_t                                       mNextDescriptorIndex = 0;  // Next free SRV descriptor index (also referred to as mNextSrvIndex)
    uint32_t                                       mSrvDescriptorSize = 0;
    uint32_t                                       mDescriptorSize = 0; // Alias for mSrvDescriptorSize
    uint32_t                                       mHeapOffset = 0;    // Starting offset in external heap
    uint32_t                                       mMaxTextures = MAX_TEXTURES; // Max SRV heap entries

    // ---- LRU texture cache ----
    // The primary cache: maps hash key -> CachedTexture.
    // Also maintains an LRU list for eviction ordering.
    uint32_t                                       mMaxCachedTextures = DEFAULT_MAX_CACHED_TEXTURES;
    uint64_t                                       mCurrentFrame = 0;

    // LRU list: front = most recently used, back = least recently used.
    // Each element is a hash key into mLRUCache.
    std::list<uint64_t>                            mLRUList;

    // Map from hash key -> (CachedTexture, iterator into mLRUList)
    struct LRUCacheEntry {
        CachedTexture texture;
        std::list<uint64_t>::iterator lruIter;
    };
    std::unordered_map<uint64_t, LRUCacheEntry>   mLRUCache;

    // Free SRV indices reclaimed via eviction (reusable)
    std::vector<uint32_t>                          mFreeSRVIndices;

    // Phase 6 RTXTextureResource cache (GetOrCreateTexture tmem overload / GetTextureByHash)
    std::unordered_map<uint64_t, RTXTextureResource> mResourceCache;

    // Phase 6 RTXTextureEntry cache (task-spec GetOrCreateTexture hash overload)
    std::unordered_map<uint64_t, RTXTextureEntry> mTextureEntryCache;

    // Legacy texture caches (backward compatibility)
    std::unordered_map<uint64_t, RTXTextureInfo>   mTextureCache;
    std::unordered_map<uint64_t, RTXTexture>       mCache;  // RTXTexture cache (GetOrConvertTexture)

    // Task-spec Phase 6: Ring upload buffer for staging texture data uploads.
    // Allocated once during Initialize with a fixed size (default 4MB), reused across
    // texture uploads to avoid per-texture upload buffer creation overhead.
    // Falls back to per-texture CreateUploadBuffer if ring buffer space is insufficient.
    ComPtr<ID3D12Resource>                          mUploadBuffer;
    size_t                                          mUploadBufferSize = 0;
    size_t                                          mUploadBufferOffset = 0;
    static constexpr size_t UPLOAD_RING_BUFFER_SIZE = 4 * 1024 * 1024; // 4 MB default

    // Flat list of all GPU texture resources, for lifetime management
    std::vector<ComPtr<ID3D12Resource>>             mTextureResources;

    // Default white texture (always at SRV index 0)
    RTXTextureInfo                                 mDefaultWhiteTexture;
    RTXTexture                                     mDefaultWhiteRTXTexture;  // RTXTexture version for GetDefaultWhiteTextureRTX
    RTXTextureEntry                                mDefaultWhiteEntry;       // RTXTextureEntry version for GetDefaultWhiteTextureEntry
    bool                                           mDefaultWhiteCreated = false;

    // Default 1x1 flat normal map (128,128,255,255) — decodes to (0,0,1) in tangent space
    // Compatible with RTXMaterial::normalMapSRV. Created at SRV index 2.
    RTXTextureInfo                                 mDefaultNormalMapTexture;
    bool                                           mDefaultNormalMapCreated = false;

    // Default 8x8 magenta/black checkerboard fallback (always at SRV index 1)
    CachedTexture                                  mCheckerboardFallback;
    bool                                           mCheckerboardCreated = false;

    // Phase 6 TextureCacheEntry cache (keyed by hash of texture data)
    // Used by the N64TextureFormat-based GetOrCreateTexture overload.
    std::unordered_map<uint64_t, TextureCacheEntry> mTextureCacheEntries;

    // Number of reserved SRV indices (0 = white, 1 = checkerboard, 2 = flat normal)
    static constexpr uint32_t RESERVED_SRV_SLOTS = 3;

    // Dedicated command allocator and list for texture uploads
    ComPtr<ID3D12CommandAllocator>                 mUploadCmdAllocator;
    ComPtr<ID3D12GraphicsCommandList4>             mUploadCmdList;

    // For the explicit device path — need our own command queue for uploads
    ComPtr<ID3D12CommandQueue>                     mOwnedCommandQueue;
    ComPtr<ID3D12Fence>                            mUploadFence;
    HANDLE                                         mUploadFenceEvent = nullptr;
    uint64_t                                       mUploadFenceValue = 0;

    mutable std::mutex                             mCacheMutex;
    bool                                           mInitialized = false;
    bool                                           mUsingExternalHeap = false;
};

} // namespace RTX

// Provide TextureManager in the SOH::RTX namespace for Phase 6 task-spec compatibility.
namespace SOH {
namespace RTX {
    using TextureManager = ::RTX::TextureManager;
    using TextureCacheEntry = ::RTX::TextureCacheEntry;
    using CachedTexture = ::RTX::CachedTexture;
    using RTXTexture = ::RTX::RTXTexture;
    using RTXTextureHandle = ::RTX::RTXTextureHandle;
    using RTXTextureInfo = ::RTX::RTXTextureInfo;
    using RTXTextureResource = ::RTX::RTXTextureResource;
    using RTXTextureEntry = ::RTX::RTXTextureEntry;
} // namespace RTX
} // namespace SOH

#endif // ENABLE_DX12_RTX
#endif // TEXTURE_MANAGER_H
