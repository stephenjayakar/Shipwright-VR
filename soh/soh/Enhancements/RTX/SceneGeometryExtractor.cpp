#ifdef ENABLE_DX12_RTX

#include "SceneGeometryExtractor.h"
#include <spdlog/spdlog.h>
#include <cstring>
#include <cmath>
#include <algorithm>

extern "C" {
#include "macros.h"
}

// Forward declarations for OTR resource loading functions.
extern "C" {
    Vtx* ResourceMgr_LoadVtxByName(char* path);
    Vtx* ResourceMgr_LoadVtxByCRC(uint64_t crc);
    Gfx* ResourceMgr_LoadGfxByName(const char* path);
    Gfx* ResourceMgr_LoadGfxByCRC(uint64_t crc);
    char* ResourceMgr_GetNameByCRC(uint64_t crc, char* alloc);
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
    Reset();

    RoomGeometry result;
    result.roomIndex = roomIndex;

    if (!room) {
        SPDLOG_ERROR("[RTX] SceneGeometryExtractor: null Room pointer for room {}", roomIndex);
        return result;
    }

    MeshHeader* meshHeader = room->meshHeader;
    if (!meshHeader) {
        SPDLOG_ERROR("[RTX] SceneGeometryExtractor: null meshHeader for room {}", roomIndex);
        return result;
    }

    m_opaqueMesh = &result.opaqueMesh;
    m_alphaMesh = &result.alphaMesh;

    uint8_t meshType = meshHeader->base.type;
    SPDLOG_INFO("[RTX] SceneGeometryExtractor: room {} meshType={}", roomIndex, meshType);

    if (meshType == 0) {
        // PolygonType0: array of PolygonDlist entries (opa + xlu display lists).
        PolygonType0* poly0 = &meshHeader->polygon0;
        uint8_t numEntries = poly0->num;
        PolygonDlist* dlists = (PolygonDlist*)SEGMENTED_TO_VIRTUAL(poly0->start);

        if (!dlists) {
            SPDLOG_WARN("[RTX] SceneGeometryExtractor: null dlist start for room {} type 0", roomIndex);
            return result;
        }

        SPDLOG_INFO("[RTX] SceneGeometryExtractor: room {} type 0 with {} DL entries", roomIndex, numEntries);

        for (uint8_t i = 0; i < numEntries; i++) {
            // Opaque display list
            if (dlists[i].opa) {
                Gfx* opaDL = (Gfx*)SEGMENTED_TO_VIRTUAL(dlists[i].opa);
                SPDLOG_DEBUG("[RTX] SceneGeometryExtractor: room {} entry {} walking opa DL at {:p}",
                             roomIndex, i, (void*)opaDL);
                m_materialState.Reset();
                WalkDisplayList(opaDL, /*isTranslucent=*/false, /*depth=*/0);
            }

            // Translucent display list
            if (dlists[i].xlu) {
                Gfx* xluDL = (Gfx*)SEGMENTED_TO_VIRTUAL(dlists[i].xlu);
                SPDLOG_DEBUG("[RTX] SceneGeometryExtractor: room {} entry {} walking xlu DL at {:p}",
                             roomIndex, i, (void*)xluDL);
                m_materialState.Reset();
                WalkDisplayList(xluDL, /*isTranslucent=*/true, /*depth=*/0);
            }
        }
    } else if (meshType == 2) {
        // PolygonType2: array of PolygonDlist2 entries with world-space positions.
        PolygonType2* poly2 = &meshHeader->polygon2;
        uint8_t numEntries = poly2->num;
        PolygonDlist2* dlists = (PolygonDlist2*)SEGMENTED_TO_VIRTUAL(poly2->start);

        if (!dlists) {
            SPDLOG_WARN("[RTX] SceneGeometryExtractor: null dlist start for room {} type 2", roomIndex);
            return result;
        }

        SPDLOG_INFO("[RTX] SceneGeometryExtractor: room {} type 2 with {} DL entries", roomIndex, numEntries);

        for (uint8_t i = 0; i < numEntries; i++) {
            // Opaque display list
            if (dlists[i].opa) {
                Gfx* opaDL = (Gfx*)SEGMENTED_TO_VIRTUAL(dlists[i].opa);
                SPDLOG_DEBUG("[RTX] SceneGeometryExtractor: room {} entry {} walking opa DL (type2) at {:p}",
                             roomIndex, i, (void*)opaDL);
                m_materialState.Reset();
                WalkDisplayList(opaDL, /*isTranslucent=*/false, /*depth=*/0);
            }

            // Translucent display list
            if (dlists[i].xlu) {
                Gfx* xluDL = (Gfx*)SEGMENTED_TO_VIRTUAL(dlists[i].xlu);
                SPDLOG_DEBUG("[RTX] SceneGeometryExtractor: room {} entry {} walking xlu DL (type2) at {:p}",
                             roomIndex, i, (void*)xluDL);
                m_materialState.Reset();
                WalkDisplayList(xluDL, /*isTranslucent=*/true, /*depth=*/0);
            }
        }
    } else if (meshType == 1) {
        // PolygonType1: pre-rendered background. Has a single display list for
        // foreground geometry but no real 3D room mesh. We extract what we can.
        PolygonType1* poly1 = (PolygonType1*)meshHeader;
        if (poly1->dlist) {
            Gfx* dl = (Gfx*)SEGMENTED_TO_VIRTUAL(poly1->dlist);
            SPDLOG_INFO("[RTX] SceneGeometryExtractor: room {} type 1 (pre-rendered bg), walking fg DL at {:p}",
                        roomIndex, (void*)dl);
            m_materialState.Reset();
            WalkDisplayList(dl, /*isTranslucent=*/false, /*depth=*/0);
        } else {
            SPDLOG_INFO("[RTX] SceneGeometryExtractor: room {} type 1 with no foreground DL, skipping", roomIndex);
        }
    } else {
        SPDLOG_WARN("[RTX] SceneGeometryExtractor: unsupported mesh type {} for room {}", meshType, roomIndex);
    }

    // Set alpha-test flags on meshes.
    result.opaqueMesh.hasAlphaTest = false;
    result.alphaMesh.hasAlphaTest = true;

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

void SceneGeometryExtractor::WalkDisplayList(const Gfx* dl, bool isTranslucent, uint32_t depth) {
    if (!dl) {
        return;
    }

    if (depth >= MAX_DL_RECURSION_DEPTH) {
        SPDLOG_WARN("[RTX] SceneGeometryExtractor: max DL recursion depth {} reached, stopping", depth);
        return;
    }

    m_totalDLsWalked++;
    uint32_t cmdCount = 0;

    for (const Gfx* cmd = dl; cmdCount < MAX_COMMANDS_PER_DL; cmd++, cmdCount++) {
        uint8_t opcode = (uint8_t)(cmd->words.w0 >> 24);
        m_totalCommandsProcessed++;

        switch (opcode) {

        // ---- Vertex loading ----
        case G_VTX:
            HandleVertexLoad(*cmd);
            break;

        case G_VTX_OTR_FILEPATH: {
            uint32_t extra = HandleVertexLoadOTRFilePath(cmd);
            cmd += extra;
            cmdCount += extra;
            break;
        }

        case G_VTX_OTR_HASH: {
            uint32_t extra = HandleVertexLoadOTRHash(cmd);
            cmd += extra;
            cmdCount += extra;
            break;
        }

        // ---- Triangle emission ----
        case G_TRI1:
            HandleTri1(*cmd, isTranslucent);
            break;

        case G_TRI2:
            HandleTri2(*cmd, isTranslucent);
            break;

        // ---- Material state ----
        case G_SETCOMBINE:
            HandleSetCombine(*cmd);
            break;

        case G_SETTIMG:
            HandleSetTextureImage(*cmd);
            break;

        case G_SETTIMG_OTR_FILEPATH: {
            uint32_t extra = HandleSetTextureImageOTRFilePath(cmd);
            cmd += extra;
            cmdCount += extra;
            break;
        }

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

        // ---- Display list control flow ----
        case G_DL: {
            // Bit 16 of w0: 0 = push (return after child), 1 = branch (replace current)
            bool isBranch = ((cmd->words.w0 >> 16) & 0x01) != 0;
            Gfx* childDL = (Gfx*)SEGMENTED_TO_VIRTUAL((void*)cmd->words.w1);

            if (childDL) {
                if (isBranch) {
                    // Branch: replace current DL (tail call). Walk child and return.
                    WalkDisplayList(childDL, isTranslucent, depth + 1);
                    return;
                } else {
                    // Push: walk child, then continue with next command.
                    WalkDisplayList(childDL, isTranslucent, depth + 1);
                }
            }
            break;
        }

        case G_DL_OTR_FILEPATH: {
            // 2-word OTR sub-DL call by file path.
            // cmd[0].w1 = pointer to file path string
            // cmd[1] = additional data (not used for path resolution)
            const char* path = (const char*)cmd[0].words.w1;
            bool isBranch = ((cmd[0].words.w0 >> 16) & 0x01) != 0;

            Gfx* childDL = nullptr;
            if (path) {
                childDL = ResourceMgr_LoadGfxByName(path);
            }

            // Advance past the second word of this 2-word command.
            cmd++;
            cmdCount++;

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
            // 2-word OTR sub-DL call by CRC hash.
            // The hash is encoded across the two command words.
            uint64_t hash = ((uint64_t)(cmd[0].words.w1) << 32) | (uint64_t)(cmd[1].words.w0);
            bool isBranch = ((cmd[0].words.w0 >> 16) & 0x01) != 0;

            Gfx* childDL = ResourceMgr_LoadGfxByCRC(hash);

            // Advance past the second word.
            cmd++;
            cmdCount++;

            if (childDL) {
                if (isBranch) {
                    WalkDisplayList(childDL, isTranslucent, depth + 1);
                    return;
                } else {
                    WalkDisplayList(childDL, isTranslucent, depth + 1);
                }
            } else {
                SPDLOG_WARN("[RTX] SceneGeometryExtractor: G_DL_OTR_HASH failed to load hash 0x{:016X}", hash);
            }
            break;
        }

        case G_ENDDL:
            // End of this display list.
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
        case G_SETTILESIZE:
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
        case G_MTX_OTR_FILEPATH:
        case G_MTX_OTR:
        case G_MOVEMEM_OTR:
            // These are 2-word OTR commands; skip the extra word.
            cmd++;
            cmdCount++;
            break;

        case G_MARKER:
        case G_INVALTEXCACHE:
        case G_PUSHCD:
        case G_SETFB:
        case G_RESETFB:
        case G_SETTIMG_FB:
        case G_BRANCH_Z_OTR:
        case G_TRI1_OTR:
        case G_TEXRECT_WIDE:
        case G_FILLWIDERECT:
            // OTR-specific commands. Some are multi-word; for safety, the ones
            // that might be 2-word we handle case-by-case. These single-word
            // variants are safe to skip.
            break;

        default:
            // Unknown opcode. Log at trace level to avoid spamming.
            SPDLOG_TRACE("[RTX] SceneGeometryExtractor: unknown opcode 0x{:02X} at DL offset {}",
                         opcode, cmdCount);
            break;
        }
    }

    if (cmdCount >= MAX_COMMANDS_PER_DL) {
        SPDLOG_WARN("[RTX] SceneGeometryExtractor: hit max command limit ({}) in DL at {:p}",
                     MAX_COMMANDS_PER_DL, (const void*)dl);
    }
}

// ============================================================================
// Vertex loading
// ============================================================================

void SceneGeometryExtractor::HandleVertexLoad(const Gfx& cmd) {
    // F3DEX2 G_VTX encoding:
    //   w0: [31:24] opcode  [23:12] n (num verts * 2, or shifted)  [6:1] v0+n
    //   Actually: n = (w0 >> 12) & 0xFF, v0 = ((w0 >> 1) & 0x7F) - n
    //   w1: pointer to Vtx array
    uint32_t n  = (cmd.words.w0 >> 12) & 0xFF;
    uint32_t v0idx = ((cmd.words.w0 >> 1) & 0x7F) - n;
    Vtx* vtxData = (Vtx*)SEGMENTED_TO_VIRTUAL((void*)cmd.words.w1);

    if (!vtxData) {
        SPDLOG_WARN("[RTX] SceneGeometryExtractor: G_VTX null vertex pointer");
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
    // 2-word command:
    //   cmds[0].w0 = opcode << 24 | (numVerts << 12)
    //   cmds[0].w1 = low 32 bits of hash (or high, depending on encoding)
    //   cmds[1].w0 = high 32 bits of hash (or vertex count)
    //   cmds[1].w1 = (bufferIndex << 16) | dataOffset
    //
    // The exact encoding varies, but we follow the SoH OTR convention:
    //   n = (cmds[0].w0 >> 12) & 0xFF
    //   v0 = ((cmds[0].w0 >> 1) & 0x7F) - n  (same as regular G_VTX)
    //   hash is assembled from cmds[0].w1 and cmds[1].w0

    uint32_t n = (uint32_t)((cmds[0].words.w0 >> 12) & 0xFF);
    uint32_t v0 = (uint32_t)(((cmds[0].words.w0 >> 1) & 0x7F) - n);

    // Reconstruct CRC hash
    uint64_t hash = ((uint64_t)(cmds[0].words.w1) << 32) | (uint64_t)(cmds[1].words.w0);

    Vtx* vtxData = ResourceMgr_LoadVtxByCRC(hash);

    if (!vtxData) {
        // Try name-based fallback
        char nameBuf[256] = {};
        char* name = ResourceMgr_GetNameByCRC(hash, nameBuf);
        if (name && name[0] != '\0') {
            vtxData = ResourceMgr_LoadVtxByName(name);
        }
    }

    if (!vtxData) {
        SPDLOG_WARN("[RTX] SceneGeometryExtractor: G_VTX_OTR_HASH failed to load hash 0x{:016X}", hash);
        return 1;
    }

    if (v0 + n > N64_VERTEX_BUFFER_SIZE) {
        SPDLOG_WARN("[RTX] SceneGeometryExtractor: G_VTX_OTR_HASH out of bounds: v0={} n={}", v0, n);
        n = (v0 < N64_VERTEX_BUFFER_SIZE) ? (N64_VERTEX_BUFFER_SIZE - v0) : 0;
    }

    for (uint32_t i = 0; i < n; i++) {
        m_vertexBuffer[v0 + i] = vtxData[i];
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
        SPDLOG_WARN("[RTX] SceneGeometryExtractor: triangle index out of range: {}, {}, {} (max {})",
                     vi0, vi1, vi2, N64_VERTEX_BUFFER_SIZE - 1);
        return;
    }

    if (!m_vertexBufferValid[vi0] || !m_vertexBufferValid[vi1] || !m_vertexBufferValid[vi2]) {
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

    // Texture coordinates: 10.5 fixed-point -> float.
    out.uv[0] = (float)v.v.tc[0] / 32.0f;
    out.uv[1] = (float)v.v.tc[1] / 32.0f;

    // Vertex color RGBA [0,1]. Always stored regardless of lighting mode,
    // since the shader may use vertex color for modulation.
    out.color[0] = v.v.cn[0] / 255.0f;
    out.color[1] = v.v.cn[1] / 255.0f;
    out.color[2] = v.v.cn[2] / 255.0f;
    out.color[3] = v.v.cn[3] / 255.0f;

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

uint32_t SceneGeometryExtractor::HandleSetTextureImageOTRFilePath(const Gfx* cmds) {
    // 2-word command:
    //   cmds[0].w1 = (uintptr_t) file path string
    //   cmds[1] = additional data (width/height info may be encoded here)
    const char* path = (const char*)cmds[0].words.w1;

    if (path) {
        m_materialState.textureAddr = (uintptr_t)path;
        SPDLOG_TRACE("[RTX] SceneGeometryExtractor: G_SETTIMG_OTR_FILEPATH: {}", path);
    } else {
        SPDLOG_WARN("[RTX] SceneGeometryExtractor: G_SETTIMG_OTR_FILEPATH null path");
    }

    return 1; // consumed 1 extra Gfx word
}

uint32_t SceneGeometryExtractor::HandleSetTextureImageOTRHash(const Gfx* cmds) {
    // 2-word command: reconstruct the hash.
    uint64_t hash = ((uint64_t)(cmds[0].words.w1) << 32) | (uint64_t)(cmds[1].words.w0);

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
    //   w0[23:0] = clear bits (inverted mask, bits to AND out)
    //   w1 = set bits (bits to OR in)
    //
    // F3DEX2 encoding: the clear bits in w0 are stored inverted.
    // newMode = (oldMode & ~clearBits) | setBits
    // In the Gfx command, w0 contains ~clearBits in the lower 24 bits.
    uint32_t clearBits = ~((uint32_t)(cmd.words.w0)) & 0x00FFFFFF;
    uint32_t setBits = (uint32_t)(cmd.words.w1);

    m_materialState.geometryMode = (m_materialState.geometryMode & ~clearBits) | setBits;
    m_materialState.lightingEnabled = (m_materialState.geometryMode & G_LIGHTING) != 0;
}

void SceneGeometryExtractor::HandleSetOtherModeL(const Gfx& cmd) {
    // G_SETOTHERMODE_L (0xE2) in F3DEX2:
    //   w0[7:0] = shift count (sft)
    //   w0[15:8] = length - 1 (len)
    //   w1 = data to set
    //
    // Builds a mask from shift and length, then:
    //   otherModeL = (otherModeL & ~mask) | data
    uint32_t sft = (uint32_t)(cmd.words.w0 & 0xFF);
    uint32_t len = ((uint32_t)(cmd.words.w0 >> 8) & 0xFF) + 1;
    uint32_t data = (uint32_t)cmd.words.w1;

    // Build the mask.
    uint32_t mask = (len == 32) ? 0xFFFFFFFF : (((1u << len) - 1) << sft);

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
