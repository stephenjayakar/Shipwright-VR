#ifdef ENABLE_DX12_RTX

#include "SceneGeometryExtractor.h"
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
    auto resolveOTRDL = [](Gfx* rawPtr) -> Gfx* {
        if (!rawPtr) return nullptr;
        // Check if this looks like a string path (starts with '_' from "__OTR__")
        // rather than actual Gfx data (which would start with an opcode byte).
        const char* asStr = (const char*)rawPtr;
        if (asStr[0] == '_' && asStr[1] == '_') {
            // It's an OTR path string — resolve to actual Gfx data.
            Gfx* result = ResourceMgr_LoadGfxByName(asStr);
            if (result) {
                // Log first command of the resolved DL for debugging
                uint8_t firstOp = (uint8_t)(result->words.w0 >> 24);
                RTX_DiagLog("resolveOTRDL: '%s' -> %p (firstOp=0x%02X, w0=0x%llX, w1=0x%llX)",
                            asStr, (void*)result, firstOp,
                            (unsigned long long)result->words.w0,
                            (unsigned long long)result->words.w1);
            } else {
                RTX_DiagLog("resolveOTRDL: '%s' -> FAILED (nullptr)", asStr);
            }
            return result;
        }
        // Already a real Gfx pointer (shouldn't happen in OTR build, but handle it)
        RTX_DiagLog("resolveOTRDL: rawPtr=%p is not an OTR path (first bytes: 0x%02X 0x%02X)",
                    (void*)rawPtr, (uint8_t)asStr[0], (uint8_t)asStr[1]);
        return rawPtr;
    };

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

        RTX_DIAG("SceneGeometryExtractor: room %u type 0, %u DL entries, dlists=%p", roomIndex, numEntries, (void*)dlists);
        SPDLOG_INFO("[RTX] SceneGeometryExtractor: room {} type 0 with {} DL entries", roomIndex, numEntries);

        for (uint8_t i = 0; i < numEntries; i++) {
            // Opaque display list
            if (dlists[i].opa) {
                Gfx* opaDL = resolveOTRDL(dlists[i].opa);
                if (opaDL) {
                    SPDLOG_DEBUG("[RTX] SceneGeometryExtractor: room {} entry {} walking opa DL at {:p} (from '{}')",
                                 roomIndex, i, (void*)opaDL, (const char*)dlists[i].opa);
                    m_materialState.Reset();
                    WalkDisplayList(opaDL, /*isTranslucent=*/false, /*depth=*/0);
                } else {
                    SPDLOG_WARN("[RTX] SceneGeometryExtractor: room {} entry {} failed to resolve opa DL path: {}",
                                roomIndex, i, (const char*)dlists[i].opa);
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
                    SPDLOG_WARN("[RTX] SceneGeometryExtractor: room {} entry {} failed to resolve xlu DL path: {}",
                                roomIndex, i, (const char*)dlists[i].xlu);
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

        RTX_DIAG("SceneGeometryExtractor: room %u type 2, %u DL entries, dlists=%p", roomIndex, numEntries, (void*)dlists);
        SPDLOG_INFO("[RTX] SceneGeometryExtractor: room {} type 2 with {} DL entries", roomIndex, numEntries);

        for (uint8_t i = 0; i < numEntries; i++) {
            // Opaque display list
            if (dlists[i].opa) {
                Gfx* opaDL = resolveOTRDL(dlists[i].opa);
                if (opaDL) {
                    SPDLOG_DEBUG("[RTX] SceneGeometryExtractor: room {} entry {} walking opa DL (type2) at {:p} (from '{}')",
                                 roomIndex, i, (void*)opaDL, (const char*)dlists[i].opa);
                    m_materialState.Reset();
                    WalkDisplayList(opaDL, /*isTranslucent=*/false, /*depth=*/0);
                } else {
                    SPDLOG_WARN("[RTX] SceneGeometryExtractor: room {} entry {} failed to resolve opa DL path: {}",
                                roomIndex, i, (const char*)dlists[i].opa);
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
                    SPDLOG_WARN("[RTX] SceneGeometryExtractor: room {} entry {} failed to resolve xlu DL path: {}",
                                roomIndex, i, (const char*)dlists[i].xlu);
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
            if (path) {
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

        // ---- Commands we recognize but don't need to act on ----
        case G_RDPPIPESYNC:
        case G_LOADBLOCK:
        case G_LOADTLUT:
        case G_SETENVCOLOR:
        case G_NOOP:
        case G_RDPFULLSYNC:
        case G_RDPTILESYNC:
        case G_RDPLOADSYNC:
        case G_SETPRIMCOLOR:
        case G_SETFOGCOLOR:
        case G_SETBLENDCOLOR:
        case G_SETFILLCOLOR:
        case G_SETSCISSOR:
        case G_SETZIMG:
        case G_SETCIMG:
        case G_LOADTILE:
        case G_TEXTURE:
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
    uint32_t v0idx = ((cmd.words.w0 >> 1) & 0x7F) - n;

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
    // 2-word command:
    //   cmds[0].w0 = opcode << 24
    //   cmds[0].w1 = (uintptr_t) file path string
    //   cmds[1].w0 = vertex count (n)
    //   cmds[1].w1 = (bufferIndex << 16) | dataOffset
    //     bufferIndex is the start slot in the vertex buffer
    const char* path = (const char*)cmds[0].words.w1;
    uint32_t n = (uint32_t)cmds[1].words.w0;
    uint32_t bufferIndex = (uint32_t)((cmds[1].words.w1 >> 16) & 0xFFFF);
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
    uint32_t dataOffset = (uint32_t)(cmds[1].words.w1 & 0xFFFF);
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
    // 2-word command (matching Fast3D gfx_vtx_hash_handler_custom):
    //   cmds[0].w0 = opcode << 24 | (numVerts << 12) | ((v0+n) << 1)
    //   cmds[0].w1 = byte offset into the vertex resource array
    //   cmds[1].w0 = high 32 bits of CRC hash
    //   cmds[1].w1 = low 32 bits of CRC hash
    //
    //   n = (cmds[0].w0 >> 12) & 0xFF
    //   v0 = ((cmds[0].w0 >> 1) & 0x7F) - n  (same as regular G_VTX)

    uint32_t n = (uint32_t)((cmds[0].words.w0 >> 12) & 0xFF);
    uint32_t v0 = (uint32_t)(((cmds[0].words.w0 >> 1) & 0x7F) - n);

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
    // Step 2: Normalize to [0,1] by dividing by the current tile dimensions.
    // This matches the Fast3D interpreter which does: u = (tc/32.0) / tex_width
    // The result is [0,1] normalized UVs suitable for DX12 texture sampling with WRAP mode.
    float texelU = (float)v.v.tc[0] / 32.0f;
    float texelV = (float)v.v.tc[1] / 32.0f;
    float texW = (float)m_materialState.texWidth;
    float texH = (float)m_materialState.texHeight;
    out.uv[0] = (texW > 0.0f) ? (texelU / texW) : texelU;
    out.uv[1] = (texH > 0.0f) ? (texelV / texH) : texelV;

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
    m_materialState.textureAddr = cmd.words.w1;
    m_materialState.texFormat = (uint8_t)((cmd.words.w0 >> 21) & 0x07);
}

void SceneGeometryExtractor::HandleSetTextureImageOTRFilePath(const Gfx* cmds) {
    // 1-word command (matching the interpreter's gfx_set_timg_otr_filepath_handler_custom):
    //   cmds[0].w0 = opcode << 24 | fmt << 21 | size << 19 | (width-1)
    //   cmds[0].w1 = (uintptr_t) file path string
    // The interpreter does NOT advance cmd0, so this is a single-word command.
    const char* path = (const char*)cmds[0].words.w1;

    if (path) {
        m_materialState.textureAddr = (uintptr_t)path;
        // Also extract format info from w0 (same encoding as G_SETTIMG)
        m_materialState.texFormat = (uint8_t)((cmds[0].words.w0 >> 21) & 0x07);
        SPDLOG_TRACE("[RTX] SceneGeometryExtractor: G_SETTIMG_OTR_FILEPATH: {}", path);
    } else {
        SPDLOG_WARN("[RTX] SceneGeometryExtractor: G_SETTIMG_OTR_FILEPATH null path");
    }
}

uint32_t SceneGeometryExtractor::HandleSetTextureImageOTRHash(const Gfx* cmds) {
    // 2-word command: reconstruct the hash from cmd[1] (high in w0, low in w1).
    // Matches Fast3D interpreter convention.
    uint64_t hash = ((uint64_t)(cmds[1].words.w0) << 32) | (uint64_t)(cmds[1].words.w1);

    // Try to resolve the hash to a name. If we can, store the name pointer
    // as the texture address so the texture manager can load it later.
    // Otherwise, store the hash value directly (the texture manager will
    // handle both cases).
    static thread_local char hashNameBuf[256];
    char* name = ResourceMgr_GetNameByCRC(hash, hashNameBuf);

    if (name && name[0] != '\0') {
        m_materialState.textureAddr = (uintptr_t)name;
        SPDLOG_TRACE("[RTX] SceneGeometryExtractor: G_SETTIMG_OTR_HASH 0x{:016X} -> {}", hash, name);
    } else {
        // Store the raw hash. The texture manager will need to handle this.
        m_materialState.textureAddr = (uintptr_t)hash;
        SPDLOG_TRACE("[RTX] SceneGeometryExtractor: G_SETTIMG_OTR_HASH 0x{:016X} (no name)", hash);
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
    //   w1[19:18] = clamp_t
    //   w1[17:14] = mask_t
    //   w1[13:10] = shift_t
    //   w1[9:8]   = clamp_s
    //   w1[7:4]   = mask_s
    //   w1[3:0]   = shift_s
    //
    // We extract format info for combiner classification.
    uint8_t tileNum = (uint8_t)((cmd.words.w1 >> 24) & 0x07);

    if (tileNum == 0) {
        m_materialState.texFormat = (uint8_t)((cmd.words.w0 >> 21) & 0x07);
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

uint32_t SceneGeometryExtractor::GetOrCreateMaterial(ExtractedMesh& mesh) {
    bool isAlphaMesh = (&mesh == m_alphaMesh);
    auto& matMap = isAlphaMesh ? m_alphaMaterialMap : m_opaqueMaterialMap;

    CombinerMode combMode = ClassifyCombiner(m_materialState.combinerMode);
    bool alphaTest = m_materialState.alphaTest || DetectAlphaTest(m_materialState.otherModeL);
    bool isWater = DetectWaterTexture(m_materialState.textureAddr);

    MaterialKey key;
    key.textureAddr = m_materialState.textureAddr;
    key.combinerMode = static_cast<uint32_t>(combMode);
    key.alphaTest = alphaTest;
    key.isWater = isWater;

    auto it = matMap.find(key);
    if (it != matMap.end()) {
        return it->second;
    }

    // Create a new material.
    Material mat;
    // textureIndex is initially 0 (default white texture). It will be resolved
    // to the correct SRV index by RTXRenderer::ResolveMaterialTextures() using
    // the full texture address stored in materialTextureAddrs.
    mat.textureIndex = 0;
    mat.combinerMode = static_cast<uint32_t>(combMode);
    mat.isAlphaTested = alphaTest ? 1 : 0;
    mat.isWater = isWater ? 1 : 0;

    uint32_t matID = (uint32_t)mesh.materials.size();
    mesh.materials.push_back(mat);
    // Store the full texture address for later resolution by RTXRenderer.
    mesh.materialTextureAddrs.push_back(m_materialState.textureAddr);
    matMap[key] = matID;

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
    //   TEXEL0 = 1 in a0/b0, 1 in c0
    //   SHADE  = 4 in a0/b0, 2 in c0
    //   ENVIRONMENT = 5 in a0/b0, 3 in c0
    //   0 = 0 in c0 (pass through)

    // Extract cycle 1 color combiner fields.
    uint32_t upper = (uint32_t)(rawCombiner >> 32);
    uint32_t lower = (uint32_t)(rawCombiner);

    uint8_t a0 = (upper >> 20) & 0x0F;
    uint8_t c0 = (upper >> 12) & 0x07; // c0 is 3 bits for color
    uint8_t d0 = (upper >>  8) & 0x07;

    // Alpha combiner cycle 1 fields.
    uint8_t Aa0 = (upper >> 4) & 0x0F; // alpha A
    uint8_t Ac0 = (upper     ) & 0x07; // alpha C (multiplier)

    // Simple heuristic classification:

    // Check for environment blend: uses ENVIRONMENT in the combiner
    if (a0 == 5 || c0 == 3) {
        return COMBINER_TEX_ENV_BLEND;
    }

    // Check for SHADE only: no texture input
    bool hasTexel = (a0 == 1) || (c0 == 1) || (d0 == 1);
    bool hasShade = (a0 == 4) || (c0 == 2) || (d0 == 4);

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
        bool alphaHasTexel = (Aa0 == 1) || (Ac0 == 1);
        bool alphaHasShade = (Aa0 == 4) || (Ac0 == 2);

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

bool SceneGeometryExtractor::DetectWaterTexture(uintptr_t textureAddr) {
    if (textureAddr == 0) {
        return false;
    }

    // Heuristic: check if the texture path string contains "water" or "river"
    // or "stream". On the PC port, the texture address is often a pointer to
    // an OTR path string.
    //
    // We do a best-effort check: if the address looks like a valid string pointer
    // (i.e., starts with typical OTR path characters), scan for water keywords.

    // Guard against dereferencing small integer values that aren't real pointers.
    // On 64-bit Windows, user-mode addresses start at 0x10000 and above.
    // Anything below that threshold is almost certainly a raw N64 segment address,
    // not a valid host pointer.
    if (textureAddr < 0x10000) {
        return false;
    }

#ifdef _WIN32
    // On Windows, use IsBadReadPtr as an additional safety check before dereferencing.
    if (IsBadReadPtr((const void*)textureAddr, 1)) {
        return false;
    }
#endif

    const char* path = (const char*)textureAddr;

    // Basic sanity check: is this a plausible string pointer?
    // OTR paths typically start with a letter or "__OTR__".
    // We check the first byte is a printable ASCII character.
    if (path[0] < 0x20 || path[0] > 0x7E) {
        return false;
    }

    // Scan for water-related substrings (case-insensitive would be ideal,
    // but we check common patterns in OoT texture paths).
    // Common water texture paths in OoT contain: "water", "river", "mizu" (Japanese for water).
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

    if (containsCI(path, "water") || containsCI(path, "river") || containsCI(path, "mizu")) {
        return true;
    }

    return false;
}

} // namespace RTX

#endif // ENABLE_DX12_RTX
