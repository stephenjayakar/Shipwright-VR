#ifdef ENABLE_DX12_RTX

#include "AccelerationStructure.h"
#include <spdlog/spdlog.h>
#include <cassert>
#include <cstring>
#include <algorithm>

namespace RTX {

AccelerationStructure::AccelerationStructure() = default;

AccelerationStructure::~AccelerationStructure() {
    Shutdown();
}

bool AccelerationStructure::Initialize(DX12Device* device) {
    if (!device || !device->IsInitialized() || !device->SupportsRaytracing()) {
        SPDLOG_ERROR("[RTX] AccelerationStructure::Initialize - invalid device");
        return false;
    }

    m_device = device;
    auto* d3dDevice = m_device->GetDevice();

    // Create a dedicated command allocator and command list for BLAS builds.
    // These are separate from the per-frame command infrastructure so that
    // BLAS builds (which happen at room load time) don't interfere with
    // the frame rendering command list.
    HRESULT hr = d3dDevice->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(&m_buildCommandAllocator));
    if (FAILED(hr)) {
        SPDLOG_ERROR("[RTX] Failed to create BLAS build command allocator: 0x{:08X}",
                     static_cast<uint32_t>(hr));
        return false;
    }

    hr = d3dDevice->CreateCommandList(
        0,
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        m_buildCommandAllocator.Get(),
        nullptr,
        IID_PPV_ARGS(&m_buildCommandList));
    if (FAILED(hr)) {
        SPDLOG_ERROR("[RTX] Failed to create BLAS build command list: 0x{:08X}",
                     static_cast<uint32_t>(hr));
        return false;
    }

    // Close immediately — we'll reset before each use
    m_buildCommandList->Close();

    // Pre-allocate the TLAS instance buffer on the upload heap (persistently mapped).
    // Each D3D12_RAYTRACING_INSTANCE_DESC is 64 bytes.
    const size_t instanceBufferSize = MAX_INSTANCES * sizeof(D3D12_RAYTRACING_INSTANCE_DESC);
    m_instanceBuffer = CreateBuffer(
        instanceBufferSize,
        D3D12_RESOURCE_FLAG_NONE,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        D3D12_HEAP_TYPE_UPLOAD);
    if (!m_instanceBuffer) {
        SPDLOG_ERROR("[RTX] Failed to create TLAS instance buffer");
        return false;
    }

    // Persistently map the instance buffer
    hr = m_instanceBuffer->Map(0, nullptr, &m_instanceBufferMapped);
    if (FAILED(hr)) {
        SPDLOG_ERROR("[RTX] Failed to map TLAS instance buffer: 0x{:08X}",
                     static_cast<uint32_t>(hr));
        return false;
    }

    // Pre-allocate TLAS scratch and result buffers to the maximum expected size.
    // We query prebuild info with the max instance count so we allocate enough.
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tlasInputs = {};
    tlasInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    tlasInputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
    tlasInputs.NumDescs = MAX_INSTANCES;
    tlasInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO tlasPrebuild = {};
    d3dDevice->GetRaytracingAccelerationStructurePrebuildInfo(&tlasInputs, &tlasPrebuild);

    m_tlasScratch = CreateBuffer(
        tlasPrebuild.ScratchDataSizeInBytes,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_HEAP_TYPE_DEFAULT);
    if (!m_tlasScratch) {
        SPDLOG_ERROR("[RTX] Failed to create TLAS scratch buffer ({} bytes)",
                     tlasPrebuild.ScratchDataSizeInBytes);
        return false;
    }

    m_tlasResult = CreateBuffer(
        tlasPrebuild.ResultDataMaxSizeInBytes,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
        D3D12_HEAP_TYPE_DEFAULT);
    if (!m_tlasResult) {
        SPDLOG_ERROR("[RTX] Failed to create TLAS result buffer ({} bytes)",
                     tlasPrebuild.ResultDataMaxSizeInBytes);
        return false;
    }

    SPDLOG_INFO("[RTX] AccelerationStructure initialized (TLAS scratch: {} bytes, result: {} bytes)",
                tlasPrebuild.ScratchDataSizeInBytes, tlasPrebuild.ResultDataMaxSizeInBytes);
    return true;
}

void AccelerationStructure::Shutdown() {
    ReleaseAll();

    if (m_instanceBuffer && m_instanceBufferMapped) {
        m_instanceBuffer->Unmap(0, nullptr);
        m_instanceBufferMapped = nullptr;
    }

    m_instanceBuffer.Reset();
    m_tlasScratch.Reset();
    m_tlasResult.Reset();
    m_buildCommandList.Reset();
    m_buildCommandAllocator.Reset();
    m_device = nullptr;

    SPDLOG_INFO("[RTX] AccelerationStructure shut down");
}

bool AccelerationStructure::BuildBLAS(const RoomGeometry& geometry) {
    if (!m_device) {
        SPDLOG_ERROR("[RTX] BuildBLAS called before Initialize");
        return false;
    }

    const uint32_t roomIndex = geometry.roomIndex;

    // Remove any existing BLAS for this room first
    if (m_blasMap.count(roomIndex)) {
        SPDLOG_WARN("[RTX] Replacing existing BLAS for room {}", roomIndex);
        RemoveBLAS(roomIndex);
    }

    const bool hasOpaque = !geometry.opaqueMesh.vertices.empty() &&
                           !geometry.opaqueMesh.indices.empty();
    const bool hasAlpha = !geometry.alphaMesh.vertices.empty() &&
                          !geometry.alphaMesh.indices.empty();

    if (!hasOpaque && !hasAlpha) {
        SPDLOG_WARN("[RTX] Room {} has no geometry to build BLAS for", roomIndex);
        return false;
    }

    BLASData blasData = {};
    blasData.roomIndex = roomIndex;
    blasData.hasOpaqueGeometry = hasOpaque;
    blasData.hasAlphaGeometry = hasAlpha;
    blasData.numGeometries = (hasOpaque ? 1 : 0) + (hasAlpha ? 1 : 0);

    // Upload geometry data to GPU default heap buffers
    if (hasOpaque) {
        const auto& mesh = geometry.opaqueMesh;

        blasData.opaqueVertexBuffer = UploadBuffer(
            mesh.vertices.data(),
            mesh.vertices.size() * sizeof(RTXVertex),
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (!blasData.opaqueVertexBuffer) {
            SPDLOG_ERROR("[RTX] Failed to upload opaque vertex buffer for room {}", roomIndex);
            return false;
        }

        blasData.opaqueIndexBuffer = UploadBuffer(
            mesh.indices.data(),
            mesh.indices.size() * sizeof(uint32_t),
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (!blasData.opaqueIndexBuffer) {
            SPDLOG_ERROR("[RTX] Failed to upload opaque index buffer for room {}", roomIndex);
            return false;
        }

        blasData.opaqueMaterialIDBuffer = UploadBuffer(
            mesh.materialIDs.data(),
            mesh.materialIDs.size() * sizeof(uint32_t),
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (!blasData.opaqueMaterialIDBuffer) {
            SPDLOG_ERROR("[RTX] Failed to upload opaque material ID buffer for room {}", roomIndex);
            return false;
        }

        blasData.opaqueMaterialBuffer = UploadBuffer(
            mesh.materials.data(),
            mesh.materials.size() * sizeof(Material),
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (!blasData.opaqueMaterialBuffer) {
            SPDLOG_ERROR("[RTX] Failed to upload opaque material buffer for room {}", roomIndex);
            return false;
        }

        SPDLOG_DEBUG("[RTX] Room {} opaque: {} verts, {} indices, {} tris, {} materials",
                     roomIndex, mesh.vertices.size(), mesh.indices.size(),
                     mesh.indices.size() / 3, mesh.materials.size());
    }

    if (hasAlpha) {
        const auto& mesh = geometry.alphaMesh;

        blasData.alphaVertexBuffer = UploadBuffer(
            mesh.vertices.data(),
            mesh.vertices.size() * sizeof(RTXVertex),
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (!blasData.alphaVertexBuffer) {
            SPDLOG_ERROR("[RTX] Failed to upload alpha vertex buffer for room {}", roomIndex);
            return false;
        }

        blasData.alphaIndexBuffer = UploadBuffer(
            mesh.indices.data(),
            mesh.indices.size() * sizeof(uint32_t),
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (!blasData.alphaIndexBuffer) {
            SPDLOG_ERROR("[RTX] Failed to upload alpha index buffer for room {}", roomIndex);
            return false;
        }

        blasData.alphaMaterialIDBuffer = UploadBuffer(
            mesh.materialIDs.data(),
            mesh.materialIDs.size() * sizeof(uint32_t),
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (!blasData.alphaMaterialIDBuffer) {
            SPDLOG_ERROR("[RTX] Failed to upload alpha material ID buffer for room {}", roomIndex);
            return false;
        }

        blasData.alphaMaterialBuffer = UploadBuffer(
            mesh.materials.data(),
            mesh.materials.size() * sizeof(Material),
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (!blasData.alphaMaterialBuffer) {
            SPDLOG_ERROR("[RTX] Failed to upload alpha material buffer for room {}", roomIndex);
            return false;
        }

        SPDLOG_DEBUG("[RTX] Room {} alpha: {} verts, {} indices, {} tris, {} materials",
                     roomIndex, mesh.vertices.size(), mesh.indices.size(),
                     mesh.indices.size() / 3, mesh.materials.size());
    }

    // Build geometry descriptions for the BLAS
    std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geometryDescs;
    geometryDescs.reserve(blasData.numGeometries);

    if (hasOpaque) {
        D3D12_RAYTRACING_GEOMETRY_DESC desc = {};
        desc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        desc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
        desc.Triangles.VertexBuffer.StartAddress =
            blasData.opaqueVertexBuffer->GetGPUVirtualAddress();
        desc.Triangles.VertexBuffer.StrideInBytes = sizeof(RTXVertex);
        desc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        desc.Triangles.VertexCount =
            static_cast<UINT>(geometry.opaqueMesh.vertices.size());
        desc.Triangles.IndexBuffer =
            blasData.opaqueIndexBuffer->GetGPUVirtualAddress();
        desc.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
        desc.Triangles.IndexCount =
            static_cast<UINT>(geometry.opaqueMesh.indices.size());
        desc.Triangles.Transform3x4 = 0; // No per-geometry transform
        geometryDescs.push_back(desc);
    }

    if (hasAlpha) {
        D3D12_RAYTRACING_GEOMETRY_DESC desc = {};
        desc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        desc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_NO_DUPLICATE_ANYHIT_INVOCATION;
        desc.Triangles.VertexBuffer.StartAddress =
            blasData.alphaVertexBuffer->GetGPUVirtualAddress();
        desc.Triangles.VertexBuffer.StrideInBytes = sizeof(RTXVertex);
        desc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        desc.Triangles.VertexCount =
            static_cast<UINT>(geometry.alphaMesh.vertices.size());
        desc.Triangles.IndexBuffer =
            blasData.alphaIndexBuffer->GetGPUVirtualAddress();
        desc.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
        desc.Triangles.IndexCount =
            static_cast<UINT>(geometry.alphaMesh.indices.size());
        desc.Triangles.Transform3x4 = 0;
        geometryDescs.push_back(desc);
    }

    // Query prebuild info
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS blasInputs = {};
    blasInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    blasInputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    blasInputs.NumDescs = static_cast<UINT>(geometryDescs.size());
    blasInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    blasInputs.pGeometryDescs = geometryDescs.data();

    auto* d3dDevice = m_device->GetDevice();
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuildInfo = {};
    d3dDevice->GetRaytracingAccelerationStructurePrebuildInfo(&blasInputs, &prebuildInfo);

    if (prebuildInfo.ResultDataMaxSizeInBytes == 0) {
        SPDLOG_ERROR("[RTX] BLAS prebuild returned zero result size for room {}", roomIndex);
        return false;
    }

    // Allocate scratch and result buffers for the BLAS
    ComPtr<ID3D12Resource> scratchBuffer = CreateBuffer(
        prebuildInfo.ScratchDataSizeInBytes,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_HEAP_TYPE_DEFAULT);
    if (!scratchBuffer) {
        SPDLOG_ERROR("[RTX] Failed to create BLAS scratch buffer for room {}", roomIndex);
        return false;
    }

    blasData.blasResult = CreateBuffer(
        prebuildInfo.ResultDataMaxSizeInBytes,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
        D3D12_HEAP_TYPE_DEFAULT);
    if (!blasData.blasResult) {
        SPDLOG_ERROR("[RTX] Failed to create BLAS result buffer for room {}", roomIndex);
        return false;
    }

    // Reset the build command allocator and command list
    HRESULT hr = m_buildCommandAllocator->Reset();
    if (FAILED(hr)) {
        SPDLOG_ERROR("[RTX] Failed to reset BLAS build command allocator: 0x{:08X}",
                     static_cast<uint32_t>(hr));
        return false;
    }

    hr = m_buildCommandList->Reset(m_buildCommandAllocator.Get(), nullptr);
    if (FAILED(hr)) {
        SPDLOG_ERROR("[RTX] Failed to reset BLAS build command list: 0x{:08X}",
                     static_cast<uint32_t>(hr));
        return false;
    }

    // Build the BLAS
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc = {};
    buildDesc.Inputs = blasInputs;
    buildDesc.ScratchAccelerationStructureData = scratchBuffer->GetGPUVirtualAddress();
    buildDesc.DestAccelerationStructureData = blasData.blasResult->GetGPUVirtualAddress();

    m_buildCommandList->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);

    // UAV barrier to ensure the BLAS build completes before any reads
    D3D12_RESOURCE_BARRIER uavBarrier = {};
    uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarrier.UAV.pResource = blasData.blasResult.Get();
    m_buildCommandList->ResourceBarrier(1, &uavBarrier);

    // Execute and wait for GPU completion
    ExecuteAndWait(m_buildCommandList.Get());

    // Scratch buffer can be released now — the BLAS result is self-contained
    // (scratchBuffer goes out of scope and releases via ComPtr)

    // Store the BLAS data
    m_blasMap[roomIndex] = std::move(blasData);

    // Rebuild the flat list of geometry buffer addresses for shader table building
    RebuildGeometryBufferList();

    SPDLOG_INFO("[RTX] Built BLAS for room {} ({} geometries, result: {} bytes, scratch: {} bytes)",
                roomIndex, m_blasMap[roomIndex].numGeometries,
                prebuildInfo.ResultDataMaxSizeInBytes, prebuildInfo.ScratchDataSizeInBytes);
    return true;
}

void AccelerationStructure::RemoveBLAS(uint32_t roomIndex) {
    auto it = m_blasMap.find(roomIndex);
    if (it == m_blasMap.end()) {
        SPDLOG_WARN("[RTX] RemoveBLAS: no BLAS found for room {}", roomIndex);
        return;
    }

    // Wait for any in-flight GPU work that might reference this BLAS
    m_device->WaitForGPU();

    m_blasMap.erase(it);
    RebuildGeometryBufferList();

    SPDLOG_INFO("[RTX] Removed BLAS for room {} ({} rooms remaining)",
                roomIndex, m_blasMap.size());
}

void AccelerationStructure::RebuildTLAS(ID3D12GraphicsCommandList4* cmdList) {
    if (!m_device || m_blasMap.empty()) {
        m_tlasBuilt = false;
        return;
    }

    if (m_blasMap.size() > MAX_INSTANCES) {
        SPDLOG_ERROR("[RTX] Too many BLAS instances ({}) — max is {}",
                     m_blasMap.size(), MAX_INSTANCES);
        return;
    }

    // Fill instance descriptions in the persistently mapped upload buffer.
    // Each BLAS becomes one instance in the TLAS.
    auto* instanceDescs =
        static_cast<D3D12_RAYTRACING_INSTANCE_DESC*>(m_instanceBufferMapped);

    // Clear the buffer region we'll use
    memset(instanceDescs, 0,
           m_blasMap.size() * sizeof(D3D12_RAYTRACING_INSTANCE_DESC));

    // We need to track cumulative geometry index for InstanceContributionToHitGroupIndex.
    // The geometry buffer list is ordered: for each BLAS in m_blasMap iteration order,
    // opaque geometry first (if present), then alpha geometry (if present).
    // We must iterate in the same order as RebuildGeometryBufferList.
    //
    // Collect and sort by roomIndex for deterministic ordering.
    std::vector<uint32_t> sortedRooms;
    sortedRooms.reserve(m_blasMap.size());
    for (const auto& pair : m_blasMap) {
        sortedRooms.push_back(pair.first);
    }
    std::sort(sortedRooms.begin(), sortedRooms.end());

    uint32_t instanceIndex = 0;
    uint32_t geometryOffset = 0;

    for (uint32_t roomIdx : sortedRooms) {
        const auto& blas = m_blasMap.at(roomIdx);

        D3D12_RAYTRACING_INSTANCE_DESC& desc = instanceDescs[instanceIndex];

        // Identity 3x4 transform (rooms are already in world space)
        desc.Transform[0][0] = 1.0f;
        desc.Transform[1][1] = 1.0f;
        desc.Transform[2][2] = 1.0f;

        desc.InstanceID = roomIdx;
        desc.InstanceMask = 0xFF;
        desc.InstanceContributionToHitGroupIndex = geometryOffset;
        desc.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_NONE;
        desc.AccelerationStructure = blas.blasResult->GetGPUVirtualAddress();

        geometryOffset += blas.numGeometries;
        instanceIndex++;
    }

    // Build the TLAS
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tlasInputs = {};
    tlasInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    tlasInputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
    tlasInputs.NumDescs = instanceIndex;
    tlasInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    tlasInputs.InstanceDescs = m_instanceBuffer->GetGPUVirtualAddress();

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc = {};
    buildDesc.Inputs = tlasInputs;
    buildDesc.ScratchAccelerationStructureData = m_tlasScratch->GetGPUVirtualAddress();
    buildDesc.DestAccelerationStructureData = m_tlasResult->GetGPUVirtualAddress();

    cmdList->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);

    // UAV barrier on the TLAS result so subsequent DispatchRays can read it
    D3D12_RESOURCE_BARRIER uavBarrier = {};
    uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarrier.UAV.pResource = m_tlasResult.Get();
    cmdList->ResourceBarrier(1, &uavBarrier);

    m_tlasBuilt = true;
}

D3D12_GPU_VIRTUAL_ADDRESS AccelerationStructure::GetTLASAddress() const {
    if (!m_tlasResult) {
        return 0;
    }
    return m_tlasResult->GetGPUVirtualAddress();
}

const std::vector<AccelerationStructure::GeometryBufferInfo>&
AccelerationStructure::GetGeometryBuffers() const {
    return m_geometryBuffers;
}

uint32_t AccelerationStructure::GetInstanceCount() const {
    return static_cast<uint32_t>(m_blasMap.size());
}

bool AccelerationStructure::HasTLAS() const {
    return m_tlasBuilt;
}

void AccelerationStructure::ReleaseAll() {
    if (m_device) {
        m_device->WaitForGPU();
    }

    m_blasMap.clear();
    m_geometryBuffers.clear();
    m_tlasBuilt = false;

    SPDLOG_INFO("[RTX] Released all acceleration structures");
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

ComPtr<ID3D12Resource> AccelerationStructure::CreateBuffer(
    size_t size,
    D3D12_RESOURCE_FLAGS flags,
    D3D12_RESOURCE_STATES initialState,
    D3D12_HEAP_TYPE heapType) {

    if (size == 0) {
        SPDLOG_ERROR("[RTX] CreateBuffer called with size 0");
        return nullptr;
    }

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = heapType;

    D3D12_RESOURCE_DESC resourceDesc = {};
    resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resourceDesc.Alignment = 0;
    resourceDesc.Width = size;
    resourceDesc.Height = 1;
    resourceDesc.DepthOrArraySize = 1;
    resourceDesc.MipLevels = 1;
    resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
    resourceDesc.SampleDesc.Count = 1;
    resourceDesc.SampleDesc.Quality = 0;
    resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    resourceDesc.Flags = flags;

    ComPtr<ID3D12Resource> resource;
    HRESULT hr = m_device->GetDevice()->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &resourceDesc,
        initialState,
        nullptr,
        IID_PPV_ARGS(&resource));

    if (FAILED(hr)) {
        SPDLOG_ERROR("[RTX] CreateBuffer failed (size: {}, heap: {}, flags: 0x{:X}): 0x{:08X}",
                     size, static_cast<int>(heapType), static_cast<uint32_t>(flags),
                     static_cast<uint32_t>(hr));
        return nullptr;
    }

    return resource;
}

ComPtr<ID3D12Resource> AccelerationStructure::UploadBuffer(
    const void* data,
    size_t size,
    D3D12_RESOURCE_STATES afterState) {

    if (!data || size == 0) {
        SPDLOG_ERROR("[RTX] UploadBuffer called with null data or size 0");
        return nullptr;
    }

    // Step 1: Create an upload heap staging buffer
    ComPtr<ID3D12Resource> stagingBuffer = CreateBuffer(
        size,
        D3D12_RESOURCE_FLAG_NONE,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        D3D12_HEAP_TYPE_UPLOAD);
    if (!stagingBuffer) {
        return nullptr;
    }

    // Step 2: Map, copy data, unmap
    void* mapped = nullptr;
    HRESULT hr = stagingBuffer->Map(0, nullptr, &mapped);
    if (FAILED(hr)) {
        SPDLOG_ERROR("[RTX] Failed to map staging buffer: 0x{:08X}",
                     static_cast<uint32_t>(hr));
        return nullptr;
    }
    memcpy(mapped, data, size);
    stagingBuffer->Unmap(0, nullptr);

    // Step 3: Create the default heap destination buffer.
    // SRV buffers don't need UAV flag — they are read-only by shaders.
    ComPtr<ID3D12Resource> destBuffer = CreateBuffer(
        size,
        D3D12_RESOURCE_FLAG_NONE,
        D3D12_RESOURCE_STATE_COMMON,
        D3D12_HEAP_TYPE_DEFAULT);
    if (!destBuffer) {
        return nullptr;
    }

    // Step 4: Record copy command on the build command list.
    // The build command list is assumed to already be in a recording state,
    // OR we need to reset it here for a standalone upload. Since UploadBuffer
    // is called multiple times during BuildBLAS before the final BLAS build
    // execute, we use a separate pattern: reset, record copy, execute, wait.
    hr = m_buildCommandAllocator->Reset();
    if (FAILED(hr)) {
        SPDLOG_ERROR("[RTX] UploadBuffer: failed to reset command allocator: 0x{:08X}",
                     static_cast<uint32_t>(hr));
        return nullptr;
    }

    hr = m_buildCommandList->Reset(m_buildCommandAllocator.Get(), nullptr);
    if (FAILED(hr)) {
        SPDLOG_ERROR("[RTX] UploadBuffer: failed to reset command list: 0x{:08X}",
                     static_cast<uint32_t>(hr));
        return nullptr;
    }

    m_buildCommandList->CopyBufferRegion(destBuffer.Get(), 0, stagingBuffer.Get(), 0, size);

    // Transition destination buffer to the desired after state
    if (afterState != D3D12_RESOURCE_STATE_COMMON) {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = destBuffer.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = afterState;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        m_buildCommandList->ResourceBarrier(1, &barrier);
    }

    // Execute and wait
    ExecuteAndWait(m_buildCommandList.Get());

    return destBuffer;
}

void AccelerationStructure::ExecuteAndWait(ID3D12GraphicsCommandList4* cmdList) {
    HRESULT hr = cmdList->Close();
    if (FAILED(hr)) {
        SPDLOG_ERROR("[RTX] ExecuteAndWait: failed to close command list: 0x{:08X}",
                     static_cast<uint32_t>(hr));
        return;
    }

    ID3D12CommandList* ppCommandLists[] = { cmdList };
    m_device->GetCommandQueue()->ExecuteCommandLists(1, ppCommandLists);

    // Wait for GPU to finish processing this command list
    m_device->WaitForGPU();
}

void AccelerationStructure::RebuildGeometryBufferList() {
    m_geometryBuffers.clear();

    // Sort rooms by index for deterministic ordering (must match TLAS instance order)
    std::vector<uint32_t> sortedRooms;
    sortedRooms.reserve(m_blasMap.size());
    for (const auto& pair : m_blasMap) {
        sortedRooms.push_back(pair.first);
    }
    std::sort(sortedRooms.begin(), sortedRooms.end());

    for (uint32_t roomIdx : sortedRooms) {
        const auto& blas = m_blasMap.at(roomIdx);

        if (blas.hasOpaqueGeometry) {
            GeometryBufferInfo info = {};
            info.vertexBufferAddress = blas.opaqueVertexBuffer->GetGPUVirtualAddress();
            info.indexBufferAddress = blas.opaqueIndexBuffer->GetGPUVirtualAddress();
            info.materialIDBufferAddress = blas.opaqueMaterialIDBuffer->GetGPUVirtualAddress();
            info.materialTableAddress = blas.opaqueMaterialBuffer->GetGPUVirtualAddress();
            m_geometryBuffers.push_back(info);
        }

        if (blas.hasAlphaGeometry) {
            GeometryBufferInfo info = {};
            info.vertexBufferAddress = blas.alphaVertexBuffer->GetGPUVirtualAddress();
            info.indexBufferAddress = blas.alphaIndexBuffer->GetGPUVirtualAddress();
            info.materialIDBufferAddress = blas.alphaMaterialIDBuffer->GetGPUVirtualAddress();
            info.materialTableAddress = blas.alphaMaterialBuffer->GetGPUVirtualAddress();
            m_geometryBuffers.push_back(info);
        }
    }

    SPDLOG_DEBUG("[RTX] Geometry buffer list rebuilt: {} entries across {} rooms",
                 m_geometryBuffers.size(), m_blasMap.size());
}

} // namespace RTX

#endif // ENABLE_DX12_RTX
