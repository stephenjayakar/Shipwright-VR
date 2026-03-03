#ifdef ENABLE_DX12_RTX

#include "SceneGeometryExtractor.h"
#include "TextureManager.h"
#include "RTXSceneConfig.h"
#include <spdlog/spdlog.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>
#ifdef _WIN32
#include <Windows.h>
#endif

#include "RTXDiagLog.h"

// macros.h has its own guards and includes C++ headers.
#include "macros.h"

// Forward declarations for OTR resource loading functions.
extern "C" {
    Vtx* ResourceMgr_LoadVtxByName(char* path);
    Vtx* ResourceMgr_LoadVtxByCRC(uint64_t crc);
    Gfx* ResourceMgr_LoadGfxByName(const char* path);
    Gfx* ResourceMgr_LoadGfxByCRC(uint64_t crc);
    char* ResourceMgr_GetNameByCRC(uint64_t crc, char* alloc);
    // The same function the Fast3D interpreter uses — goes through
    // ResourceManager::GetResourceRawPointer(crc) which calls LoadResource(crc).
    void* ResourceGetDataByCrc(uint64_t crc);
    const char* ResourceGetNameByCrc(uint64_t crc);
    // The game's segment table — set up by the engine during scene/room loading.
    // Segment addresses (e.g., 0x08000000) are resolved by:
    //   base = gSegments[segNum] + (addr & 0x00FFFFFE)
    // The LSB (bit 0) is used as a "segmented address" flag by the Fast3D interpreter.
    extern uintptr_t gSegments[];
}

// Resolve a segment address using the game's segment table, matching the
// Fast3D interpreter's SegAddr function.
static void* ResolveSegAddr(uintptr_t w1) {
    if (w1 & 1) {
        // Segmented address: bit 0 is set as a flag.
        uint32_t segNum = (uint32_t)(w1 >> 24);
        uint32_t offset = (uint32_t)(w1 & 0x00FFFFFE);
        if (segNum < 16 && gSegments[segNum] != 0) {
            return (void*)(gSegments[segNum] + offset);
        }
        return nullptr; // Segment not set up
    }
    // Not segmented — return as-is (should be a valid host pointer)
    return (void*)w1;
}

namespace RTX {

// ============================================================================
// SEH-safe OTR display list resolution helper
// ============================================================================
// Extracted from the lambda to a standalone static function because MSVC forbids
// __try/__except inside lambdas and functions with C++ objects with destructors.
#ifdef _WIN32
static bool SafeCheckOTRPrefix(const char* ptr) {
    bool result = false;
    __try {
        result = (ptr[0] == '_' && ptr[1] == '_');
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        result = false;
    }
    return result;
}

static bool SafeReadFirstTwoBytes(const char* ptr, uint8_t& out0, uint8_t& out1) {
    bool ok = false;
    __try {
        out0 = (uint8_t)ptr[0];
        out1 = (uint8_t)ptr[1];
        ok = true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        out0 = 0;
        out1 = 0;
        ok = false;
    }
    return ok;
}
#endif

static Gfx* ResolveOTRDisplayList(Gfx* rawPtr) {
    if (!rawPtr) return nullptr;
    const char* asStr = (const char*)rawPtr;
    bool looksLikeOTRPath = false;
#ifdef _WIN32
    looksLikeOTRPath = SafeCheckOTRPrefix(asStr);
    // If the pointer caused an access violation in SafeCheckOTRPrefix, it returned false.
    // We also can't safely use it as a Gfx* if the pointer is bad.
    // Check by trying to read the first two bytes:
    if (!looksLikeOTRPath) {
        uint8_t byte0 = 0, byte1 = 0;
        if (!SafeReadFirstTwoBytes(asStr, byte0, byte1)) {
            RTX_DiagLog("ResolveOTRDisplayList: rawPtr=%p caused access violation, skipping", (void*)rawPtr);
            return nullptr;
        }
        RTX_DiagLog("ResolveOTRDisplayList: rawPtr=%p is not an OTR path (first bytes: 0x%02X 0x%02X)",
                    (void*)rawPtr, byte0, byte1);
        return rawPtr;
    }
#else
    if ((uintptr_t)rawPtr > 0x10000) {
        looksLikeOTRPath = (asStr[0] == '_' && asStr[1] == '_');
    }
    if (!looksLikeOTRPath) {
        if ((uintptr_t)rawPtr > 0x10000) {
            RTX_DiagLog("ResolveOTRDisplayList: rawPtr=%p is not an OTR path (first bytes: 0x%02X 0x%02X)",
                        (void*)rawPtr, (uint8_t)asStr[0], (uint8_t)asStr[1]);
        } else {
            RTX_DiagLog("ResolveOTRDisplayList: rawPtr=%p is below 0x10000, treating as raw Gfx*", (void*)rawPtr);
        }
        return rawPtr;
    }
#endif
    // It's an OTR path string — resolve to actual Gfx data.
    Gfx* result = ResourceMgr_LoadGfxByName(asStr);
    if (result) {
        uint8_t firstOp = (uint8_t)(result->words.w0 >> 24);
        RTX_DiagLog("ResolveOTRDisplayList: '%s' -> %p (firstOp=0x%02X, w0=0x%llX, w1=0x%llX)",
                    asStr, (void*)result, firstOp,
                    (unsigned long long)result->words.w0,
                    (unsigned long long)result->words.w1);
    } else {
        RTX_DiagLog("ResolveOTRDisplayList: '%s' -> FAILED (nullptr)", asStr);
    }
    return result;
}

// ============================================================================
// Construction / destruction
// ============================================================================

SceneGeometryExtractor::SceneGeometryExtractor()
    : m_opaqueMesh(nullptr)
    , m_alphaMesh(nullptr)
    , m_totalCommandsProcessed(0)
    , m_totalTrianglesEmitted(0)
    , m_totalVerticesLoaded(0)
    , m_totalDLsWalked(0) {
    Reset();
}

SceneGeometryExtractor::~SceneGeometryExtractor() = default;

void SceneGeometryExtractor::Reset() {
    memset(m_vertexBuffer, 0, sizeof(m_vertexBuffer));
    memset(m_vertexBufferValid, 0, sizeof(m_vertexBufferValid));
    m_materialState.Reset();
    m_opaqueMesh = nullptr;
    m_alphaMesh = nullptr;
    m_opaqueMaterialMap.clear();
    m_alphaMaterialMap.clear();
    m_persistentPathStrings.clear();
    m_totalCommandsProcessed = 0;
    m_totalTrianglesEmitted = 0;
    m_totalVerticesLoaded = 0;
    m_totalDLsWalked = 0;
}

// ============================================================================
// Public API
// ============================================================================

RoomGeometry SceneGeometryExtractor::ExtractRoomGeometry(Room* room, uint32_t roomIndex) {
    printf("[RTX] ExtractGeometry: starting room %u\n", roomIndex);
    RTX_DIAG("SceneGeometryExtractor: BEGIN extraction for room %u", roomIndex);
    Reset();

    RoomGeometry result;
    result.roomIndex = roomIndex;

    if (!room) {
        RTX_DIAG("SceneGeometryExtractor: EARLY EXIT - null Room pointer for room %u", roomIndex);
        SPDLOG_ERROR("[RTX] SceneGeometryExtractor: null Room pointer for room {}", roomIndex);
        return result;
    }

    MeshHeader* meshHeader = room->meshHeader;
    if (!meshHeader) {
        RTX_DIAG("SceneGeometryExtractor: EARLY EXIT - null meshHeader for room %u (room=%p, segment=%p)",
                 roomIndex, (void*)room, (void*)room->segment);
        SPDLOG_ERROR("[RTX] SceneGeometryExtractor: null meshHeader for room {}", roomIndex);
        return result;
    }
    RTX_DIAG("SceneGeometryExtractor: room %u meshHeader=%p, segment=%p", roomIndex, (void*)meshHeader, (void*)room->segment);

    m_opaqueMesh = &result.opaqueMesh;
    m_alphaMesh = &result.alphaMesh;

    uint8_t meshType = meshHeader->base.type;
    SPDLOG_INFO("[RTX] SceneGeometryExtractor: room {} meshType={}", roomIndex, meshType);

    // Helper: In the SoH (OTR) build, PolygonDlist::opa/xlu fields are NOT
    // pointers to Gfx command data. They are char* pointers to OTR resource
    // path strings (e.g. "__OTR__kokiri_forest/room_0_opaDL_004B10").
    // The game's renderer emits gSPDisplayList() which the interpreter resolves
    // at draw time. We need to do the same resolution here by calling
    // ResourceMgr_LoadGfxByName() to get the actual Gfx* pointer.
    // Use the standalone static function for OTR DL resolution (SEH-safe on Windows).
    auto resolveOTRDL = &ResolveOTRDisplayList;

    if (meshType == 0) {
        // PolygonType0: array of PolygonDlist entries (opa + xlu display lists).
        PolygonType0* poly0 = &meshHeader->polygon0;
        uint8_t numEntries = poly0->num;
        PolygonDlist* dlists = (PolygonDlist*)SEGMENTED_TO_VIRTUAL(poly0->start);

        if (!dlists) {
            RTX_DIAG("SceneGeometryExtractor: EARLY EXIT - null dlist start for room %u type 0", roomIndex);
            SPDLOG_WARN("[RTX] SceneGeometryExtractor: null dlist start for room {} type 0", roomIndex);
            return result;
        }

        // Sanity cap: N64 rooms typically have 1-4 DL entries. Cap at 64 to prevent
        // reading way past the end of the dlist array from malformed mesh headers.
        if (numEntries > 64) {
            SPDLOG_WARN("[RTX] SceneGeometryExtractor: room {} type 0 has unreasonable numEntries={}, capping to 64", roomIndex, numEntries);
            numEntries = 64;
        }

        RTX_DIAG("SceneGeometryExtractor: room %u type 0, %u DL entries, dlists=%p", roomIndex, numEntries, (void*)dlists);
        SPDLOG_INFO("[RTX] SceneGeometryExtractor: room {} type 0 with {} DL entries", roomIndex, numEntries);

        for (uint8_t i = 0; i < numEntries; i++) {
            // Opaque display list
            if (dlists[i].opa) {
                Gfx* opaDL = resolveOTRDL(dlists[i].opa);
                if (opaDL) {
                    SPDLOG_DEBUG("[RTX] SceneGeometryExtractor: room {} entry {} walking opa DL at {:p}",
                                 roomIndex, i, (void*)opaDL);
                    m_materialState.Reset();
                    WalkDisplayList(opaDL, /*isTranslucent=*/false, /*depth=*/0);
                } else {
                    SPDLOG_WARN("[RTX] SceneGeometryExtractor: room {} entry {} failed to resolve opa DL (ptr={:p})",
                                roomIndex, i, (void*)dlists[i].opa);
                }
            }

            // Translucent display list
            if (dlists[i].xlu) {
                Gfx* xluDL = resolveOTRDL(dlists[i].xlu);
                if (xluDL) {
                    SPDLOG_DEBUG("[RTX] SceneGeometryExtractor: room {} entry {} walking xlu DL at {:p}",
                                 roomIndex, i, (void*)xluDL);
                    m_materialState.Reset();
                    WalkDisplayList(xluDL, /*isTranslucent=*/true, /*depth=*/0);
                } else {
                    SPDLOG_WARN("[RTX] SceneGeometryExtractor: room {} entry {} failed to resolve xlu DL (ptr={:p})",
                                roomIndex, i, (void*)dlists[i].xlu);
                }
            }
        }
    } else if (meshType == 2) {
        // PolygonType2: array of PolygonDlist2 entries with world-space positions.
        PolygonType2* poly2 = &meshHeader->polygon2;
        uint8_t numEntries = poly2->num;
        PolygonDlist2* dlists = (PolygonDlist2*)SEGMENTED_TO_VIRTUAL(poly2->start);

        if (!dlists) {
            RTX_DIAG("SceneGeometryExtractor: EARLY EXIT - null dlist start for room %u type 2", roomIndex);
            SPDLOG_WARN("[RTX] SceneGeometryExtractor: null dlist start for room {} type 2", roomIndex);
            return result;
        }

        // Sanity cap: N64 rooms typically have 1-4 DL entries. Cap at 64 to prevent
        // reading way past the end of the dlist array from malformed mesh headers.
        if (numEntries > 64) {
            SPDLOG_WARN("[RTX] SceneGeometryExtractor: room {} type 2 has unreasonable numEntries={}, capping to 64", roomIndex, numEntries);
            numEntries = 64;
        }

        RTX_DIAG("SceneGeometryExtractor: room %u type 2, %u DL entries, dlists=%p", roomIndex, numEntries, (void*)dlists);
        SPDLOG_INFO("[RTX] SceneGeometryExtractor: room {} type 2 with {} DL entries", roomIndex, numEntries);

        for (uint8_t i = 0; i < numEntries; i++) {
            // Opaque display list
            if (dlists[i].opa) {
                Gfx* opaDL = resolveOTRDL(dlists[i].opa);
                if (opaDL) {
                    SPDLOG_DEBUG("[RTX] SceneGeometryExtractor: room {} entry {} walking opa DL (type2) at {:p}",
                                 roomIndex, i, (void*)opaDL);
                    m_materialState.Reset();
                    WalkDisplayList(opaDL, /*isTranslucent=*/false, /*depth=*/0);
                } else {
                    SPDLOG_WARN("[RTX] SceneGeometryExtractor: room {} entry {} failed to resolve opa DL type2 (ptr={:p})",
                                roomIndex, i, (void*)dlists[i].opa);
                }
            }

            // Translucent display list
            if (dlists[i].xlu) {
                Gfx* xluDL = resolveOTRDL(dlists[i].xlu);
                if (xluDL) {
                    SPDLOG_DEBUG("[RTX] SceneGeometryExtractor: room {} entry {} walking xlu DL (type2) at {:p}",
                                 roomIndex, i, (void*)xluDL);
                    m_materialState.Reset();
                    WalkDisplayList(xluDL, /*isTranslucent=*/true, /*depth=*/0);
                } else {
                    SPDLOG_WARN("[RTX] SceneGeometryExtractor: room {} entry {} failed to resolve xlu DL type2 (ptr={:p})",
                                roomIndex, i, (void*)dlists[i].xlu);
                }
            }
        }
    } else if (meshType == 1) {
        // PolygonType1: pre-rendered background. Has a single display list for
        // foreground geometry but no real 3D room mesh. We extract what we can.
        PolygonType1* poly1 = (PolygonType1*)meshHeader;
        if (poly1->dlist) {
            Gfx* dl = resolveOTRDL((Gfx*)poly1->dlist);
            if (dl) {
                SPDLOG_INFO("[RTX] SceneGeometryExtractor: room {} type 1 (pre-rendered bg), walking fg DL at {:p}",
                            roomIndex, (void*)dl);
                m_materialState.Reset();
                WalkDisplayList(dl, /*isTranslucent=*/false, /*depth=*/0);
            } else {
                SPDLOG_INFO("[RTX] SceneGeometryExtractor: room {} type 1 failed to resolve fg DL", roomIndex);
            }
        } else {
            SPDLOG_INFO("[RTX] SceneGeometryExtractor: room {} type 1 with no foreground DL, skipping", roomIndex);
        }
    } else {
        RTX_DIAG("SceneGeometryExtractor: UNSUPPORTED mesh type %u for room %u", meshType, roomIndex);
        SPDLOG_WARN("[RTX] SceneGeometryExtractor: unsupported mesh type {} for room {}", meshType, roomIndex);
    }

    // Set alpha-test flags on meshes.
    result.opaqueMesh.hasAlphaTest = false;
    result.alphaMesh.hasAlphaTest = true;

    // Compute total vertex count and triangle count for diagnostics
    size_t totalVertexCount = result.opaqueMesh.vertices.size() + result.alphaMesh.vertices.size();
    size_t totalTriangleCount = (result.opaqueMesh.indices.size() + result.alphaMesh.indices.size()) / 3;
    printf("[RTX] Extracting geometry: %zu tris so far (room %u complete, %zu verts)\n", totalTriangleCount, roomIndex, totalVertexCount);

    // Log vertex position bounds for debugging ray-geometry intersection
    {
        float minX = 1e9f, minY = 1e9f, minZ = 1e9f;
        float maxX = -1e9f, maxY = -1e9f, maxZ = -1e9f;
        auto updateBounds = [&](const ExtractedMesh& mesh) {
            for (const auto& v : mesh.vertices) {
                if (v.position[0] < minX) minX = v.position[0];
                if (v.position[1] < minY) minY = v.position[1];
                if (v.position[2] < minZ) minZ = v.position[2];
                if (v.position[0] > maxX) maxX = v.position[0];
                if (v.position[1] > maxY) maxY = v.position[1];
                if (v.position[2] > maxZ) maxZ = v.position[2];
            }
        };
        updateBounds(result.opaqueMesh);
        updateBounds(result.alphaMesh);
        RTX_DIAG("[BOUNDS] Room %u vertex AABB: (%.1f, %.1f, %.1f) to (%.1f, %.1f, %.1f)",
                 roomIndex, minX, minY, minZ, maxX, maxY, maxZ);
        // First 3 vertices
        if (!result.opaqueMesh.vertices.empty()) {
            for (int i = 0; i < 3 && i < (int)result.opaqueMesh.vertices.size(); i++) {
                const auto& v = result.opaqueMesh.vertices[i];
                RTX_DIAG("[BOUNDS] Room %u opaque vert[%d]: pos=(%.1f, %.1f, %.1f) nrm=(%.2f, %.2f, %.2f) uv=(%.3f, %.3f)",
                         roomIndex, i, v.position[0], v.position[1], v.position[2],
                         v.normal[0], v.normal[1], v.normal[2], v.uv[0], v.uv[1]);
            }
        }
    }
    RTX_DIAG("[DIAG] SceneGeometryExtractor: Room %u COMPLETE. "
             "Total vertices=%zu, Total triangles=%zu. "
             "Opaque: %zu verts, %zu indices (%zu tris), %zu materials. "
             "Alpha: %zu verts, %zu indices (%zu tris), %zu materials. "
             "DLs walked=%u, cmds processed=%u.",
             roomIndex,
             totalVertexCount, totalTriangleCount,
             result.opaqueMesh.vertices.size(),
             result.opaqueMesh.indices.size(),
             result.opaqueMesh.indices.size() / 3,
             result.opaqueMesh.materials.size(),
             result.alphaMesh.vertices.size(),
             result.alphaMesh.indices.size(),
             result.alphaMesh.indices.size() / 3,
             result.alphaMesh.materials.size(),
             m_totalDLsWalked,
             m_totalCommandsProcessed);

    // Log per-material texture address info for debugging texture resolution
    {
        auto logMeshTexAddrs = [](const ExtractedMesh& mesh, const char* meshName, uint32_t roomIndex) {
            uint32_t withAddr = 0, withString = 0, withoutAddr = 0;
            for (size_t i = 0; i < mesh.materialTextureAddrs.size(); i++) {
                uintptr_t addr = mesh.materialTextureAddrs[i];
                if (addr == 0) {
                    withoutAddr++;
                } else if (addr > 0x10000) {
                    withAddr++;
                    bool isStr = false;
#ifdef _WIN32
                    __try {
                        const char* s = (const char*)addr;
                        isStr = (s[0] == '_' && s[1] == '_');
                    } __except(EXCEPTION_EXECUTE_HANDLER) {
                        isStr = false;
                    }
#endif
                    if (isStr) withString++;
                } else {
                    withAddr++;
                }
            }
            RTX_DiagLog("[RTX] Room %u %s materials: %zu total, %u with addr, %u OTR strings, %u no-addr",
                        roomIndex, meshName, mesh.materials.size(), withAddr, withString, withoutAddr);
        };
        logMeshTexAddrs(result.opaqueMesh, "opaque", roomIndex);
        logMeshTexAddrs(result.alphaMesh, "alpha", roomIndex);
    }

    SPDLOG_INFO("[RTX] SceneGeometryExtractor: room {} extraction complete. "
                "Opaque: {} verts, {} indices, {} materials. "
                "Alpha: {} verts, {} indices, {} materials. "
                "Total: {} tris emitted, {} verts loaded, {} DLs walked, {} cmds processed.",
                roomIndex,
                result.opaqueMesh.vertices.size(),
                result.opaqueMesh.indices.size(),
                result.opaqueMesh.materials.size(),
                result.alphaMesh.vertices.size(),
                result.alphaMesh.indices.size(),
                result.alphaMesh.materials.size(),
                m_totalTrianglesEmitted,
                m_totalVerticesLoaded,
                m_totalDLsWalked,
                m_totalCommandsProcessed);

    // Texture resolution summary - critical for debugging white texture issue
    {
        uint32_t opaqueResolved = 0, opaqueUnresolved = 0;
        uint32_t alphaResolved = 0, alphaUnresolved = 0;
        for (const auto& mat : result.opaqueMesh.materials) {
            // textureIndex > 1 means resolved to a real texture (0=white, 1=checkerboard fallback)
            if (mat.textureIndex > 1) opaqueResolved++;
            else opaqueUnresolved++;
        }
        for (const auto& mat : result.alphaMesh.materials) {
            if (mat.textureIndex > 1) alphaResolved++;
            else alphaUnresolved++;
        }
        uint32_t opaqueWithPath = 0, alphaWithPath = 0;
        for (const auto& p : result.opaqueMesh.materialTexturePaths) {
            if (!p.empty()) opaqueWithPath++;
        }
        for (const auto& p : result.alphaMesh.materialTexturePaths) {
            if (!p.empty()) alphaWithPath++;
        }

        // Count textured vs untextured triangles for diagnostic purposes.
        // A "textured" triangle is one whose material has textureIndex > 1 (resolved to a real texture).
        // An "untextured" triangle uses the fallback (checkerboard=1 or white=0).
        // Water triangles are counted separately.
        uint32_t opaqueTexturedTris = 0, opaqueUntexturedTris = 0, opaqueWaterTris = 0;
        uint32_t alphaTexturedTris = 0, alphaUntexturedTris = 0, alphaWaterTris = 0;

        auto countTriangleTextures = [](const ExtractedMesh& mesh,
                                        uint32_t& texturedOut, uint32_t& untexturedOut, uint32_t& waterOut) {
            for (size_t triIdx = 0; triIdx < mesh.materialIDs.size(); triIdx++) {
                uint32_t matID = mesh.materialIDs[triIdx];
                if (matID < mesh.materials.size()) {
                    const Material& mat = mesh.materials[matID];
                    if (mat.isWater) {
                        waterOut++;
                    } else if (mat.textureIndex > 1) {
                        texturedOut++;
                    } else {
                        untexturedOut++;
                    }
                }
            }
        };

        countTriangleTextures(result.opaqueMesh, opaqueTexturedTris, opaqueUntexturedTris, opaqueWaterTris);
        countTriangleTextures(result.alphaMesh, alphaTexturedTris, alphaUntexturedTris, alphaWaterTris);

        RTX_DIAG("Room %u texture summary: "
                 "opaque mats(%u resolved, %u unresolved, %u with OTR path) "
                 "alpha mats(%u resolved, %u unresolved, %u with OTR path) "
                 "TextureManager nextSRV=%u",
                 roomIndex,
                 opaqueResolved, opaqueUnresolved, opaqueWithPath,
                 alphaResolved, alphaUnresolved, alphaWithPath,
                 RTX::TextureManager::GetInstance().GetNextSRVIndex());

        // First-frame diagnostic: per-triangle texture breakdown.
        // This helps identify the Deku Tree white texture issue — if many triangles
        // show up as "untextured", it means texture binding during extraction is failing.
        // We log for the first batch of room extractions (up to 32 rooms) then stop.
        static uint32_t s_firstFrameTriDiagCount = 0;
        if (s_firstFrameTriDiagCount < 32) {
            s_firstFrameTriDiagCount++;
            RTX_DIAG("[FIRST_FRAME_DIAG] Room %u triangle texture breakdown: "
                     "OPAQUE: %u textured, %u untextured (fallback), %u water. "
                     "ALPHA: %u textured, %u untextured (fallback), %u water. "
                     "TOTAL: %u textured, %u untextured, %u water out of %zu triangles.",
                     roomIndex,
                     opaqueTexturedTris, opaqueUntexturedTris, opaqueWaterTris,
                     alphaTexturedTris, alphaUntexturedTris, alphaWaterTris,
                     opaqueTexturedTris + alphaTexturedTris,
                     opaqueUntexturedTris + alphaUntexturedTris,
                     opaqueWaterTris + alphaWaterTris,
                     totalTriangleCount);

            // Log individual material details for the first few rooms
            auto logMaterialDetails = [roomIndex](const ExtractedMesh& mesh, const char* meshName) {
                for (size_t i = 0; i < mesh.materials.size(); i++) {
                    const Material& mat = mesh.materials[i];
                    // Count triangles using this material
                    uint32_t triCount = 0;
                    for (size_t t = 0; t < mesh.materialIDs.size(); t++) {
                        if (mesh.materialIDs[t] == (uint32_t)i) triCount++;
                    }
                    const char* texPath = (i < mesh.materialTexturePaths.size() && !mesh.materialTexturePaths[i].empty())
                                          ? mesh.materialTexturePaths[i].c_str() : "(none)";
                    RTX_DIAG("[FIRST_FRAME_DIAG] Room %u %s mat[%zu]: texIdx=%u combiner=%u alpha=%u water=%u "
                             "wrap=(%u,%u) dim=(%ux%u) tris=%u path='%.80s'",
                             roomIndex, meshName, i, mat.textureIndex, mat.combinerMode,
                             mat.isAlphaTested, mat.isWater, mat.wrapModeS, mat.wrapModeT,
                             mat.texWidthPx, mat.texHeightPx, triCount, texPath);
                }
            };

            if (!result.opaqueMesh.materials.empty()) {
                logMaterialDetails(result.opaqueMesh, "opaque");
            }
            if (!result.alphaMesh.materials.empty()) {
                logMaterialDetails(result.alphaMesh, "alpha");
            }
        }
    }

    // Step 4 of the Deku Tree texture fix task: Log TEXTURE_BIND diagnostics for the
    // first frame only. This logs every material's textureID, dimensions, and format
    // to help identify if the Deku Tree's textures are missing, wrong format, or wrong binding.
    {
        auto& texMgr = RTX::TextureManager::GetInstance();
        // Guard: only call LogFirstFrameTextureBinds if the SRV heap is initialized.
        // This prevents null dereference if TextureManager hasn't been fully set up yet.
        if (texMgr.GetSRVHeap() != nullptr && !texMgr.HasLoggedFirstFrameBinds()) {
            // Additional guard: ensure materials data pointer is valid before passing
            if (!result.opaqueMesh.materials.empty()) {
                const Material* opaqueMatPtr = result.opaqueMesh.materials.data();
                if (opaqueMatPtr != nullptr) {
                    texMgr.LogFirstFrameTextureBinds(
                        opaqueMatPtr,
                        static_cast<uint32_t>(result.opaqueMesh.materials.size()),
                        "opaque", roomIndex);
                }
            }
            if (!result.alphaMesh.materials.empty()) {
                const Material* alphaMatPtr = result.alphaMesh.materials.data();
                if (alphaMatPtr != nullptr) {
                    texMgr.LogFirstFrameTextureBinds(
                        alphaMatPtr,
                        static_cast<uint32_t>(result.alphaMesh.materials.size()),
                        "alpha", roomIndex);
                }
            }
        }
    }

    m_opaqueMesh = nullptr;
    m_alphaMesh = nullptr;

    return result;
}

// ============================================================================
// Display list walker
// ============================================================================

// Internal implementation of display list walking, separated so we can wrap
// the outer call in SEH to catch access violations from bad DL pointers.
void SceneGeometryExtractor::WalkDisplayListInner(const Gfx* dl, bool isTranslucent, uint32_t depth) {
    m_totalDLsWalked++;
    uint32_t cmdCount = 0;
    uint32_t localTriCount = 0;
    uint32_t localVtxCount = 0;
    uint32_t localDLHashCount = 0;
    uint32_t localVtxHashCount = 0;
    uint32_t localDLFPCount = 0;
    uint32_t localVtxFPCount = 0;

    if (m_totalDLsWalked <= 30 || (m_totalDLsWalked % 50) == 0) {
        RTX_DIAG("SceneGeometryExtractor: WalkDisplayList #%u at %p (depth=%u, translucent=%s)",
                 m_totalDLsWalked, (const void*)dl, depth, isTranslucent ? "yes" : "no");
    }

    for (const Gfx* cmd = dl; cmdCount < MAX_COMMANDS_PER_DL; cmd++, cmdCount++) {
        uint8_t opcode = (uint8_t)(cmd->words.w0 >> 24);
        m_totalCommandsProcessed++;

        switch (opcode) {

        // ---- Vertex loading ----
        case G_VTX:
            localVtxCount++;
            HandleVertexLoad(*cmd);
            break;

        case G_VTX_OTR_FILEPATH: {
            localVtxFPCount++;
            uint32_t extra = HandleVertexLoadOTRFilePath(cmd);
            cmd += extra;
            cmdCount += extra;
            break;
        }

        case G_VTX_OTR_HASH: {
            localVtxHashCount++;
            uint32_t extra = HandleVertexLoadOTRHash(cmd);
            cmd += extra;
            cmdCount += extra;
            break;
        }

        // ---- Triangle emission ----
        case G_TRI1:
            localTriCount++;
            HandleTri1(*cmd, isTranslucent);
            break;

        case G_TRI2:
            localTriCount += 2;
            HandleTri2(*cmd, isTranslucent);
            break;

        // ---- Material state ----
        case G_SETCOMBINE:
            HandleSetCombine(*cmd);
            break;

        case G_SETTIMG:
            HandleSetTextureImage(*cmd);
            break;

        case G_SETTIMG_OTR_FILEPATH:
            // G_SETTIMG_OTR_FILEPATH is a 1-word command (unlike the HASH variant):
            //   w0 = opcode | fmt | size | width-1
            //   w1 = (uintptr_t) file path string
            // The interpreter handler (gfx_set_timg_otr_filepath_handler_custom) does NOT
            // advance cmd0 and returns false, so the main loop advances by 1 word total.
            HandleSetTextureImageOTRFilePath(cmd);
            break;

        case G_SETTIMG_OTR_HASH: {
            uint32_t extra = HandleSetTextureImageOTRHash(cmd);
            cmd += extra;
            cmdCount += extra;
            break;
        }

        case G_GEOMETRYMODE:
            HandleGeometryMode(*cmd);
            break;

        case G_SETOTHERMODE_L:
            HandleSetOtherModeL(*cmd);
            break;

        case G_SETTILE:
            HandleSetTile(*cmd);
            break;

        case G_SETTILESIZE:
            HandleSetTileSize(*cmd);
            break;

        // ---- Display list control flow ----
        case G_DL: {
            // Bit 16 of w0: 0 = push (return after child), 1 = branch (replace current)
            bool isBranch = ((cmd->words.w0 >> 16) & 0x01) != 0;
            uintptr_t rawAddr = (uintptr_t)cmd->words.w1;
            Gfx* childDL = nullptr;

            // Use the same resolution logic as the Fast3D interpreter's SegAddr.
            // In the OTR build, w1 might be:
            // 1. A segment address with bit 0 set (e.g., 0x08000001) — resolve via gSegments
            // 2. A host pointer (e.g., > 0x10000 with bit 0 clear) — already valid
            // 3. An OTR path string pointer — starts with "__"
            if (rawAddr == 0) {
                // Null pointer, skip
            } else if (rawAddr & 1) {
                // Segmented address (bit 0 set) — resolve via game's segment table
                void* resolved = ResolveSegAddr(rawAddr);
                if (resolved) {
                    childDL = (Gfx*)resolved;
                    static uint32_t s_segResolveCount = 0;
                    s_segResolveCount++;
                    if (s_segResolveCount <= 10) {
                        uint32_t segNum = (uint32_t)(rawAddr >> 24);
                        RTX_DIAG("G_DL: resolved segment addr 0x%llX (seg=%u) -> %p",
                                 (unsigned long long)rawAddr, segNum, (void*)childDL);
                    }
                } else {
                    static uint32_t s_segFailCount = 0;
                    s_segFailCount++;
                    if (s_segFailCount <= 10) {
                        uint32_t segNum = (uint32_t)(rawAddr >> 24);
                        RTX_DIAG("G_DL: segment addr 0x%llX (seg=%u) not resolved (segment not loaded)",
                                 (unsigned long long)rawAddr, segNum);
                    }
                }
            } else if (rawAddr > 0x10000) {
                // Looks like a host pointer. Check if it's an OTR path string.
                const char* asStr = (const char*)rawAddr;
                bool looksLikeString = false;
#ifdef _WIN32
                __try {
                    looksLikeString = (asStr[0] == '_' && asStr[1] == '_');
                } __except(EXCEPTION_EXECUTE_HANDLER) {
                    looksLikeString = false;
                }
#endif
                if (looksLikeString) {
                    childDL = ResourceMgr_LoadGfxByName(asStr);
                } else {
                    childDL = (Gfx*)rawAddr;
                }
            }

            if (childDL) {
                if (isBranch) {
                    WalkDisplayList(childDL, isTranslucent, depth + 1);
                    return;
                } else {
                    WalkDisplayList(childDL, isTranslucent, depth + 1);
                }
            }
            break;
        }

        case G_DL_OTR_FILEPATH: {
            localDLFPCount++;
            // 1-word OTR sub-DL call by file path.
            // (Matching interpreter's gfx_dl_otr_filepath_handler_custom which does NOT
            // advance cmd0 — the main loop advances by 1 word.)
            //   cmd[0].w0 = opcode << 24 | (push/branch << 16)
            //   cmd[0].w1 = (uintptr_t) file path string
            const char* path = (const char*)cmd[0].words.w1;
            bool isBranch = ((cmd[0].words.w0 >> 16) & 0x01) != 0;

            Gfx* childDL = nullptr;
            if (path && (uintptr_t)path > 0x10000) {
                childDL = ResourceMgr_LoadGfxByName(path);
            }

            if (childDL) {
                if (isBranch) {
                    WalkDisplayList(childDL, isTranslucent, depth + 1);
                    return;
                } else {
                    WalkDisplayList(childDL, isTranslucent, depth + 1);
                }
            } else if (path) {
                SPDLOG_WARN("[RTX] SceneGeometryExtractor: G_DL_OTR_FILEPATH failed to load: {}", path);
            }
            break;
        }

        case G_DL_OTR_HASH: {
            localDLHashCount++;
            // 2-word OTR sub-DL call by CRC hash.
            // The hash is stored entirely in cmd[1]: high 32 bits in w0, low 32 in w1.
            // (cmd[0].w1 is unused/zero for this variant.)
            // This matches the Fast3D interpreter's gfx_dl_otr_hash_handler_custom.
            uint64_t hash = ((uint64_t)(cmd[1].words.w0) << 32) | (uint64_t)(cmd[1].words.w1);
            bool isBranch = ((cmd[0].words.w0 >> 16) & 0x01) != 0;

            // Advance past the second word first (before any early-out).
            cmd++;
            cmdCount++;

            // Try the ResourceMgr path first (goes through name lookup).
            Gfx* childDL = ResourceMgr_LoadGfxByCRC(hash);

            // If that fails, try ResourceGetDataByCrc (the exact function the
            // Fast3D interpreter uses — goes through ResourceManager::LoadResource(crc)).
            if (!childDL) {
                childDL = (Gfx*)ResourceGetDataByCrc(hash);
            }

            // Log diagnostics for the first N failures
            static uint32_t s_dlHashFailCount = 0;
            if (childDL) {
                if (isBranch) {
                    WalkDisplayList(childDL, isTranslucent, depth + 1);
                    return;
                } else {
                    WalkDisplayList(childDL, isTranslucent, depth + 1);
                }
            } else {
                s_dlHashFailCount++;
                if (s_dlHashFailCount <= 20) {
                    const char* name = ResourceGetNameByCrc(hash);
                    SPDLOG_WARN("[RTX] SceneGeometryExtractor: G_DL_OTR_HASH #{} failed to load hash 0x{:016X} "
                                "(name='{}', cmd[0].w0=0x{:X} w1=0x{:X}, cmd[1].w0=0x{:X} w1=0x{:X})",
                                s_dlHashFailCount, hash,
                                name ? name : "(null)",
                                (uint64_t)(cmd[-1].words.w0), (uint64_t)(cmd[-1].words.w1),
                                (uint64_t)(cmd[0].words.w0), (uint64_t)(cmd[0].words.w1));
                }
            }
            break;
        }

        case G_ENDDL:
            // End of this display list.
            if (m_totalDLsWalked <= 30 || localTriCount > 0 || localVtxCount > 0 || localVtxHashCount > 0 || localVtxFPCount > 0) {
                RTX_DIAG("SceneGeometryExtractor: DL #%u done: %u cmds, %u tris, %u G_VTX, %u VTX_HASH, %u VTX_FP, %u DL_HASH, %u DL_FP",
                         m_totalDLsWalked, cmdCount, localTriCount, localVtxCount,
                         localVtxHashCount, localVtxFPCount, localDLHashCount, localDLFPCount);
            }
            return;

        // ---- G_LOADTLUT: the previous G_SETTIMG set the texture address to the TLUT.
        //      Save it as lastTlutAddr so CI textures can reference it.
        //      Then RESTORE textureAddr to the pre-TLUT value (prevTextureAddr).
        //      N64 DL sequence for CI textures:
        //        G_SETTIMG(actual texture)   ← textureAddr = texture, prevTextureAddr = old
        //        G_SETTIMG(TLUT palette)     ← textureAddr = TLUT,    prevTextureAddr = texture
        //        G_LOADTLUT                  ← save TLUT, restore textureAddr = prevTextureAddr = texture
        //      Without the restore, textureAddr would remain pointing to the TLUT palette,
        //      causing the material to use the palette as its texture (visually wrong).
        case G_LOADTLUT:
            // Save current textureAddr (which is the TLUT address) to lastTlutAddr
            m_materialState.lastTlutAddr = m_materialState.textureAddr;
            // Restore textureAddr, texFormat, and texSize to the actual texture values
            // from before the TLUT G_SETTIMG overwrote them.
            if (m_materialState.prevTextureAddr != 0) {
                m_materialState.textureAddr = m_materialState.prevTextureAddr;
                m_materialState.texFormat = m_materialState.prevTexFormat;
                m_materialState.texSize = m_materialState.prevTexSize;
            }
            {
                static uint32_t s_tlutCount = 0;
                s_tlutCount++;
                if (s_tlutCount <= 20) {
                    RTX_DIAG("G_LOADTLUT #%u: saving TLUT addr=0x%llX, restoring textureAddr=0x%llX fmt=%u siz=%u (from prev)",
                             s_tlutCount,
                             (unsigned long long)m_materialState.lastTlutAddr,
                             (unsigned long long)m_materialState.textureAddr,
                             m_materialState.texFormat, m_materialState.texSize);
                }
            }
            break;

        // ---- G_TEXTURE: texture scaling and enable/disable ----
        case G_TEXTURE: {
            // G_TEXTURE (0xD7 in F3DEX2) — Set texture scaling and enable/disable.
            //   w0: [23:16] bowtie/xparam  [13:11] level  [10:8] tile  [7:0] on (F3DEX2)
            //   w1: [31:16] scaleS (Q0.16 fixed-point)  [15:0] scaleT (Q0.16 fixed-point)
            //
            // The RSP multiplies each vertex's texture coordinates by these scale
            // factors before rasterization. Most OoT geometry uses 0xFFFF (1.0).
            // Some effects (e.g., animated water, LOD) use smaller values to
            // compress UVs into a subrange of the texture.
            // We convert to float: scale = rawScale / 65536.0f
            // The "on" value enables/disables texturing. When disabled, the combiner
            // should use vertex color only (SHADE mode), but we don't enforce this
            // since the combiner mode already captures this intent.
            //
            // F3DEX2: on is in bits [7:0] (8 bits), typically G_ON=1 or G_OFF=0.
            // F3DEX1: on is in bits [7:1] (7 bits), so G_ON=1 appears as 0x02.
            // We check the low byte for any non-zero value for compatibility.
            uint16_t rawScaleS = (uint16_t)((cmd->words.w1 >> 16) & 0xFFFF);
            uint16_t rawScaleT = (uint16_t)(cmd->words.w1 & 0xFFFF);
            bool texOn = (cmd->words.w0 & 0xFF) != 0;
            // Only update scale if enabled and non-zero (avoid multiplying by zero later)
            if (texOn && (rawScaleS > 0 || rawScaleT > 0)) {
                m_materialState.texScaleS = (float)rawScaleS / 65536.0f;
                m_materialState.texScaleT = (float)rawScaleT / 65536.0f;
                m_materialState.texEnabled = true;
            } else if (!texOn) {
                m_materialState.texEnabled = false;
                // Keep scale values from the last enabled state
            }
            break;
        }

        case G_SETENVCOLOR:
            m_materialState.envColorR = (uint8_t)((cmd->words.w1 >> 24) & 0xFF);
            m_materialState.envColorG = (uint8_t)((cmd->words.w1 >> 16) & 0xFF);
            m_materialState.envColorB = (uint8_t)((cmd->words.w1 >> 8) & 0xFF);
            m_materialState.envColorA = (uint8_t)(cmd->words.w1 & 0xFF);
            m_materialState.hasEnvColor = true;
            break;

        case G_SETPRIMCOLOR:
            m_materialState.primColorR = (uint8_t)((cmd->words.w1 >> 24) & 0xFF);
            m_materialState.primColorG = (uint8_t)((cmd->words.w1 >> 16) & 0xFF);
            m_materialState.primColorB = (uint8_t)((cmd->words.w1 >> 8) & 0xFF);
            m_materialState.primColorA = (uint8_t)(cmd->words.w1 & 0xFF);
            m_materialState.hasPrimColor = true;
            break;

        // ---- Commands we recognize but don't need to act on ----
        case G_RDPPIPESYNC:
        case G_LOADBLOCK:
        case G_NOOP:
        case G_RDPFULLSYNC:
        case G_RDPTILESYNC:
        case G_RDPLOADSYNC:
        case G_SETFOGCOLOR:
        case G_SETBLENDCOLOR:
        case G_SETFILLCOLOR:
        case G_SETSCISSOR:
        case G_SETZIMG:
        case G_SETCIMG:
        case G_LOADTILE:
            break;
        case G_SETOTHERMODE_H:
        case G_RDPHALF_1:
        case G_RDPHALF_2:
        case G_POPMTX:
        case G_MTX:
        case G_MOVEWORD:
        case G_MOVEMEM:
        case G_MODIFYVTX:
        case G_CULLDL:
        case G_BRANCH_Z:
            // Recognized but no action needed for geometry extraction.
            break;

        // ---- OTR multi-word commands we skip over ----
        case G_MTX_OTR:
        case G_MOVEMEM_OTR:
            // These are 2-word OTR commands (the interpreter handler advances cmd0++);
            // skip the extra word.
            cmd++;
            cmdCount++;
            break;

        case G_MTX_OTR_FILEPATH:
            // 1-word OTR command (the interpreter handler does NOT advance cmd0).
            // No extra words to skip.
            break;

        // ---- OTR triangle command ----
        case G_TRI1_OTR:
            // G_TRI1_OTR uses the same vertex index encoding as F3DEX2 G_TRI1
            // (vertex indices * 2 packed into w0), with w1 = 0.
            // The GBI macro gsSP1Triangle_OTR encodes via __gsSP1Triangle_w1f
            // which is the same as gsSP1Triangle in F3DEX_GBI_2 mode.
            HandleTri1(*cmd, isTranslucent);
            break;

        case G_INVALTEXCACHE:
        case G_PUSHCD:
        case G_SETFB:
        case G_RESETFB:
        case G_SETTIMG_FB:
            // OTR-specific single-word commands (their interpreter handlers do NOT
            // advance cmd0). Safe to skip.
            break;

        case G_MARKER:
            // 2-word OTR command (the interpreter handler advances cmd0++).
            cmd++;
            cmdCount++;
            break;

        case G_BRANCH_Z_OTR:
        case G_FILLWIDERECT:
        case G_REGBLENDEDTEX:
            // 2-word OTR commands (the interpreter handlers advance cmd0 once).
            // Skip the extra word.
            cmd++;
            cmdCount++;
            break;

        case G_TEXRECT_WIDE:
        case G_IMAGERECT:
            // 3-word OTR commands (the interpreter handlers advance cmd0 twice).
            cmd += 2;
            cmdCount += 2;
            break;

        default:
            // Unknown opcode. Log at trace level to avoid spamming.
            SPDLOG_TRACE("[RTX] SceneGeometryExtractor: unknown opcode 0x{:02X} at DL offset {}",
                         opcode, cmdCount);
            break;
        }
    }

    if (cmdCount >= MAX_COMMANDS_PER_DL) {
        RTX_DIAG("SceneGeometryExtractor: HIT MAX COMMAND LIMIT (%u) in DL #%u at %p (tris=%u)",
                 MAX_COMMANDS_PER_DL, m_totalDLsWalked, (const void*)dl, localTriCount);
        SPDLOG_WARN("[RTX] SceneGeometryExtractor: hit max command limit ({}) in DL at {:p}",
                     MAX_COMMANDS_PER_DL, (const void*)dl);
    }
}

void SceneGeometryExtractor::WalkDisplayList(const Gfx* dl, bool isTranslucent, uint32_t depth) {
    if (!dl) {
        static uint32_t s_nullDLCount = 0;
        s_nullDLCount++;
        if (s_nullDLCount <= 5) {
            RTX_DIAG("SceneGeometryExtractor::WalkDisplayList() null DL pointer (depth=%u, skip #%u)", depth, s_nullDLCount);
        }
        return;
    }

    if (depth >= MAX_DL_RECURSION_DEPTH) {
        RTX_DIAG("SceneGeometryExtractor::WalkDisplayList() MAX recursion depth %u reached at %p, stopping", depth, (const void*)dl);
        SPDLOG_WARN("[RTX] SceneGeometryExtractor: max DL recursion depth {} reached, stopping", depth);
        return;
    }

#ifdef _WIN32
    // Wrap in SEH to catch access violations from bad display list pointers.
    // OTR resource pointers can be stale or point to unmapped memory if the
    // resource didn't load correctly.
    __try {
        WalkDisplayListInner(dl, isTranslucent, depth);
    } __except(GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        RTX_DIAG("SceneGeometryExtractor: ACCESS VIOLATION walking DL at %p (depth=%u), skipping", (const void*)dl, depth);
        SPDLOG_ERROR("[RTX] SceneGeometryExtractor: access violation walking DL at {:p} (depth {}), skipping",
                     (const void*)dl, depth);
    }
#else
    WalkDisplayListInner(dl, isTranslucent, depth);
#endif
}

// ============================================================================
// Vertex loading
// ============================================================================

void SceneGeometryExtractor::HandleVertexLoad(const Gfx& cmd) {
    // F3DEX2 G_VTX encoding:
    //   w0: [31:24] opcode  [23:12] n (num verts * 2, or shifted)  [6:1] v0+n
    //   Actually: n = (w0 >> 12) & 0xFF, v0 = ((w0 >> 1) & 0x7F) - n
    //   w1: pointer to Vtx array (may be a segment address in the OTR build)
    uint32_t n  = (cmd.words.w0 >> 12) & 0xFF;
    uint32_t v0raw = (cmd.words.w0 >> 1) & 0x7F;
    // Guard against underflow: if n > v0raw, v0idx would wrap to a huge value
    if (n > v0raw) {
        static uint32_t s_vtxUnderflowCount = 0;
        s_vtxUnderflowCount++;
        if (s_vtxUnderflowCount <= 5) {
            SPDLOG_WARN("[RTX] SceneGeometryExtractor: G_VTX underflow: v0raw={} < n={}", v0raw, n);
        }
        return;
    }
    uint32_t v0idx = v0raw - n;

    // Resolve w1 using the same logic as the Fast3D interpreter
    uintptr_t rawAddr = (uintptr_t)cmd.words.w1;
    Vtx* vtxData = nullptr;
    if (rawAddr & 1) {
        vtxData = (Vtx*)ResolveSegAddr(rawAddr);
    } else if (rawAddr != 0) {
        vtxData = (Vtx*)rawAddr;
    }

    if (!vtxData) {
        static uint32_t s_vtxNullCount = 0;
        s_vtxNullCount++;
        if (s_vtxNullCount <= 5) {
            SPDLOG_WARN("[RTX] SceneGeometryExtractor: G_VTX null vertex pointer (w1=0x{:X})", rawAddr);
        }
        return;
    }

    if (v0idx + n > N64_VERTEX_BUFFER_SIZE) {
        SPDLOG_WARN("[RTX] SceneGeometryExtractor: G_VTX out of bounds: v0={} n={} (max {})",
                     v0idx, n, N64_VERTEX_BUFFER_SIZE);
        n = (v0idx < N64_VERTEX_BUFFER_SIZE) ? (N64_VERTEX_BUFFER_SIZE - v0idx) : 0;
    }

    for (uint32_t i = 0; i < n; i++) {
        m_vertexBuffer[v0idx + i] = vtxData[i];
        m_vertexBufferValid[v0idx + i] = true;
    }

    m_totalVerticesLoaded += n;
}

uint32_t SceneGeometryExtractor::HandleVertexLoadOTRFilePath(const Gfx* cmds) {
    if (!cmds) return 1;
    // 2-word command:
    //   cmds[0].w0 = opcode << 24
    //   cmds[0].w1 = (uintptr_t) file path string
    //   cmds[1].w0 = vertex count (n)
    //   cmds[1].w1 = (bufferIndex << 16) | dataOffset
    //     bufferIndex is the start slot in the vertex buffer
    const char* path = (const char*)cmds[0].words.w1;
    uint32_t n = (uint32_t)cmds[1].words.w0;
    uint32_t bufferIndex = (uint32_t)((cmds[1].words.w1 >> 16) & 0xFFFF);
    // Sanity cap on vertex count to prevent huge out-of-bounds reads
    if (n > N64_VERTEX_BUFFER_SIZE) {
        SPDLOG_WARN("[RTX] SceneGeometryExtractor: G_VTX_OTR_FILEPATH unreasonable n={}, capping to {}", n, N64_VERTEX_BUFFER_SIZE);
        n = N64_VERTEX_BUFFER_SIZE;
    }
    // dataOffset can be used to offset into the resource's vertex array
    // but for most scene geometry it's 0; we pass the path to the resource manager.

    if (!path) {
        SPDLOG_WARN("[RTX] SceneGeometryExtractor: G_VTX_OTR_FILEPATH null path");
        return 1;
    }

    Vtx* vtxData = ResourceMgr_LoadVtxByName((char*)path);
    if (!vtxData) {
        SPDLOG_WARN("[RTX] SceneGeometryExtractor: G_VTX_OTR_FILEPATH failed to load: {}", path);
        return 1;
    }

    uint32_t v0 = bufferIndex;
    if (v0 + n > N64_VERTEX_BUFFER_SIZE) {
        SPDLOG_WARN("[RTX] SceneGeometryExtractor: G_VTX_OTR_FILEPATH out of bounds: v0={} n={}", v0, n);
        n = (v0 < N64_VERTEX_BUFFER_SIZE) ? (N64_VERTEX_BUFFER_SIZE - v0) : 0;
    }

    // Apply data offset (lower 16 bits of cmds[1].w1) to index into the vertex array.
    // dataOffset is a vertex index into the resource's vertex array (NOT byte offset here).
    // Sanity check: if dataOffset is unreasonably large, skip to avoid reading way past
    // the end of the vertex resource. Most OoT vertex resources have < 1000 vertices.
    uint32_t dataOffset = (uint32_t)(cmds[1].words.w1 & 0xFFFF);
    if (dataOffset > 4096) {
        SPDLOG_WARN("[RTX] SceneGeometryExtractor: G_VTX_OTR_FILEPATH unreasonable dataOffset={}", dataOffset);
        return 1;
    }
    Vtx* srcVerts = vtxData + dataOffset;

    for (uint32_t i = 0; i < n; i++) {
        m_vertexBuffer[v0 + i] = srcVerts[i];
        m_vertexBufferValid[v0 + i] = true;
    }

    m_totalVerticesLoaded += n;

    SPDLOG_TRACE("[RTX] SceneGeometryExtractor: G_VTX_OTR_FILEPATH loaded {} verts at slot {} from {}",
                 n, v0, path);

    return 1; // consumed 1 extra Gfx word
}

uint32_t SceneGeometryExtractor::HandleVertexLoadOTRHash(const Gfx* cmds) {
    if (!cmds) return 1;
    // 2-word command (matching Fast3D gfx_vtx_hash_handler_custom):
    //   cmds[0].w0 = opcode << 24 | (numVerts << 12) | ((v0+n) << 1)
    //   cmds[0].w1 = byte offset into the vertex resource array
    //   cmds[1].w0 = high 32 bits of CRC hash
    //   cmds[1].w1 = low 32 bits of CRC hash
    //
    //   n = (cmds[0].w0 >> 12) & 0xFF
    //   v0 = ((cmds[0].w0 >> 1) & 0x7F) - n  (same as regular G_VTX)

    uint32_t n = (uint32_t)((cmds[0].words.w0 >> 12) & 0xFF);
    uint32_t v0raw = (uint32_t)((cmds[0].words.w0 >> 1) & 0x7F);
    // Guard against underflow: if n > v0raw, v0 would wrap to a huge value
    if (n > v0raw) {
        static uint32_t s_underflowCount = 0;
        s_underflowCount++;
        if (s_underflowCount <= 10) {
            SPDLOG_WARN("[RTX] SceneGeometryExtractor: G_VTX_OTR_HASH underflow: v0raw={} < n={}", v0raw, n);
        }
        return 1;
    }
    uint32_t v0 = v0raw - n;

    // Reconstruct CRC hash from cmd[1] (high in w0, low in w1)
    uint64_t hash = ((uint64_t)(cmds[1].words.w0) << 32) | (uint64_t)(cmds[1].words.w1);

    // cmds[0].w1 is a byte offset into the vertex resource array
    uintptr_t vtxByteOffset = (uintptr_t)cmds[0].words.w1;

    Vtx* vtxData = ResourceMgr_LoadVtxByCRC(hash);

    if (!vtxData) {
        // Try ResourceGetDataByCrc (same function the interpreter uses).
        vtxData = (Vtx*)ResourceGetDataByCrc(hash);
    }

    if (!vtxData) {
        // Try name-based fallback
        char nameBuf[256] = {};
        char* name = ResourceMgr_GetNameByCRC(hash, nameBuf);
        if (name && name[0] != '\0') {
            vtxData = ResourceMgr_LoadVtxByName(name);
        }
    }

    if (!vtxData) {
        static uint32_t s_vtxHashFailCount = 0;
        s_vtxHashFailCount++;
        if (s_vtxHashFailCount <= 20) {
            const char* name = ResourceGetNameByCrc(hash);
            SPDLOG_WARN("[RTX] SceneGeometryExtractor: G_VTX_OTR_HASH #{} failed to load hash 0x{:016X} "
                        "(name='{}', offset=0x{:X}, n={}, v0={})",
                        s_vtxHashFailCount, hash,
                        name ? name : "(null)",
                        (uint64_t)vtxByteOffset, n, v0);
        }
        return 1;
    }

    // Apply byte offset to get the starting vertex pointer
    Vtx* srcVerts = (Vtx*)((uint8_t*)vtxData + vtxByteOffset);

    if (v0 + n > N64_VERTEX_BUFFER_SIZE) {
        SPDLOG_WARN("[RTX] SceneGeometryExtractor: G_VTX_OTR_HASH out of bounds: v0={} n={}", v0, n);
        n = (v0 < N64_VERTEX_BUFFER_SIZE) ? (N64_VERTEX_BUFFER_SIZE - v0) : 0;
    }

    for (uint32_t i = 0; i < n; i++) {
        m_vertexBuffer[v0 + i] = srcVerts[i];
        m_vertexBufferValid[v0 + i] = true;
    }

    m_totalVerticesLoaded += n;

    return 1; // consumed 1 extra Gfx word
}

// ============================================================================
// Triangle emission
// ============================================================================

void SceneGeometryExtractor::HandleTri1(const Gfx& cmd, bool isTranslucent) {
    // F3DEX2 G_TRI1:
    //   w0: [23:16] v0*2  [15:8] v1*2  [7:0] v2*2
    uint32_t vi0 = ((cmd.words.w0 >> 16) & 0xFF) / 2;
    uint32_t vi1 = ((cmd.words.w0 >>  8) & 0xFF) / 2;
    uint32_t vi2 = ((cmd.words.w0      ) & 0xFF) / 2;

    EmitTriangle(vi0, vi1, vi2, isTranslucent);
}

void SceneGeometryExtractor::HandleTri2(const Gfx& cmd, bool isTranslucent) {
    // F3DEX2 G_TRI2: two triangles packed into one command.
    //   First tri in w0:  [23:16] v0*2  [15:8] v1*2  [7:0] v2*2
    //   Second tri in w1: [23:16] v3*2  [15:8] v4*2  [7:0] v5*2
    uint32_t vi0 = ((cmd.words.w0 >> 16) & 0xFF) / 2;
    uint32_t vi1 = ((cmd.words.w0 >>  8) & 0xFF) / 2;
    uint32_t vi2 = ((cmd.words.w0      ) & 0xFF) / 2;

    uint32_t vi3 = ((cmd.words.w1 >> 16) & 0xFF) / 2;
    uint32_t vi4 = ((cmd.words.w1 >>  8) & 0xFF) / 2;
    uint32_t vi5 = ((cmd.words.w1      ) & 0xFF) / 2;

    EmitTriangle(vi0, vi1, vi2, isTranslucent);
    EmitTriangle(vi3, vi4, vi5, isTranslucent);
}

void SceneGeometryExtractor::EmitTriangle(uint32_t vi0, uint32_t vi1, uint32_t vi2, bool isTranslucent) {
    // Validate output mesh pointers are set (they should be during extraction).
    if (!m_opaqueMesh || !m_alphaMesh) {
        static uint32_t s_nullMeshCount = 0;
        s_nullMeshCount++;
        if (s_nullMeshCount <= 5) {
            RTX_DIAG("SceneGeometryExtractor::EmitTriangle: null mesh pointer (opaque=%p, alpha=%p), skipping",
                     (void*)m_opaqueMesh, (void*)m_alphaMesh);
        }
        return;
    }

    // Validate vertex buffer indices.
    if (vi0 >= N64_VERTEX_BUFFER_SIZE || vi1 >= N64_VERTEX_BUFFER_SIZE || vi2 >= N64_VERTEX_BUFFER_SIZE) {
        static uint32_t s_oobCount = 0;
        s_oobCount++;
        if (s_oobCount <= 10) {
            RTX_DIAG("SceneGeometryExtractor: triangle OOB indices %u,%u,%u (max=%u) #%u", vi0, vi1, vi2, N64_VERTEX_BUFFER_SIZE - 1, s_oobCount);
        }
        SPDLOG_WARN("[RTX] SceneGeometryExtractor: triangle index out of range: {}, {}, {} (max {})",
                     vi0, vi1, vi2, N64_VERTEX_BUFFER_SIZE - 1);
        return;
    }

    if (!m_vertexBufferValid[vi0] || !m_vertexBufferValid[vi1] || !m_vertexBufferValid[vi2]) {
        static uint32_t s_invalidCount = 0;
        s_invalidCount++;
        if (s_invalidCount <= 10) {
            RTX_DIAG("SceneGeometryExtractor: triangle references unloaded vertices %u(v=%d),%u(v=%d),%u(v=%d) #%u",
                     vi0, m_vertexBufferValid[vi0], vi1, m_vertexBufferValid[vi1], vi2, m_vertexBufferValid[vi2], s_invalidCount);
        }
        SPDLOG_WARN("[RTX] SceneGeometryExtractor: triangle references unloaded vertex slot(s): {}, {}, {}",
                     vi0, vi1, vi2);
        return;
    }

    // Determine whether this triangle goes into the alpha or opaque mesh.
    // Alpha test is determined by the current render mode (otherModeL) or
    // by the fact that the DL is from the translucent (xlu) list.
    bool isAlpha = isTranslucent || m_materialState.alphaTest || DetectAlphaTest(m_materialState.otherModeL);

    ExtractedMesh& targetMesh = isAlpha ? *m_alphaMesh : *m_opaqueMesh;

    // Get or create a material for the current state.
    uint32_t matID = GetOrCreateMaterial(targetMesh);

    // Convert the three N64 vertices and append to the mesh.
    uint32_t baseIndex = (uint32_t)targetMesh.vertices.size();

    targetMesh.vertices.push_back(ConvertVertex(m_vertexBuffer[vi0]));
    targetMesh.vertices.push_back(ConvertVertex(m_vertexBuffer[vi1]));
    targetMesh.vertices.push_back(ConvertVertex(m_vertexBuffer[vi2]));

    // Compute face normal from winding order for vertices that don't have proper normals.
    // When lighting is disabled, ConvertVertex sets normals to a default (0,1,0) up vector,
    // which is wrong for non-horizontal faces. Even when lighting IS enabled, the N64 vertex
    // normals are per-vertex and may be zero-length or inaccurate. Computing the face normal
    // from the triangle winding order provides a correct geometric normal for lighting.
    // This is critical for RTX path tracing where normals drive diffuse/specular shading.
    {
        RTXVertex& v0 = targetMesh.vertices[baseIndex + 0];
        RTXVertex& v1 = targetMesh.vertices[baseIndex + 1];
        RTXVertex& v2 = targetMesh.vertices[baseIndex + 2];

        // Edge vectors
        float e1x = v1.position[0] - v0.position[0];
        float e1y = v1.position[1] - v0.position[1];
        float e1z = v1.position[2] - v0.position[2];
        float e2x = v2.position[0] - v0.position[0];
        float e2y = v2.position[1] - v0.position[1];
        float e2z = v2.position[2] - v0.position[2];

        // Cross product: normal = e1 x e2
        float nx = e1y * e2z - e1z * e2y;
        float ny = e1z * e2x - e1x * e2z;
        float nz = e1x * e2y - e1y * e2x;

        // Normalize
        float len = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (len > 1e-7f) {
            nx /= len;
            ny /= len;
            nz /= len;
        } else {
            // Degenerate triangle - use up vector as fallback
            nx = 0.0f;
            ny = 1.0f;
            nz = 0.0f;
        }

        // For non-lit vertices (lighting disabled), always use the computed face normal
        // since ConvertVertex only provides a placeholder (0,1,0).
        // For lit vertices, use the computed face normal if the vertex normal is zero-length
        // (some OoT geometry has zero normals even with lighting enabled).
        auto applyFaceNormal = [&](RTXVertex& v) {
            float vnLen = v.normal[0] * v.normal[0] + v.normal[1] * v.normal[1] + v.normal[2] * v.normal[2];
            bool isDefaultUp = (!m_materialState.lightingEnabled) ||
                               (vnLen < 0.01f); // Zero or near-zero vertex normal
            if (isDefaultUp) {
                v.normal[0] = nx;
                v.normal[1] = ny;
                v.normal[2] = nz;
            }
        };
        applyFaceNormal(v0);
        applyFaceNormal(v1);
        applyFaceNormal(v2);

        // ================================================================
        // Decal Z-bias: offset decal/overlay geometry slightly along the
        // face normal to prevent co-planar Z-fighting with the ground.
        //
        // In rasterized rendering, ZMODE_DEC adjusts the Z comparison to
        // favor the decal over the surface below. In ray tracing, there's
        // no Z-buffer — the closest intersection wins. Co-planar triangles
        // cause non-deterministic hit selection (50/50 ground vs decal)
        // depending on floating-point precision.
        //
        // We apply a small offset (0.25 N64 units ≈ 0.25cm) along the
        // face normal. This is small enough to be invisible but large
        // enough to ensure the ray consistently hits the decal first.
        // A slightly larger bias (0.25 vs 0.1) is needed because ray-triangle
        // intersection with co-planar geometry has more precision issues than
        // rasterized Z-testing.
        //
        // This fixes dirt path overlays in Kokiri Forest, shadow decals,
        // and other ground-overlay textures that weren't rendering.
        // ================================================================
        bool isDecalTri = DetectDecalMode(m_materialState.otherModeL);
        if (isDecalTri) {
            constexpr float DECAL_Z_BIAS = 0.25f; // Small offset in N64 world units
            v0.position[0] += nx * DECAL_Z_BIAS;
            v0.position[1] += ny * DECAL_Z_BIAS;
            v0.position[2] += nz * DECAL_Z_BIAS;
            v1.position[0] += nx * DECAL_Z_BIAS;
            v1.position[1] += ny * DECAL_Z_BIAS;
            v1.position[2] += nz * DECAL_Z_BIAS;
            v2.position[0] += nx * DECAL_Z_BIAS;
            v2.position[1] += ny * DECAL_Z_BIAS;
            v2.position[2] += nz * DECAL_Z_BIAS;
        }
    }

    targetMesh.indices.push_back(baseIndex + 0);
    targetMesh.indices.push_back(baseIndex + 1);
    targetMesh.indices.push_back(baseIndex + 2);

    targetMesh.materialIDs.push_back(matID);

    m_totalTrianglesEmitted++;
}

// ============================================================================
// Vertex conversion
// ============================================================================

RTXVertex SceneGeometryExtractor::ConvertVertex(const Vtx& v) const {
    RTXVertex out;

    // Position: int16 -> float (world-space units).
    out.position[0] = (float)v.v.ob[0];
    out.position[1] = (float)v.v.ob[1];
    out.position[2] = (float)v.v.ob[2];

    // Normal or vertex color. When lighting is enabled, cn[0..2] contain packed
    // normals (signed bytes). Otherwise, they are vertex colors.
    if (m_materialState.lightingEnabled) {
        // Interpret cn[0..2] as signed normals in [-127, 127].
        out.normal[0] = (float)((int8_t)v.v.cn[0]) / 127.0f;
        out.normal[1] = (float)((int8_t)v.v.cn[1]) / 127.0f;
        out.normal[2] = (float)((int8_t)v.v.cn[2]) / 127.0f;
    } else {
        // No lighting: use a default up normal.
        out.normal[0] = 0.0f;
        out.normal[1] = 1.0f;
        out.normal[2] = 0.0f;
    }

    // Texture coordinates: S10.5 fixed-point -> normalized [0,1] for DX12 sampling.
    // Step 1: Convert from S10.5 fixed-point to texel space (divide by 32).
    // Step 2: Apply G_TEXTURE scale factors (scaleS, scaleT from Q0.16 fixed-point).
    //         The N64 RSP multiplies vertex TCs by these scale factors before rasterization.
    //         Most OoT geometry uses scale=1.0 (0xFFFF), but some effects use fractional scales.
    // Step 3: Apply tile shift from G_SETTILE (power-of-2 TC scaling).
    //         shift < 11: divide by 2^shift  (e.g., shift=1 → /2, shift=5 → /32)
    //         shift >= 11: multiply by 2^(16-shift)  (e.g., shift=15 → *2, shift=11 → *32)
    //         shift == 0: no modification (most common in OoT)
    //         This is applied by the RDP after the RSP's G_TEXTURE scale.
    // Step 4: Normalize to [0,1] by dividing by the current tile dimensions.
    // This matches the Fast3D interpreter which does: u = (tc/32.0 * scale) / tex_width
    // The result is [0,1] normalized UVs suitable for DX12 texture sampling with WRAP mode.
    float texelU = (float)v.v.tc[0] / 32.0f;
    float texelV = (float)v.v.tc[1] / 32.0f;

    // Apply G_TEXTURE scale (default 1.0; fractional values compress UVs)
    texelU *= m_materialState.texScaleS;
    texelV *= m_materialState.texScaleT;

    // Apply tile shift (power-of-2 TC scaling from G_SETTILE shift_s/shift_t).
    // Most OoT textures use shift=0 (no modification), so this is typically a no-op.
    // When non-zero, it provides an additional power-of-2 scale to the texture coordinates.
    if (m_materialState.tileShiftS != 0) {
        if (m_materialState.tileShiftS < 11) {
            texelU /= (float)(1 << m_materialState.tileShiftS);
        } else {
            texelU *= (float)(1 << (16 - m_materialState.tileShiftS));
        }
    }
    if (m_materialState.tileShiftT != 0) {
        if (m_materialState.tileShiftT < 11) {
            texelV /= (float)(1 << m_materialState.tileShiftT);
        } else {
            texelV *= (float)(1 << (16 - m_materialState.tileShiftT));
        }
    }

    float texW = (float)m_materialState.texWidth;
    float texH = (float)m_materialState.texHeight;
    // Guard against division by zero if texWidth/texHeight tracking failed.
    // Default to 32 (the most common N64 texture size) as fallback.
    out.uv[0] = (texW > 0.0f) ? (texelU / texW) : (texelU / 32.0f);
    out.uv[1] = (texH > 0.0f) ? (texelV / texH) : (texelV / 32.0f);

    // Protect against NaN/Inf UVs from degenerate vertex data or bad texture scale.
    // This can happen if v.v.tc[] contains extreme values, or if texScaleS/T is 0.
    if (std::isnan(out.uv[0]) || std::isinf(out.uv[0])) out.uv[0] = 0.0f;
    if (std::isnan(out.uv[1]) || std::isinf(out.uv[1])) out.uv[1] = 0.0f;

    // Vertex color RGBA [0,1].
    // When lighting is enabled, cn[0..2] are normals (signed bytes), not colors.
    // The N64 RSP would compute lighting and place the result in the vertex color,
    // but we're reading raw vertex data before RSP processing. So when lighting is
    // enabled, we set color to white (1,1,1) and let the shader compute lighting.
    // Alpha (cn[3]) is always valid regardless of lighting mode.
    if (m_materialState.lightingEnabled) {
        out.color[0] = 1.0f;
        out.color[1] = 1.0f;
        out.color[2] = 1.0f;
        out.color[3] = v.v.cn[3] / 255.0f;
    } else {
        out.color[0] = v.v.cn[0] / 255.0f;
        out.color[1] = v.v.cn[1] / 255.0f;
        out.color[2] = v.v.cn[2] / 255.0f;
        out.color[3] = v.v.cn[3] / 255.0f;
    }

    return out;
}

// ============================================================================
// Material state command handlers
// ============================================================================

void SceneGeometryExtractor::HandleSetCombine(const Gfx& cmd) {
    // G_SETCOMBINE (0xFC):
    //   The full 64-bit combiner mode is packed across w0 and w1.
    //   w0[23:0] = upper 24 bits of combiner mode
    //   w1 = lower 32 bits of combiner mode
    uint64_t upper = (uint64_t)(cmd.words.w0 & 0x00FFFFFF);
    uint64_t lower = (uint64_t)(cmd.words.w1);
    m_materialState.combinerMode = (upper << 32) | lower;
}

void SceneGeometryExtractor::HandleSetTextureImage(const Gfx& cmd) {
    // G_SETTIMG (0xFD):
    //   w0[23:21] = format, w0[20:19] = size, w0[11:0] = width-1
    //   w1 = address of texture data (on PC/OTR this is a pointer or OTR address)
    m_materialState.prevTextureAddr = m_materialState.textureAddr;
    m_materialState.prevTexFormat = m_materialState.texFormat;
    m_materialState.prevTexSize = m_materialState.texSize;
    m_materialState.textureAddr = cmd.words.w1;
    m_materialState.texFormat = (uint8_t)((cmd.words.w0 >> 21) & 0x07);
    m_materialState.texSize = (uint8_t)((cmd.words.w0 >> 19) & 0x03);

    // Log texture intercept info for first N textures
    {
        static uint32_t s_setTimgCount = 0;
        s_setTimgCount++;
        if (s_setTimgCount <= 30) {
            bool isStr = false;
#ifdef _WIN32
            if (cmd.words.w1 > 0x10000) {
                __try {
                    const char* s = (const char*)cmd.words.w1;
                    isStr = (s[0] == '_' && s[1] == '_');
                } __except(EXCEPTION_EXECUTE_HANDLER) {
                    isStr = false;
                }
            }
#endif
            RTX_DiagLog("G_SETTIMG #%u: addr=0x%llX fmt=%u siz=%u isOTRStr=%s",
                         s_setTimgCount, (unsigned long long)cmd.words.w1,
                         m_materialState.texFormat, m_materialState.texSize,
                         isStr ? "YES" : "NO");
        }
    }
}

void SceneGeometryExtractor::HandleSetTextureImageOTRFilePath(const Gfx* cmds) {
    if (!cmds) return;
    // 1-word command (matching the interpreter's gfx_set_timg_otr_filepath_handler_custom):
    //   cmds[0].w0 = opcode << 24 | fmt << 21 | size << 19 | (width-1)
    //   cmds[0].w1 = (uintptr_t) file path string
    // The interpreter does NOT advance cmd0, so this is a single-word command.
    const char* path = (const char*)cmds[0].words.w1;

    if (path) {
        m_materialState.prevTextureAddr = m_materialState.textureAddr;
        m_materialState.prevTexFormat = m_materialState.texFormat;
        m_materialState.prevTexSize = m_materialState.texSize;
        m_materialState.textureAddr = (uintptr_t)path;
        // Also extract format info from w0 (same encoding as G_SETTIMG)
        m_materialState.texFormat = (uint8_t)((cmds[0].words.w0 >> 21) & 0x07);
        m_materialState.texSize = (uint8_t)((cmds[0].words.w0 >> 19) & 0x03);
        static uint32_t s_filePathCount = 0;
        s_filePathCount++;
        if (s_filePathCount <= 30) {
            RTX_DIAG("G_SETTIMG_OTR_FILEPATH #%u: path='%.120s' addr=0x%llX fmt=%u",
                     s_filePathCount, path, (unsigned long long)(uintptr_t)path, m_materialState.texFormat);
        }
        SPDLOG_TRACE("[RTX] SceneGeometryExtractor: G_SETTIMG_OTR_FILEPATH: {}", path);
    } else {
        RTX_DIAG("G_SETTIMG_OTR_FILEPATH: NULL path!");
        SPDLOG_WARN("[RTX] SceneGeometryExtractor: G_SETTIMG_OTR_FILEPATH null path");
    }
}

uint32_t SceneGeometryExtractor::HandleSetTextureImageOTRHash(const Gfx* cmds) {
    if (!cmds) return 1;
    // 2-word command: reconstruct the hash from cmd[1] (high in w0, low in w1).
    // Matches Fast3D interpreter convention.
    uint64_t hash = ((uint64_t)(cmds[1].words.w0) << 32) | (uint64_t)(cmds[1].words.w1);

    // Save previous texture address and format before we overwrite them.
    // This is used by G_LOADTLUT to restore textureAddr after the TLUT is captured.
    m_materialState.prevTextureAddr = m_materialState.textureAddr;
    m_materialState.prevTexFormat = m_materialState.texFormat;
    m_materialState.prevTexSize = m_materialState.texSize;

    // Extract format info from cmds[0].w0 (same encoding as G_SETTIMG:
    //   w0[23:21] = format, w0[20:19] = size).
    // NOTE: The OTR HASH variant may not always have valid format bits in w0
    // (depends on how the display list factory emits it). We only update
    // format/size if the bits look valid (non-zero or matching known patterns).
    {
        uint8_t fmt = (uint8_t)((cmds[0].words.w0 >> 21) & 0x07);
        uint8_t siz = (uint8_t)((cmds[0].words.w0 >> 19) & 0x03);
        // Only update if at least one is non-zero, to avoid overwriting valid info
        // from a preceding G_SETTIMG with garbage zeros.
        if (fmt != 0 || siz != 0) {
            m_materialState.texFormat = fmt;
            m_materialState.texSize = siz;
        }
    }

    // Try to resolve the hash to a name. If we can, store the name as a
    // persistent string so it survives until ResolveMaterialTextures runs.
    // ResourceMgr_GetNameByCRC writes into the provided buffer which is temporary,
    // so we MUST copy the result into m_persistentPathStrings.
    char hashNameBuf[256];
    char* name = ResourceMgr_GetNameByCRC(hash, hashNameBuf);

    static uint32_t s_hashTexCount = 0;
    s_hashTexCount++;

    if (name && name[0] != '\0') {
        // Persist the string so the pointer stored in textureAddr remains valid
        m_persistentPathStrings.emplace_back(name);
        m_materialState.textureAddr = (uintptr_t)m_persistentPathStrings.back().c_str();
        if (s_hashTexCount <= 30) {
            RTX_DIAG("G_SETTIMG_OTR_HASH #%u: CRC=0x%llX -> '%s' (persisted)",
                     s_hashTexCount, (unsigned long long)hash, name);
        }
        SPDLOG_TRACE("[RTX] SceneGeometryExtractor: G_SETTIMG_OTR_HASH 0x{:016X} -> {}", hash, name);
    } else {
        // No name resolved. Try to load the resource data by CRC directly.
        const char* resName = ResourceGetNameByCrc(hash);
        if (resName && resName[0] != '\0') {
            m_persistentPathStrings.emplace_back(resName);
            m_materialState.textureAddr = (uintptr_t)m_persistentPathStrings.back().c_str();
            if (s_hashTexCount <= 30) {
                RTX_DIAG("G_SETTIMG_OTR_HASH #%u: CRC=0x%llX -> '%s' (via ResourceGetNameByCrc, persisted)",
                         s_hashTexCount, (unsigned long long)hash, resName);
            }
            SPDLOG_TRACE("[RTX] SceneGeometryExtractor: G_SETTIMG_OTR_HASH 0x{:016X} -> {} (via ResourceGetNameByCrc)", hash, resName);
        } else {
            // Store the raw hash value. ResolveMaterialTextures will not be able
            // to load this as an OTR path, but at least it's a non-zero value
            // that can be used for diagnostic purposes.
            m_materialState.textureAddr = (uintptr_t)hash;
            if (s_hashTexCount <= 30) {
                RTX_DIAG("G_SETTIMG_OTR_HASH #%u: CRC=0x%llX -> NO NAME FOUND (stored raw hash)",
                         s_hashTexCount, (unsigned long long)hash);
            }
            SPDLOG_TRACE("[RTX] SceneGeometryExtractor: G_SETTIMG_OTR_HASH 0x{:016X} (no name)", hash);
        }
    }

    return 1; // consumed 1 extra Gfx word
}

void SceneGeometryExtractor::HandleGeometryMode(const Gfx& cmd) {
    // G_GEOMETRYMODE (0xD9) in F3DEX2:
    //   w0[23:0] = AND mask (bits to keep; inverted clear mask)
    //   w1 = set bits (bits to OR in)
    //
    // F3DEX2 encoding (matching Fast3D interpreter gfx_geometry_mode_handler_f3dex2):
    //   andMask = w0 & 0x00FFFFFF
    //   newMode = (oldMode & andMask) | setBits
    uint32_t andMask = (uint32_t)(cmd.words.w0) & 0x00FFFFFF;
    uint32_t setBits = (uint32_t)(cmd.words.w1);

    m_materialState.geometryMode = (m_materialState.geometryMode & andMask) | setBits;
    m_materialState.lightingEnabled = (m_materialState.geometryMode & G_LIGHTING) != 0;
}

void SceneGeometryExtractor::HandleSetOtherModeL(const Gfx& cmd) {
    // G_SETOTHERMODE_L (0xE2) in F3DEX2:
    //   w0[7:0]  = len - 1
    //   w0[15:8] = 32 - sft - len
    //   w1 = data to set
    //
    // Decoding (matches Fast3D interpreter gfx_othermode_l_handler_f3dex2):
    //   len = (w0 & 0xFF) + 1
    //   sft = 31 - ((w0 >> 8) & 0xFF) - (w0 & 0xFF)
    //
    // Builds a mask from shift and length, then:
    //   otherModeL = (otherModeL & ~mask) | data
    uint32_t lenM1 = (uint32_t)(cmd.words.w0 & 0xFF);
    uint32_t ssub  = (uint32_t)((cmd.words.w0 >> 8) & 0xFF);
    uint32_t len   = lenM1 + 1;
    uint32_t sft   = 31 - ssub - lenM1;
    uint32_t data = (uint32_t)cmd.words.w1;

    // Build the mask.
    uint32_t mask = (len >= 32) ? 0xFFFFFFFF : (((1u << len) - 1) << sft);

    m_materialState.otherModeL = (m_materialState.otherModeL & ~mask) | (data & mask);

    // Re-derive alpha test state from render mode.
    m_materialState.alphaTest = DetectAlphaTest(m_materialState.otherModeL);
}

void SceneGeometryExtractor::HandleSetTile(const Gfx& cmd) {
    // G_SETTILE (0xF5):
    //   We only care about tile 0 for the main texture dimensions.
    //   This command sets tile descriptor parameters. The actual tile
    //   dimensions come from G_SETTILESIZE, but the format comes from here.
    //
    //   w0[23:21] = format
    //   w0[20:19] = size (bits per texel)
    //   w0[17:9]  = line (texels per line / 8)
    //   w0[8:0]   = tmem address
    //   w1[25:24] = tile number
    //   w1[23:20] = palette
    //   w1[19:18] = cmt        (bit 18 = mirror_t, bit 19 = clamp_t)
    //   w1[17:14] = mask_t
    //   w1[13:10] = shift_t
    //   w1[9:8]   = cms        (bit 8 = mirror_s, bit 9 = clamp_s)
    //   w1[7:4]   = mask_s
    //   w1[3:0]   = shift_s
    //
    // We extract format info and wrap modes for texture sampling.
    uint8_t tileNum = (uint8_t)((cmd.words.w1 >> 24) & 0x07);

    if (tileNum == 0) {
        m_materialState.texFormat = (uint8_t)((cmd.words.w0 >> 21) & 0x07);
        m_materialState.texSize = (uint8_t)((cmd.words.w0 >> 19) & 0x03);

        // Extract wrap/clamp/mirror mode for S and T axes.
        // N64 tile descriptor bit fields (from GBI Gsettile struct):
        //   S axis: ms = bit 8 (mirror), cs = bit 9 (clamp)
        //   T axis: mt = bit 18 (mirror), ct = bit 19 (clamp)
        //   mask_s = bits [7:4], mask_t = bits [17:14]
        //
        // The 2-bit cm field packs as: bit 0 = mirror, bit 1 = clamp.
        // This matches the GBI struct order: ms(bit8) then cs(bit9) for S,
        //                                   mt(bit18) then ct(bit19) for T.
        uint8_t cms = (uint8_t)((cmd.words.w1 >> 8) & 0x03);   // [ms, cs] for S axis
        uint8_t cmt = (uint8_t)((cmd.words.w1 >> 18) & 0x03);  // [mt, ct] for T axis
        uint8_t maskS = (uint8_t)((cmd.words.w1 >> 4) & 0x0F);
        uint8_t maskT = (uint8_t)((cmd.words.w1 >> 14) & 0x0F);

        // Derive wrap mode from the N64 tile descriptor bits.
        // mask=0 → always CLAMP (N64 hardware: mask controls the wrap width;
        //          mask=0 means the texture never wraps, effectively clamping)
        // mask>0:
        //   mirror=0, clamp=0 → WRAP  (standard power-of-2 wrapping)
        //   mirror=1, clamp=0 → MIRROR (mirror at tile boundary)
        //   mirror=0, clamp=1 → CLAMP  (clamp to tile edge)
        //   mirror=1, clamp=1 → MIRROR then CLAMP (treat as CLAMP for simplicity)
        auto deriveWrapMode = [](uint8_t cm, uint8_t mask) -> uint8_t {
            if (mask == 0) return 2; // CLAMP (mask 0 = no wrap)
            bool mirror = (cm & 0x01) != 0;  // bit 0 = mirror flag (ms or mt)
            bool clamp  = (cm & 0x02) != 0;  // bit 1 = clamp flag  (cs or ct)
            if (mirror && !clamp) return 1;  // MIRROR
            if (clamp)            return 2;  // CLAMP
            return 0;  // WRAP
        };

        m_materialState.wrapModeS = deriveWrapMode(cms, maskS);
        m_materialState.wrapModeT = deriveWrapMode(cmt, maskT);

        // Extract tile shift values for TC scaling.
        // shift_s = w1[3:0], shift_t = w1[13:10]
        // These apply a power-of-2 scale to texture coordinates:
        //   shift < 11: divide TCs by 2^shift (right-shift)
        //   shift >= 11: multiply TCs by 2^(16-shift) (left-shift)
        //   shift == 0: no modification (most common in OoT)
        m_materialState.tileShiftS = (uint8_t)(cmd.words.w1 & 0x0F);
        m_materialState.tileShiftT = (uint8_t)((cmd.words.w1 >> 10) & 0x0F);
    }
}

void SceneGeometryExtractor::HandleSetTileSize(const Gfx& cmd) {
    // G_SETTILESIZE (0xF2):
    //   w0[23:12] = uls (upper-left S, 10.2 fixed-point)
    //   w0[11:0]  = ult (upper-left T, 10.2 fixed-point)
    //   w1[26:24] = tile number
    //   w1[23:12] = lrs (lower-right S, 10.2 fixed-point)
    //   w1[11:0]  = lrt (lower-right T, 10.2 fixed-point)
    //
    // Texture width  = (lrs - uls) / 4 + 1  (converting from 10.2 to integer texels)
    // Texture height = (lrt - ult) / 4 + 1
    uint8_t tileNum = (uint8_t)((cmd.words.w1 >> 24) & 0x07);

    if (tileNum == 0) {
        uint32_t uls = (cmd.words.w0 >> 12) & 0xFFF;
        uint32_t ult = cmd.words.w0 & 0xFFF;
        uint32_t lrs = (cmd.words.w1 >> 12) & 0xFFF;
        uint32_t lrt = cmd.words.w1 & 0xFFF;

        uint16_t width  = (uint16_t)((lrs - uls) / 4 + 1);
        uint16_t height = (uint16_t)((lrt - ult) / 4 + 1);

        // Only update if dimensions are valid (> 0 and reasonable)
        if (width > 0 && width <= 1024 && height > 0 && height <= 1024) {
            m_materialState.texWidth = width;
            m_materialState.texHeight = height;
        }
    }
}

// ============================================================================
// Material management
// ============================================================================

// SEH-safe wrapper for EagerResolveOTRTexture to prevent crashes from bad
// OTR resource pointers or uninitialized TextureManager internal state.
// This MUST be a standalone function because MSVC forbids __try/__except in
// functions that contain C++ objects with destructors.
#ifdef _WIN32
static uint32_t SafeEagerResolveOTRTexture(
    RTX::TextureManager& texMgr,
    const char* pathBuf, uint8_t texFormat, uint8_t texSize,
    uint16_t texWidth, uint16_t texHeight, const char* tlutPath) {
    uint32_t srvIdx = 0;
    __try {
        srvIdx = texMgr.EagerResolveOTRTexture(
            pathBuf, texFormat, texSize, texWidth, texHeight, tlutPath);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        srvIdx = 0;
        static uint32_t s_eagerCrashCount = 0;
        s_eagerCrashCount++;
        if (s_eagerCrashCount <= 5) {
            RTX_DiagLog("[RTX] EagerResolveOTRTexture CRASHED for path='%.120s' (caught #%u)",
                        pathBuf, s_eagerCrashCount);
        }
    }
    return srvIdx;
}
#endif

// SEH-safe helper: try to read a C string from an address that might be invalid.
// Returns true if the address looks like a printable string (at least 2 chars),
// and copies up to maxLen chars into outBuf. outBuf will be null-terminated.
// Must NOT use C++ objects with destructors (SEH requirement).
#ifdef _WIN32
static bool TryCopyStringFromAddr(uintptr_t addr, char* outBuf, size_t maxLen) {
    if (addr < 0x10000 || !outBuf || maxLen == 0) return false;
    outBuf[0] = '\0';
    bool result = false;
    __try {
        const char* s = (const char*)addr;
        // Check first two bytes for OTR path pattern
        if ((s[0] == '_' && s[1] == '_') || (s[0] >= 0x20 && s[0] <= 0x7E)) {
            // Copy the string safely
            size_t i = 0;
            for (; i < maxLen - 1 && s[i] != '\0'; i++) {
                outBuf[i] = s[i];
            }
            outBuf[i] = '\0';
            result = (i > 0);
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        outBuf[0] = '\0';
        result = false;
    }
    return result;
}
#endif

uint32_t SceneGeometryExtractor::GetOrCreateMaterial(ExtractedMesh& mesh) {
    // Safety: if mesh pointers are null (shouldn't happen), return material 0.
    if (!m_alphaMesh || !m_opaqueMesh) {
        return 0;
    }
    bool isAlphaMesh = (&mesh == m_alphaMesh);
    auto& matMap = isAlphaMesh ? m_alphaMaterialMap : m_opaqueMaterialMap;
    auto lumaFromRgb = [](uint8_t r, uint8_t g, uint8_t b) -> uint8_t {
        const uint32_t l = (77u * (uint32_t)r + 150u * (uint32_t)g + 29u * (uint32_t)b) >> 8;
        return (uint8_t)l;
    };
    const uint8_t primLuma = m_materialState.hasPrimColor
        ? lumaFromRgb(m_materialState.primColorR, m_materialState.primColorG, m_materialState.primColorB)
        : 0;
    const uint8_t envLuma = m_materialState.hasEnvColor
        ? lumaFromRgb(m_materialState.envColorR, m_materialState.envColorG, m_materialState.envColorB)
        : 0;

    CombinerMode combMode = ClassifyCombiner(m_materialState.combinerMode);
    bool alphaTest = m_materialState.alphaTest || DetectAlphaTest(m_materialState.otherModeL);
    // Water detection: multi-strategy approach.
    // 1. Texture path/address detection (primary, most reliable)
    // 2. Render mode heuristic (fallback for translucent geometry only)
    //    Only applies when: (a) in the alpha/translucent mesh, (b) render mode is
    //    blended without alpha test, and (c) texture detection didn't already identify it.
    //    This catches water surfaces whose segment 0x0C textures were converted to OTR
    //    hashes that don't contain water keywords.
    bool isWater = DetectWaterTexture(m_materialState.textureAddr);
    // Avoid classifying non-water alpha geometry (grass decals, foliage masks) as water.
    // Restrict render-mode water fallback to intensity/alpha texture formats only.
    const bool waterLikeTexFormat = (m_materialState.texFormat == 3u) || (m_materialState.texFormat == 4u);
    if (!isWater && isAlphaMesh && waterLikeTexFormat) {
        isWater = DetectWaterRenderMode(m_materialState.otherModeL, true);
        if (isWater) {
            static uint32_t s_waterRMDetectCount = 0;
            s_waterRMDetectCount++;
            if (s_waterRMDetectCount <= 20) {
                RTX_DiagLog("[RTX] GetOrCreateMaterial: water detected via RENDER MODE #%u "
                            "(otherModeL=0x%08X, texAddr=0x%llX, translucent=%s)",
                            s_waterRMDetectCount, m_materialState.otherModeL,
                            (unsigned long long)m_materialState.textureAddr,
                            isAlphaMesh ? "yes" : "no");
            }
        }
    }
    if (isWater) {
        static uint32_t s_waterTotalCount = 0;
        s_waterTotalCount++;
        if (s_waterTotalCount <= 20) {
            RTX_DiagLog("[RTX] GetOrCreateMaterial: WATER SURFACE #%u detected "
                        "(texAddr=0x%llX, otherModeL=0x%08X, isAlpha=%s, combiner=0x%llX)",
                        s_waterTotalCount,
                        (unsigned long long)m_materialState.textureAddr,
                        m_materialState.otherModeL,
                        isAlphaMesh ? "yes" : "no",
                        (unsigned long long)m_materialState.combinerMode);
        }
    }

    // Decal detection: ZMODE_DEC indicates overlay geometry that sits on top of
    // other surfaces at the same Z depth (e.g., dirt paths on grass).
    // Decals that are NOT water need special handling in ray tracing (Z bias offset).
    bool isDecal = DetectDecalMode(m_materialState.otherModeL);
    // Kokiri path override: some path overlays are authored as bright IA masks and
    // may not always carry ZMODE_DEC in translated render state. Force decal behavior
    // for known path texture IDs so shader tint/alpha logic can run.
    bool forcePathDecal = false;
#ifdef _WIN32
    if (m_materialState.textureAddr > 0x10000) {
        char pathBuf[512];
        if (TryCopyStringFromAddr(m_materialState.textureAddr, pathBuf, sizeof(pathBuf))) {
            if (strstr(pathBuf, "spot04_room_0Tex_01A290") != nullptr ||
                strstr(pathBuf, "spot04_room_0Tex_01A2") != nullptr ||
                strstr(pathBuf, "spot04_room_0Tex_019A90") != nullptr ||
                strstr(pathBuf, "spot04_room_0Tex_019290") != nullptr ||
                strstr(pathBuf, "spot04_sceneTex_00F218") != nullptr ||
                strstr(pathBuf, "spot04_sceneTex_00FE18") != nullptr) {
                forcePathDecal = true;
            }
        }
    }
#endif
    if (forcePathDecal) {
        isDecal = true;
        alphaTest = true;
    }
    // Non-water decals in the translucent mesh are effectively mask overlays
    // in OoT content. Force alpha-test so AnyHit can clip the rectangular quad.
    if (isDecal && isAlphaMesh && !isWater) {
        alphaTest = true;
    }
    if (isDecal && !isWater) {
        static uint32_t s_decalDetectCount = 0;
        s_decalDetectCount++;
        if (s_decalDetectCount <= 30) {
            RTX_DiagLog("[RTX] GetOrCreateMaterial: DECAL OVERLAY #%u detected "
                        "(otherModeL=0x%08X, texAddr=0x%llX, isAlpha=%s, combiner=0x%llX)",
                        s_decalDetectCount,
                        m_materialState.otherModeL,
                        (unsigned long long)m_materialState.textureAddr,
                        isAlphaMesh ? "yes" : "no",
                        (unsigned long long)m_materialState.combinerMode);
        }
    }

    MaterialKey key;
    key.textureAddr = m_materialState.textureAddr;
    key.combinerMode = static_cast<uint32_t>(combMode);
    key.alphaTest = alphaTest;
    key.isWater = isWater;
    key.isDecal = isDecal;
    key.wrapModeS = m_materialState.wrapModeS;
    key.wrapModeT = m_materialState.wrapModeT;
    key.primLuma = primLuma;
    key.envLuma = envLuma;

    auto it = matMap.find(key);
    if (it != matMap.end()) {
        return it->second;
    }

    // Create a new material.
    Material mat;
    mat.textureIndex = 0; // Default unresolved state (white fallback).
                         // IMPORTANT: ResolveMaterialTextures / deferred re-resolve treat <=2
                         // as fallback/reserved and >2 as real resolved textures.
    mat.combinerMode = static_cast<uint32_t>(combMode);
    mat.isAlphaTested = alphaTest ? 1 : 0;
    mat.isWater = isWater ? 1 : 0;
    mat.isDecal = isDecal ? 1 : 0;
    // Preserve original N64 texture format/size for shader-side material heuristics.
    // Packing:
    //   bits  0.. 7: texSize
    //   bits  8..15: texFormat
    //   bits 16..23: prim color luminance (0 if unset)
    //   bits 24..31: env  color luminance (0 if unset)
    mat._materialPad = (static_cast<uint32_t>(envLuma) << 24) |
                       (static_cast<uint32_t>(primLuma) << 16) |
                       (static_cast<uint32_t>(m_materialState.texFormat) << 8) |
                       static_cast<uint32_t>(m_materialState.texSize);
    mat.wrapModeS = m_materialState.wrapModeS;
    mat.wrapModeT = m_materialState.wrapModeT;
    mat.texWidthPx = m_materialState.texWidth;
    mat.texHeightPx = m_materialState.texHeight;

    // Eagerly attempt to resolve the texture right now by checking the TextureManager
    // cache. This catches textures that were already uploaded via RTX_InterceptTexture
    // before this geometry extraction runs. Textures not yet loaded will remain at
    // textureIndex=0 (white fallback) and be resolved by deferred re-resolve in DispatchAndPresent.
    if (m_materialState.textureAddr > 0x10000) {
        auto& texMgr = RTX::TextureManager::GetInstance();
        if (texMgr.GetSRVHeap()) {
            // Check if the address is an OTR path string using the SEH-safe helper.
            // OTR texture paths may have the "__OTR__" prefix or not.
            // The display list factory stores paths like "scenes/shared/spot04_scene/..."
            // (without __OTR__ prefix) which ResourceMgr handles fine.
            // We accept any printable string longer than 4 chars as a potential path.
            char pathBuf[512];
            bool isOTRStr = false;
#ifdef _WIN32
            isOTRStr = TryCopyStringFromAddr(m_materialState.textureAddr, pathBuf, sizeof(pathBuf));
            // Accept strings that look like OTR paths:
            // - Start with "__OTR__" (explicit OTR prefix)
            // - Start with a letter or digit (likely a resource path like "scenes/..." or "objects/...")
            // - Are at least 4 chars long (to avoid false positives from short random strings)
            if (isOTRStr) {
                size_t pathLen = strlen(pathBuf);
                bool looksLikePath = (pathLen >= 4) && (
                    (pathBuf[0] == '_' && pathBuf[1] == '_') ||     // __OTR__ prefix
                    (pathBuf[0] >= 'a' && pathBuf[0] <= 'z') ||     // lowercase start (scenes/, objects/, etc.)
                    (pathBuf[0] >= 'A' && pathBuf[0] <= 'Z') ||     // uppercase start
                    (pathBuf[0] >= '0' && pathBuf[0] <= '9')        // numeric start
                );
                if (!looksLikePath) {
                    isOTRStr = false;
                }
            }
#endif
            if (isOTRStr && pathBuf[0] != '\0') {
                // Try to resolve the TLUT path too (for CI textures)
                const char* tlutPathForEager = nullptr;
                char tlutBuf[512];
#ifdef _WIN32
                if (m_materialState.lastTlutAddr > 0x10000) {
                    bool tlutIsStr = TryCopyStringFromAddr(m_materialState.lastTlutAddr, tlutBuf, sizeof(tlutBuf));
                    if (tlutIsStr && strlen(tlutBuf) >= 4) {
                        tlutPathForEager = tlutBuf;
                    }
                }
#endif
                // Resolve the texture via TextureManager. Use SEH-protected wrapper on
                // Windows to prevent crashes from bad OTR resources or TextureManager state.
#ifdef _WIN32
                uint32_t srvIdx = SafeEagerResolveOTRTexture(
                    texMgr, pathBuf,
                    m_materialState.texFormat,
                    m_materialState.texSize,
                    m_materialState.texWidth,
                    m_materialState.texHeight,
                    tlutPathForEager);
#else
                uint32_t srvIdx = texMgr.EagerResolveOTRTexture(
                    pathBuf,
                    m_materialState.texFormat,
                    m_materialState.texSize,
                    m_materialState.texWidth,
                    m_materialState.texHeight,
                    tlutPathForEager);
#endif
                if (srvIdx > 0) {
                    mat.textureIndex = srvIdx;
                }
            } else {
                // Not a string pointer — try address-based hash lookup
                constexpr uint64_t FNV_OFFSET = 0xcbf29ce484222325ULL;
                constexpr uint64_t FNV_PRIME  = 0x100000001b3ULL;
                uint64_t addrHash = FNV_OFFSET;
                uintptr_t addr = m_materialState.textureAddr;
                for (size_t b = 0; b < sizeof(addr); b++) {
                    addrHash ^= static_cast<uint64_t>((addr >> (b * 8)) & 0xFF);
                    addrHash *= FNV_PRIME;
                }
                uint32_t srvIdx = texMgr.GetSRVIndexForHash(addrHash);
                if (srvIdx == 0) {
                    srvIdx = texMgr.GetSRVIndexForHash(static_cast<uint64_t>(addr));
                }
                if (srvIdx > 0) {
                    mat.textureIndex = srvIdx;
                }
            }
        }
    }

    uint32_t matID = (uint32_t)mesh.materials.size();
    mesh.materials.push_back(mat);
    // Store the full texture address for later resolution by RTXRenderer.
    mesh.materialTextureAddrs.push_back(m_materialState.textureAddr);

    // Store a durable copy of the OTR path string (if the address is a string pointer).
    // The raw pointer in materialTextureAddrs may become invalid after the
    // SceneGeometryExtractor is reused, but these string copies survive for the
    // lifetime of the ExtractedMesh, enabling deferred texture loading during
    // re-resolve passes.
    {
        std::string pathCopy;
        if (m_materialState.textureAddr > 0x10000) {
#ifdef _WIN32
            char tmpBuf[512];
            if (TryCopyStringFromAddr(m_materialState.textureAddr, tmpBuf, sizeof(tmpBuf))) {
                pathCopy = tmpBuf;
            }
#else
            const char* s = (const char*)m_materialState.textureAddr;
            if (s[0] == '_' && s[1] == '_') {
                pathCopy = s;
            }
#endif
        }
        mesh.materialTexturePaths.push_back(std::move(pathCopy));
    }

    // Store a durable copy of the TLUT path string (if the lastTlutAddr is a string pointer).
    // This is used by TryLoadOTRTexture to decode CI4/CI8 textures.
    {
        std::string tlutPathCopy;
        if (m_materialState.lastTlutAddr > 0x10000) {
#ifdef _WIN32
            char tmpBuf[512];
            if (TryCopyStringFromAddr(m_materialState.lastTlutAddr, tmpBuf, sizeof(tmpBuf))) {
                tlutPathCopy = tmpBuf;
            }
#else
            const char* s = (const char*)m_materialState.lastTlutAddr;
            if (s[0] == '_' && s[1] == '_') {
                tlutPathCopy = s;
            }
#endif
        }
        mesh.materialTlutPaths.push_back(std::move(tlutPathCopy));
    }
    // Store per-material texture format info for use during OTR texture resolution.
    {
        ExtractedMesh::MaterialTexInfo texInfo;
        texInfo.texFormat = m_materialState.texFormat;
        texInfo.texSize = m_materialState.texSize;
        texInfo.texWidth = m_materialState.texWidth;
        texInfo.texHeight = m_materialState.texHeight;
        mesh.materialTexInfos.push_back(texInfo);
    }

    matMap[key] = matID;

    // Log material creation with texture address details
    {
        bool isStr = !mesh.materialTexturePaths.back().empty();
        static uint32_t s_matCreateLog = 0;
        static uint32_t s_matEagerResolved = 0;
        s_matCreateLog++;
        if (mat.textureIndex > 2) s_matEagerResolved++;
        if (s_matCreateLog <= 40 || (s_matCreateLog % 100) == 0) {
            if (isStr) {
                RTX_DIAG("CreateMaterial #%u: matID=%u texIdx=%u%s path='%s' comb=%u alpha=%u water=%u %s texW=%u texH=%u (eager=%u/%u)",
                         s_matCreateLog, matID, mat.textureIndex,
                         mat.textureIndex > 2 ? " [RESOLVED]" : " [UNRESOLVED]",
                         mesh.materialTexturePaths.back().c_str(),
                         mat.combinerMode, mat.isAlphaTested, mat.isWater,
                         isAlphaMesh ? "ALPHA" : "OPAQUE",
                         m_materialState.texWidth, m_materialState.texHeight,
                         s_matEagerResolved, s_matCreateLog);
            } else {
                RTX_DIAG("CreateMaterial #%u: matID=%u texIdx=%u%s texAddr=0x%llX (not string) comb=%u alpha=%u water=%u %s texW=%u texH=%u (eager=%u/%u)",
                         s_matCreateLog, matID, mat.textureIndex,
                         mat.textureIndex > 2 ? " [RESOLVED]" : " [UNRESOLVED]",
                         (unsigned long long)m_materialState.textureAddr,
                         mat.combinerMode, mat.isAlphaTested, mat.isWater,
                         isAlphaMesh ? "ALPHA" : "OPAQUE",
                         m_materialState.texWidth, m_materialState.texHeight,
                         s_matEagerResolved, s_matCreateLog);
            }
        }
    }

    SPDLOG_DEBUG("[RTX] SceneGeometryExtractor: created material {} (texAddr=0x{:X} comb={} alpha={} water={})",
                 matID, m_materialState.textureAddr, mat.combinerMode, mat.isAlphaTested, mat.isWater);

    return matID;
}

CombinerMode SceneGeometryExtractor::ClassifyCombiner(uint64_t rawCombiner) {
    // The N64 combiner has two cycles, each with four color slots (A, B, C, D)
    // and four alpha slots. The 64-bit value encodes all of them.
    //
    // Common combiner setups in OoT:
    //
    // TEXEL0 * SHADE:
    //   G_CC_MODULATERGB   = (TEXEL0, 0, SHADE, 0)  -> Modulate RGB
    //   G_CC_MODULATERGBA  = same + alpha modulation  -> Modulate RGBA
    //
    // TEXEL0 only:
    //   G_CC_DECALRGB      = (0, 0, 0, TEXEL0)       -> Decal
    //   G_CC_DECALRGBA     = same + TEXEL0 alpha
    //
    // SHADE only:
    //   G_CC_SHADE         = (0, 0, 0, SHADE)         -> Shade only
    //
    // Environment blend:
    //   G_CC_BLENDRGBA     = (TEXEL0, ENVIRONMENT, TEXEL0_ALPHA, ...) -> Env blend
    //
    // Decoding the fields from the 64-bit combiner mode:
    // Upper 32 bits (from w0 & 0x00FFFFFF shifted):
    //   [23:20] a0  [19:15] b0  [14:12] c0  [11:8] d0  [7:4] Aa0  [3:0] Ac0
    // Lower 32 bits (w1):
    //   [31:28] a1  [27:24] b1  [23:21] c1  [20:18] d1  [17:15] Ab0  [14:12] Ad0
    //   [11:9] Aa1  [8:6] Ab1   [5:3] Ac1   [2:0] Ad1
    //
    // Key values for cycle 1 (the common one):
    //   TEXEL0 = 1 in a0/b0/d0, 1 in c0
    //   TEXEL1 = 2 in a0/b0/d0, 2 in c0
    //   SHADE  = 4 in a0/b0/d0, 4 in c0
    //   ENVIRONMENT = 5 in a0/b0/d0, 5 in c0
    //   0 = 0 in c0 (pass through)

    // Extract cycle 0 color combiner fields from the packed 64-bit mode.
    //
    // The N64 combiner formula is: color = (A - B) * C + D
    //   where A, B, C, D are selected by the combiner mode bits.
    //
    // The 64-bit combiner mode is packed as:
    //   rawCombiner = ((w0 & 0x00FFFFFF) << 32) | w1
    //
    // w0 & 0x00FFFFFF (upper 24 bits of rawCombiner) layout:
    //   [23:20] = a0  (4 bits) — color A input, cycle 0 (G_CCMUX_* values)
    //   [19:15] = c0  (5 bits) — color C input (multiplier), cycle 0
    //   [14:12] = Aa0 (3 bits) — alpha A input, cycle 0 (G_ACMUX_* values)
    //   [11:9]  = Ac0 (3 bits) — alpha C input, cycle 0
    //   [8:5]   = a1  (4 bits) — color A input, cycle 1
    //   [4:0]   = c1  (5 bits) — color C input, cycle 1
    //
    // w1 (lower 32 bits of rawCombiner) layout:
    //   [31:28] = b0  (4 bits) — color B input, cycle 0
    //   [27:24] = b1  (4 bits) — color B input, cycle 1
    //   [23:21] = Aa1 (3 bits) — alpha A input, cycle 1
    //   [20:18] = Ac1 (3 bits) — alpha C input, cycle 1
    //   [17:15] = d0  (3 bits) — color D input, cycle 0
    //   [14:12] = Ab0 (3 bits) — alpha B input, cycle 0
    //   [11:9]  = Ad0 (3 bits) — alpha D input, cycle 0
    //   [8:6]   = d1  (3 bits) — color D input, cycle 1
    //   [5:3]   = Ab1 (3 bits) — alpha B input, cycle 1
    //   [2:0]   = Ad1 (3 bits) — alpha D input, cycle 1
    //
    // IMPORTANT: The bit positions are derived from the GBI macros:
    //   GCCc0w0(saRGB0=a0, mRGB0=c0, saA0=Aa0, mA0=Ac0) packs into w0[23:0]
    //   GCCc0w1(sbRGB0=b0, aRGB0=d0, sbA0=Ab0, aA0=Ad0) packs into w1[31:0]
    //
    // This was previously extracting the wrong bit fields, causing
    // combiner misclassification and white rendering of textured objects.
    uint32_t upper = (uint32_t)(rawCombiner >> 32);
    uint32_t lower = (uint32_t)(rawCombiner);

    // Color combiner cycle 0 fields
    uint8_t a0 = (upper >> 20) & 0x0F;   // color A: 4 bits at [23:20]
    uint8_t c0 = (upper >> 15) & 0x1F;   // color C: 5 bits at [19:15]
    uint8_t b0 = (lower >> 28) & 0x0F;   // color B: 4 bits at w1[31:28]
    uint8_t d0 = (lower >> 15) & 0x07;   // color D: 3 bits at w1[17:15]

    // Alpha combiner cycle 0 fields
    uint8_t Aa0 = (upper >> 12) & 0x07;  // alpha A: 3 bits at [14:12]
    uint8_t Ac0 = (upper >>  9) & 0x07;  // alpha C: 3 bits at [11:9]

    // Simple heuristic classification:

    // Check for environment blend: uses ENVIRONMENT in any combiner input.
    //   A input: 5 = ENVIRONMENT
    //   B input: 5 = ENVIRONMENT
    //   C input: 5 = ENVIRONMENT
    //   D input: 5 = ENVIRONMENT
    // G_CC_BLENDRGBA is (TEXEL0, ENVIRONMENT, TEXEL0_ALPHA, TEXEL0) where b0=5.
    if (a0 == 5 || b0 == 5 || c0 == 5 || d0 == 5) {
        return COMBINER_TEX_ENV_BLEND;
    }

    // Check for texture and shade presence in the combiner inputs.
    //
    // N64 Color Combiner input values (from gbi.h):
    //   A/B inputs (4 bits): 0=COMBINED, 1=TEXEL0, 2=TEXEL1, 3=PRIM, 4=SHADE, 5=ENV
    //   C input (5 bits, but typically 0-5 used):
    //     0=COMBINED, 1=TEXEL0, 2=TEXEL1, 3=PRIMITIVE, 4=SHADE, 5=ENVIRONMENT
    //   D input (3 bits): 0=COMBINED, 1=TEXEL0, 2=TEXEL1, 3=PRIM, 4=SHADE, 5=ENV, 6=1, 7=0
    //
    // hasTexel: any input references TEXEL0 (value 1) or TEXEL1 (value 2)
    // hasShade: any input references SHADE (value 4 in A/D, value 4 in C)
    //
    // IMPORTANT FIX: c0 == 2 is TEXEL1, NOT SHADE. SHADE in c0 is value 4.
    // Previously c0 == 2 was incorrectly counted as hasShade, causing combiners
    // with TEXEL1 in the C slot to be classified as SHADE-only (vertex color only),
    // which made textured geometry like the Deku Tree appear solid white.
    bool hasTexel = (a0 == 1 || a0 == 2) || (b0 == 1 || b0 == 2) || (c0 == 1 || c0 == 2) || (d0 == 1 || d0 == 2);
    bool hasShade = (a0 == 4) || (b0 == 4) || (c0 == 4) || (d0 == 4);

    if (!hasTexel && hasShade) {
        return COMBINER_SHADE;
    }

    if (hasTexel && !hasShade) {
        // Decal: texture only, no shade modulation.
        return COMBINER_DECAL;
    }

    if (hasTexel && hasShade) {
        // Modulate: texture * shade. Check alpha for RGBA variant.
        // If alpha combiner also uses both texel and shade alpha, it's MODULATE_RGBA.
        //
        // Alpha combiner A input values (4 bits):
        //   0=COMBINED, 1=TEXEL0, 2=TEXEL1, 3=PRIM, 4=SHADE, 5=ENV, 6=1, 7=0
        // Alpha combiner C input values (3 bits):
        //   0=LOD_FRAC, 1=TEXEL0, 2=TEXEL1, 3=PRIM, 4=SHADE, 5=ENV, 6=PRIM_LOD_FRAC, 7=0
        bool alphaHasTexel = (Aa0 == 1 || Aa0 == 2) || (Ac0 == 1 || Ac0 == 2);
        bool alphaHasShade = (Aa0 == 4) || (Ac0 == 4);

        if (alphaHasTexel || alphaHasShade) {
            return COMBINER_MODULATE_RGBA;
        }
        return COMBINER_MODULATE_RGB;
    }

    // Default: plain modulate.
    return COMBINER_MODULATE_RGB;
}

bool SceneGeometryExtractor::DetectAlphaTest(uint32_t otherModeL) {
    // Alpha testing on N64 is indicated by CVG_X_ALPHA being set in the render mode.
    // CVG_X_ALPHA means "use alpha for coverage", which effectively performs alpha test.
    // FORCE_BL + CVG_X_ALPHA together with translucent Z mode also indicate blending,
    // but CVG_X_ALPHA without FORCE_BL is the clearest alpha-test indicator.
    //
    // Additionally, the AA_TEX_EDGE render modes use CVG_X_ALPHA | ALPHA_CVG_SEL
    // which is the canonical alpha test mode.

    bool hasCvgXAlpha = (otherModeL & CVG_X_ALPHA) != 0;

    // Also check for ZMODE_XLU which indicates translucent rendering (not just alpha test).
    // Pure alpha test typically uses ZMODE_OPA with CVG_X_ALPHA.
    bool isZModeXlu = (otherModeL & 0x0C00) == ZMODE_XLU;

    // If CVG_X_ALPHA is set and we're not in full XLU mode, it's alpha test.
    if (hasCvgXAlpha && !isZModeXlu) {
        return true;
    }

    // If CVG_X_ALPHA is set with ALPHA_CVG_SEL, it's definitely alpha test
    // (this is the AA_TEX_EDGE pattern).
    if (hasCvgXAlpha && (otherModeL & ALPHA_CVG_SEL) != 0) {
        return true;
    }

    return false;
}

bool SceneGeometryExtractor::DetectWaterRenderMode(uint32_t otherModeL, bool isTranslucent) {
    // OoT water surfaces use specific render modes that are distinctly different from
    // other translucent surfaces (fog, particles, alpha-tested foliage).
    //
    // Water render modes in OoT:
    //   G_RM_XLU_SURF  = FORCE_BL | ZMODE_XLU | blending formula
    //   G_RM_CLD_SURF  = FORCE_BL | ZMODE_INTER | CVG_DST_SAVE | blending formula
    //
    // Key pattern: FORCE_BL + ZMODE_XLU + no CVG_X_ALPHA
    // This means: blended surface (semi-transparent), not alpha-tested.
    //
    // We ONLY apply this heuristic when:
    //   1. Triangle comes from a translucent (xlu) display list
    //   2. Render mode has FORCE_BL (blending is forced)
    //   3. Z mode is XLU (translucent depth) - NOT INTER or OPA
    //   4. Does NOT have CVG_X_ALPHA (not alpha-tested)
    //   5. Does NOT have ALPHA_CVG_SEL (not using coverage for alpha)
    //
    // We specifically require ZMODE_XLU (not ZMODE_INTER) to be more conservative.
    // ZMODE_INTER is used by many non-water surfaces like cloud shadows and decals.
    // ZMODE_XLU is the canonical water Z mode in OoT.
    //
    // This catches water surfaces even when their texture address isn't recognized
    // (e.g., segment 0x0C textures converted to OTR hashes without water keywords).

    if (!isTranslucent) return false;

    bool hasForceBL = (otherModeL & FORCE_BL) != 0;
    bool hasCvgXAlpha = (otherModeL & CVG_X_ALPHA) != 0;
    bool hasAlphaCvgSel = (otherModeL & ALPHA_CVG_SEL) != 0;
    bool isZModeXlu = (otherModeL & 0x0C00) == ZMODE_XLU;

    // Conservative pattern: only match blended XLU surfaces without alpha test flags.
    // This is the canonical OoT water render mode.
    if (hasForceBL && isZModeXlu && !hasCvgXAlpha && !hasAlphaCvgSel) {
        static uint32_t s_waterRMCount = 0;
        s_waterRMCount++;
        if (s_waterRMCount <= 10) {
            RTX_DiagLog("DetectWaterRenderMode: otherModeL=0x%08X -> WATER (FORCE_BL|ZMODE_XLU, no alpha)",
                        otherModeL);
        }
        return true;
    }

    // Additional pattern: specific known water render modes from OoT.
    // 0x00552078 = Z_CMP | Z_UPD | IM_RD | CVG_DST_SAVE | ZMODE_XLU | FORCE_BL | GBL_c*
    // This is the G_RM_XLU_SURF2 variant used by many OoT water surfaces.
    // 0x005049D8 = Another common water render mode (G_RM_CLD_SURF2 variant).
    // We mask out the blender cycle-specific bits (lower 14 bits) and check the mode flags.
    {
        uint32_t modeFlags = otherModeL & 0xFFFF0000;
        // G_RM_XLU_SURF: flags typically have Z_CMP | Z_UPD | FORCE_BL | ZMODE_XLU
        // which equals 0x00550000 (approximate, varies by cycle bits)
        if (modeFlags == 0x00550000 || modeFlags == 0x00500000) {
            static uint32_t s_waterRM2Count = 0;
            s_waterRM2Count++;
            if (s_waterRM2Count <= 10) {
                RTX_DiagLog("DetectWaterRenderMode: otherModeL=0x%08X -> WATER (known XLU_SURF mode flags 0x%08X)",
                            otherModeL, modeFlags);
            }
            return true;
        }
    }

    // Additional pattern: G_RM_ZB_XLU_SURF / G_RM_ZB_CLD_SURF
    // Some OoT water surfaces use Z-buffered translucent modes that include Z_CMP | Z_UPD.
    // G_RM_ZB_XLU_SURF = Z_CMP | Z_UPD | IM_RD | CVG_DST_FULL | ZMODE_XLU | FORCE_BL
    // The key signature: FORCE_BL, Z_CMP, ZMODE_XLU set; CVG_X_ALPHA and ALPHA_CVG_SEL clear.
    // This is slightly less restrictive than the first check (allows IM_RD and various CVG_DST).
    //
    // N64 render mode flag bit positions (in the lower 15 bits of otherModeL):
    //   Z_CMP = 0x10, Z_UPD = 0x20, IM_RD = 0x40
    //   CVG_DST = bits [9:8] (0x00=CLAMP, 0x100=WRAP, 0x200=FULL, 0x300=SAVE)
    //   ZMODE = bits [11:10] (0x000=OPA, 0x400=INTER, 0x800=XLU, 0xC00=DEC)
    //   CVG_X_ALPHA = 0x1000, ALPHA_CVG_SEL = 0x2000, FORCE_BL = 0x4000
    {
        bool hasZCmp = (otherModeL & Z_CMP) != 0;
        bool hasZUpd = (otherModeL & Z_UPD) != 0;
        if (hasForceBL && hasZCmp && hasZUpd && isZModeXlu && !hasCvgXAlpha && !hasAlphaCvgSel) {
            static uint32_t s_waterRM3Count = 0;
            s_waterRM3Count++;
            if (s_waterRM3Count <= 10) {
                RTX_DiagLog("DetectWaterRenderMode: otherModeL=0x%08X -> WATER (ZB_XLU_SURF pattern: Z_CMP|Z_UPD|FORCE_BL|ZMODE_XLU)",
                            otherModeL);
            }
            return true;
        }
    }

    // NOTE: ZMODE_DEC (decal Z mode) surfaces are NOT classified as water here.
    // Previously, G_RM_ZB_OVL_SURF (FORCE_BL | ZMODE_DEC | CVG_DST_SAVE) was
    // incorrectly detected as water. However, this render mode is also used for:
    //   - Dirt path overlays on grass (Kokiri Forest)
    //   - Shadow decals on floors
    //   - Ground texture overlays
    // These non-water decals were being misclassified as water, causing them to
    // receive water material properties (UV scrolling, high reflectivity) and
    // potentially be culled or rendered incorrectly.
    //
    // Water surfaces that use ZMODE_DEC will still be detected by:
    //   1. DetectWaterTexture() (texture path keyword matching)
    //   2. The segment 0x0C detection in DetectWaterTexture()
    //   3. Material override system in RTXSceneConfig
    // So removing this heuristic does NOT prevent actual water from being detected.

    return false;
}

bool SceneGeometryExtractor::DetectWaterTexture(uintptr_t textureAddr) {
    if (textureAddr == 0) {
        return false;
    }

    // Heuristic: check if the texture path string contains water-related keywords.
    // On the PC port (SoH/OTR build), the texture address is typically a pointer to
    // an OTR resource path string (e.g., "scenes/shared/spot04_scene/spot04_room_0Tex_00B0A8").
    //
    // We combine multiple detection strategies:
    //   0. Segment 0x0C detection (animated water scroll texture)
    //   1. OTR path keyword matching (water, river, stream, pond, etc.)
    //   2. Known Kokiri Forest water texture hex offsets
    //   3. Material override system lookup (RTXSceneConfig registered overrides)
    //   4. Resolved segment address path check (resolve gSegments then check string)

    // =========================================================================
    // Strategy 0: Detect N64 segment 0x0C addresses (water scroll texture).
    //
    // In OoT, water surfaces use segment 0x0C for their animated scrolling texture.
    // The game code sets gSegments[0x0C] to point to the current water texture frame
    // each gameplay tick, and the display list references it as 0x0Cxxxxxx.
    //
    // In the SoH/OTR build, segmented addresses have bit 0 set as a flag (matching
    // the Fast3D interpreter's SegAddr convention). So segment 0x0C addresses look
    // like 0x0C000001, 0x0C001001, etc.
    //
    // These addresses are NOT valid host pointers and cannot be dereferenced as
    // OTR path strings. We detect them by checking the segment number in the high
    // byte. If it's segment 0x0C, this is almost certainly a water scroll texture.
    //
    // Additionally check for segment 0x09 which some scenes use for water effects,
    // and segment 0x08 for scene-specific water textures.
    // =========================================================================
    {
        // Check if this looks like a segmented address (bit 0 set, or value in segment range).
        // In the OTR build, segmented addresses have bit 0 set.
        bool isSegmented = (textureAddr & 1) != 0;
        if (isSegmented) {
            uint32_t segNum = (uint32_t)(textureAddr >> 24);
            // Segment 0x0C is the canonical water scroll segment in OoT.
            // It's set by the game code (e.g., in Kokiri Forest scene draw function)
            // to point to the animated water texture frame.
            if (segNum == 0x0C) {
                RTX_DiagLog("DetectWaterTexture: segment 0x0C address 0x%llX -> WATER (scroll segment)",
                            (unsigned long long)textureAddr);
                return true;
            }
        }

        // Also check for non-segmented addresses that are in the N64 address range.
        // Some display lists use plain segment addresses without the OTR bit-0 flag.
        // N64 segment addresses are 32-bit values with segment in bits 24-27.
        if (textureAddr < 0x10000000ULL && textureAddr >= 0x01000000ULL) {
            uint32_t segNum = (uint32_t)((textureAddr >> 24) & 0x0F);
            if (segNum == 0x0C) {
                RTX_DiagLog("DetectWaterTexture: raw segment 0x0C address 0x%llX -> WATER",
                            (unsigned long long)textureAddr);
                return true;
            }
        }
    }

    // Guard against dereferencing small integer values that aren't real pointers.
    // On 64-bit Windows, user-mode addresses start at 0x10000 and above.
    // Anything below that threshold is almost certainly a raw N64 segment address,
    // not a valid host pointer.
    if (textureAddr < 0x10000) {
        return false;
    }

    // Use SEH-safe path reading on Windows to avoid crashes from bad pointers.
    // The textureAddr may point to freed or unmapped memory.
    char pathBuf[512];
    pathBuf[0] = '\0';
#ifdef _WIN32
    if (!TryCopyStringFromAddr(textureAddr, pathBuf, sizeof(pathBuf))) {
        return false;
    }
#else
    {
        const char* s = (const char*)textureAddr;
        if (s[0] < 0x20 || s[0] > 0x7E) return false;
        size_t i = 0;
        for (; i < sizeof(pathBuf) - 1 && s[i] != '\0'; i++) {
            pathBuf[i] = s[i];
        }
        pathBuf[i] = '\0';
    }
#endif
    if (pathBuf[0] == '\0') {
        return false;
    }

    const char* path = pathBuf;

    // Case-insensitive substring search helper.
    auto containsCI = [](const char* haystack, const char* needle) -> bool {
        if (!haystack || !needle) return false;
        size_t hLen = strlen(haystack);
        size_t nLen = strlen(needle);
        if (nLen > hLen) return false;
        for (size_t i = 0; i <= hLen - nLen; i++) {
            bool match = true;
            for (size_t j = 0; j < nLen; j++) {
                char h = haystack[i + j];
                char n = needle[j];
                // Simple ASCII tolower
                if (h >= 'A' && h <= 'Z') h += 32;
                if (n >= 'A' && n <= 'Z') n += 32;
                if (h != n) {
                    match = false;
                    break;
                }
            }
            if (match) return true;
        }
        return false;
    };

    // Kokiri exclusions: these are grass/ground overlays that were being
    // misclassified as water by translucent render-mode fallback.
    if (containsCI(path, "spot04_room_0Tex_018A90") ||
        containsCI(path, "spot04_room_0Tex_01B090")) {
        return false;
    }

    // Strategy 1: Generic water keywords in OTR texture path.
    // These match water textures across all OoT scenes.
    if (containsCI(path, "water") || containsCI(path, "river") || containsCI(path, "mizu") ||
        containsCI(path, "stream") || containsCI(path, "pond") || containsCI(path, "lake") ||
        containsCI(path, "waterfall") || containsCI(path, "fountain") ||
        containsCI(path, "suimen") /* Japanese for water surface */) {
        return true;
    }

    // Strategy 2: Kokiri Forest-specific water texture patterns.
    // These match known spot04 (Kokiri Forest) texture hex offsets for water geometry.
    // spot04_room_0Tex_00B0A8: main stream/river water texture
    // spot04_room_0Tex_00B8A8: secondary water texture (pond/edge)
    // spot04_room_0Tex_00A8A8: additional water variant
    // spot04_room_0Tex_00C0A8, 00C8A8: additional variants observed in some OTR builds
    if (containsCI(path, "spot04_room_0Tex_00B0A8") ||
        containsCI(path, "spot04_room_0Tex_00B8A8") ||
        containsCI(path, "spot04_room_0Tex_00A8A8") ||
        containsCI(path, "spot04_room_0Tex_00C0A8") ||
        containsCI(path, "spot04_room_0Tex_00C8A8")) {
        return true;
    }

    // Strategy 2b: Additional diagnostic for unrecognized textures in known water scenes.
    // Log texture paths from Kokiri Forest that we DON'T recognize, to help identify
    // water textures that need to be added to the detection patterns.
    if (containsCI(path, "spot04")) {
        static uint32_t s_spot04UnknownCount = 0;
        s_spot04UnknownCount++;
        if (s_spot04UnknownCount <= 30) {
            RTX_DiagLog("DetectWaterTexture: Kokiri Forest texture NOT matched as water: '%s'", path);
        }
    }

    // Strategy 3: Check against the material override system.
    // If RTXSceneConfig has registered a material override with isWater=true for
    // a pattern that matches this texture path, use that classification.
    // This allows data-driven water detection without code changes.
    {
        std::string pathStr(path);
        // Extract just the texture name portion (after the last '/' if present)
        size_t lastSlash = pathStr.find_last_of('/');
        std::string texName = (lastSlash != std::string::npos) ? pathStr.substr(lastSlash + 1) : pathStr;

        // Check all registered material overrides for water flag
        const auto& overrides = RTX::GetMaterialOverrides();
        for (const auto& entry : overrides) {
            if (entry.second.isWater) {
                // Check if the override pattern appears anywhere in the texture path
                if (containsCI(path, entry.first.c_str())) {
                    return true;
                }
            }
        }
    }

    // Strategy 4: Try resolving segment addresses via gSegments[] and check the
    // resolved pointer as an OTR path. Some water textures are loaded through
    // segment indirection where gSegments[N] points to an OTR path string.
    // This handles cases where the texture was set via plain G_SETTIMG with a
    // segment address, and the segment was already resolved by the engine.
    {
        bool isSegmented = (textureAddr & 1) != 0;
        if (isSegmented) {
            uint32_t segNum = (uint32_t)(textureAddr >> 24);
            uint32_t offset = (uint32_t)(textureAddr & 0x00FFFFFE);
            if (segNum < 16 && gSegments[segNum] != 0) {
                uintptr_t resolved = gSegments[segNum] + offset;
                if (resolved > 0x10000) {
                    char resolvedPathBuf[512];
                    resolvedPathBuf[0] = '\0';
                    bool gotString = false;
#ifdef _WIN32
                    gotString = TryCopyStringFromAddr(resolved, resolvedPathBuf, sizeof(resolvedPathBuf));
#else
                    // Non-Windows: best-effort string copy (no SEH protection)
                    {
                        const char* s = (const char*)resolved;
                        if (s[0] >= 0x20 && s[0] <= 0x7E) {
                            size_t i = 0;
                            for (; i < sizeof(resolvedPathBuf) - 1 && s[i] != '\0'; i++) {
                                resolvedPathBuf[i] = s[i];
                            }
                            resolvedPathBuf[i] = '\0';
                            gotString = (resolvedPathBuf[0] != '\0');
                        }
                    }
#endif
                    if (gotString && resolvedPathBuf[0] != '\0') {
                        if (containsCI(resolvedPathBuf, "water") ||
                            containsCI(resolvedPathBuf, "river") ||
                            containsCI(resolvedPathBuf, "stream") ||
                            containsCI(resolvedPathBuf, "pond") ||
                            containsCI(resolvedPathBuf, "mizu")) {
                            RTX_DiagLog("DetectWaterTexture: resolved seg 0x%X addr 0x%llX -> '%s' -> WATER",
                                        segNum, (unsigned long long)textureAddr, resolvedPathBuf);
                            return true;
                        }
                    }
                }
            }
        }
    }

    return false;
}

bool SceneGeometryExtractor::DetectDecalMode(uint32_t otherModeL) {
    // Detect ZMODE_DEC (decal Z mode): bits [11:10] = 0b11 = 0xC00.
    // Decal mode renders geometry at the same Z depth as the underlying surface.
    // In OoT, this is used for:
    //   - Dirt paths/road overlays on grass (Kokiri Forest, Hyrule Field)
    //   - Shadow decals on floors
    //   - Ground markings and texture overlays
    //   - Some semi-transparent surface overlays
    //
    // Render modes using ZMODE_DEC include:
    //   G_RM_ZB_OVL_SURF  (overlay surface with Z buffering)
    //   G_RM_ZB_CLD_SURF  (cloud/blend surface with decal Z)
    //
    // Note: ZMODE_DEC is NOT the same as ZMODE_XLU (translucent Z mode).
    // ZMODE_DEC adjusts the Z comparison to allow co-planar rendering,
    // while ZMODE_XLU adjusts Z for translucent back-to-front ordering.
    bool isZModeDec = (otherModeL & 0x0C00) == ZMODE_DEC;
    return isZModeDec;
}

} // namespace RTX

#endif // ENABLE_DX12_RTX
