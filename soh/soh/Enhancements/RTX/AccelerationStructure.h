#pragma once
#ifndef ACCELERATION_STRUCTURE_H
#define ACCELERATION_STRUCTURE_H

#ifdef ENABLE_DX12_RTX

#include "DX12Device.h"
#include "RTXTypes.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <vector>
#include <unordered_map>

using Microsoft::WRL::ComPtr;

namespace RTX {

class AccelerationStructure {
public:
    AccelerationStructure();
    ~AccelerationStructure();

    bool Initialize(DX12Device* device);
    void Shutdown();

    // Build BLAS for a room (called at room load time)
    bool BuildBLAS(const RoomGeometry& geometry);

    // Remove BLAS for a room
    void RemoveBLAS(uint32_t roomIndex);

    // Rebuild TLAS with all current room instances
    // Must be called each frame before DispatchRays
    void RebuildTLAS(ID3D12GraphicsCommandList4* cmdList);

    // Get TLAS GPU virtual address for binding to root signature
    D3D12_GPU_VIRTUAL_ADDRESS GetTLASAddress() const;

    // Per-geometry buffer info for shader table building
    struct GeometryBufferInfo {
        D3D12_GPU_VIRTUAL_ADDRESS vertexBufferAddress;
        D3D12_GPU_VIRTUAL_ADDRESS indexBufferAddress;
        D3D12_GPU_VIRTUAL_ADDRESS materialIDBufferAddress;
        D3D12_GPU_VIRTUAL_ADDRESS materialTableAddress;
    };

    // Get buffer info for all geometries (for building hit group shader table)
    const std::vector<GeometryBufferInfo>& GetGeometryBuffers() const;

    // Get number of loaded BLAS instances
    uint32_t GetInstanceCount() const;

    bool HasTLAS() const;

    void ReleaseAll();

    // Re-upload material buffer data for a room geometry.
    // Called when material textureIndex values have been updated after initial BLAS build
    // (e.g., after texture re-resolution).
    void UpdateMaterialBuffers(const RoomGeometry& geometry);

private:
    // Helper to create a GPU buffer
    ComPtr<ID3D12Resource> CreateBuffer(size_t size, D3D12_RESOURCE_FLAGS flags,
                                        D3D12_RESOURCE_STATES initialState,
                                        D3D12_HEAP_TYPE heapType);

    // Helper to upload CPU data to GPU default heap buffer
    ComPtr<ID3D12Resource> UploadBuffer(const void* data, size_t size,
                                        D3D12_RESOURCE_STATES afterState);

    // Execute a command list and wait (for one-shot operations like BLAS build)
    void ExecuteAndWait(ID3D12GraphicsCommandList4* cmdList);

    // Rebuild the flat geometry buffer info list from current BLAS map
    void RebuildGeometryBufferList();

    DX12Device* m_device = nullptr;

    // Dedicated command allocator for BLAS builds (separate from frame rendering)
    ComPtr<ID3D12CommandAllocator> m_buildCommandAllocator;
    ComPtr<ID3D12GraphicsCommandList4> m_buildCommandList;

    // Per-room BLAS data
    struct BLASData {
        uint32_t roomIndex;
        ComPtr<ID3D12Resource> blasResult;

        // GPU buffers for opaque geometry data
        ComPtr<ID3D12Resource> opaqueVertexBuffer;
        ComPtr<ID3D12Resource> opaqueIndexBuffer;
        ComPtr<ID3D12Resource> opaqueMaterialIDBuffer;
        ComPtr<ID3D12Resource> opaqueMaterialBuffer;

        // GPU buffers for alpha-tested geometry data
        ComPtr<ID3D12Resource> alphaVertexBuffer;
        ComPtr<ID3D12Resource> alphaIndexBuffer;
        ComPtr<ID3D12Resource> alphaMaterialIDBuffer;
        ComPtr<ID3D12Resource> alphaMaterialBuffer;

        uint32_t numGeometries = 0; // 1 (opaque only) or 2 (opaque + alpha)
        bool hasOpaqueGeometry = false;
        bool hasAlphaGeometry = false;
    };

    std::unordered_map<uint32_t, BLASData> m_blasMap; // key: roomIndex
    std::vector<GeometryBufferInfo> m_geometryBuffers;

    // TLAS data
    ComPtr<ID3D12Resource> m_tlasResult;
    ComPtr<ID3D12Resource> m_tlasScratch;
    ComPtr<ID3D12Resource> m_instanceBuffer; // Upload heap, persistently mapped
    void* m_instanceBufferMapped = nullptr;
    bool m_tlasBuilt = false;

    static const uint32_t MAX_INSTANCES = 16; // 3 rooms + future actors
    static const uint32_t MAX_GEOMETRIES_PER_INSTANCE = 2; // opaque + alpha
};

} // namespace RTX

#endif // ENABLE_DX12_RTX
#endif // ACCELERATION_STRUCTURE_H
