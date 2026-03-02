#pragma once
#ifndef SCENE_GEOMETRY_EXTRACTOR_H
#define SCENE_GEOMETRY_EXTRACTOR_H

#ifdef ENABLE_DX12_RTX

#include "RTXTypes.h"
#include <vector>
#include <deque>
#include <unordered_map>
#include <string>
#include <cstdint>

// These headers have their own extern "C" guards internally.
#include "global.h"
#include "z64.h"

namespace RTX {

// Maximum number of vertex buffer slots on the N64 RSP (F3DEX2 microcode).
static constexpr uint32_t N64_VERTEX_BUFFER_SIZE = 32;

// Maximum display list recursion depth to prevent infinite loops from malformed data.
// Complex OoT rooms like Kokiri Forest have deeply nested display list hierarchies
// (room DL → sub-DL → texture setup DL → geometry DL). 24 handles all known cases.
static constexpr uint32_t MAX_DL_RECURSION_DEPTH = 24;

// Maximum number of Gfx commands to process per display list as a safety limit.
// Kokiri Forest and other complex OoT rooms can have very large display lists
// with many sub-DL calls, texture changes, and geometry batches. The Deku Tree
// background geometry in particular is deep in the display list hierarchy.
// 16384 accommodates even the largest OoT rooms without truncation.
static constexpr uint32_t MAX_COMMANDS_PER_DL = 16384;

class SceneGeometryExtractor {
public:
    SceneGeometryExtractor();
    ~SceneGeometryExtractor();

    // Extract all geometry from a room's mesh header display lists.
    // Returns a RoomGeometry struct containing opaque and alpha-tested meshes
    // with deduplicated materials and per-triangle material IDs.
    RoomGeometry ExtractRoomGeometry(Room* room, uint32_t roomIndex);

    // Reset all internal state. Called automatically at the start of ExtractRoomGeometry.
    void Reset();

private:
    // ---- Display list walking ----

    // Walk a single display list, emitting vertices/triangles into the target mesh.
    // isTranslucent: true if this DL came from the xlu (translucent) pointer.
    // depth: current recursion depth for G_DL sub-calls.
    void WalkDisplayList(const Gfx* dl, bool isTranslucent, uint32_t depth);

    // Inner implementation of WalkDisplayList, separated for SEH wrapping.
    void WalkDisplayListInner(const Gfx* dl, bool isTranslucent, uint32_t depth);

    // ---- Command handlers ----

    // G_VTX (0x01): Load vertices into the RSP vertex buffer.
    void HandleVertexLoad(const Gfx& cmd);

    // G_VTX_OTR_FILEPATH (0x24): OTR 2-word vertex load by file path.
    // Returns number of extra Gfx words consumed (1).
    uint32_t HandleVertexLoadOTRFilePath(const Gfx* cmds);

    // G_VTX_OTR_HASH (0x32): OTR 2-word vertex load by CRC hash.
    // Returns number of extra Gfx words consumed (1).
    uint32_t HandleVertexLoadOTRHash(const Gfx* cmds);

    // G_TRI1 (0x05): Emit one triangle.
    void HandleTri1(const Gfx& cmd, bool isTranslucent);

    // G_TRI2 (0x06): Emit two triangles.
    void HandleTri2(const Gfx& cmd, bool isTranslucent);

    // G_SETCOMBINE (0xFC): Update combiner mode in material state.
    void HandleSetCombine(const Gfx& cmd);

    // G_SETTIMG (0xFD): Update texture image address in material state.
    void HandleSetTextureImage(const Gfx& cmd);

    // G_SETTIMG_OTR_FILEPATH (0x25): OTR 1-word set texture image by path.
    // Unlike the HASH variant, this is a single-word command (the interpreter
    // handler does not advance cmd0).
    void HandleSetTextureImageOTRFilePath(const Gfx* cmds);

    // G_SETTIMG_OTR_HASH (0x20): OTR 2-word set texture image by hash.
    // Returns number of extra Gfx words consumed (1).
    uint32_t HandleSetTextureImageOTRHash(const Gfx* cmds);

    // G_GEOMETRYMODE (0xD9): Update geometry mode (lighting, etc).
    void HandleGeometryMode(const Gfx& cmd);

    // G_SETOTHERMODE_L (0xE2): Update other mode low (render mode).
    void HandleSetOtherModeL(const Gfx& cmd);

    // G_SETTILE (0xF5): Update tile descriptor (texture dimensions).
    void HandleSetTile(const Gfx& cmd);

    // G_SETTILESIZE (0xF2): Update tile size (for UV normalization).
    void HandleSetTileSize(const Gfx& cmd);

    // ---- Vertex/triangle emission ----

    // Convert an N64 Vtx to an RTXVertex using current material state.
    RTXVertex ConvertVertex(const Vtx& v) const;

    // Emit a single triangle (3 vertex buffer indices) into the appropriate mesh.
    void EmitTriangle(uint32_t vi0, uint32_t vi1, uint32_t vi2, bool isTranslucent);

    // ---- Material management ----

    // Get or create a material from the current N64MaterialState.
    // Returns the material ID in the target mesh.
    uint32_t GetOrCreateMaterial(ExtractedMesh& mesh);

    // Classify the raw 64-bit combiner mode into a simplified CombinerMode enum.
    static CombinerMode ClassifyCombiner(uint64_t rawCombiner);

    // Detect if the current otherModeL indicates alpha testing.
    static bool DetectAlphaTest(uint32_t otherModeL);

    // Detect if the current texture path is a water surface.
    static bool DetectWaterTexture(uintptr_t textureAddr);

    // Detect water via N64 render mode (otherModeL) combined with translucent context.
    // OoT water uses ZMODE_XLU + FORCE_BL without CVG_X_ALPHA (blended, not alpha-tested).
    // Returns true if the render mode pattern matches typical OoT water surfaces.
    static bool DetectWaterRenderMode(uint32_t otherModeL, bool isTranslucent);

    // Detect if the current otherModeL indicates decal/overlay rendering mode.
    // Decals use ZMODE_DEC (Z mode = decal) which renders at the same Z depth as
    // the underlying surface. In OoT, dirt paths, road markings, and ground overlays
    // use this mode (G_RM_ZB_OVL_SURF, G_RM_ZB_CLD_SURF with ZMODE_DEC).
    // Returns true if the render mode indicates decal geometry.
    static bool DetectDecalMode(uint32_t otherModeL);

    // ---- State ----

    // N64 RSP vertex buffer (32 slots).
    Vtx m_vertexBuffer[N64_VERTEX_BUFFER_SIZE];
    bool m_vertexBufferValid[N64_VERTEX_BUFFER_SIZE];

    // Current N64 material/render state being tracked as we walk the display list.
    N64MaterialState m_materialState;

    // Output meshes being built (pointers into the current RoomGeometry).
    ExtractedMesh* m_opaqueMesh;
    ExtractedMesh* m_alphaMesh;

    // Material deduplication: key is (textureAddr, combinerMode, alphaTest, isWater),
    // value is material index in the respective ExtractedMesh::materials vector.
    struct MaterialKey {
        uintptr_t textureAddr;
        uint32_t combinerMode;
        bool alphaTest;
        bool isWater;
        bool isDecal;
        uint8_t wrapModeS;
        uint8_t wrapModeT;

        bool operator==(const MaterialKey& other) const {
            return textureAddr == other.textureAddr &&
                   combinerMode == other.combinerMode &&
                   alphaTest == other.alphaTest &&
                   isWater == other.isWater &&
                   isDecal == other.isDecal &&
                   wrapModeS == other.wrapModeS &&
                   wrapModeT == other.wrapModeT;
        }
    };

    struct MaterialKeyHash {
        size_t operator()(const MaterialKey& k) const {
            size_t h = std::hash<uintptr_t>()(k.textureAddr);
            h ^= std::hash<uint32_t>()(k.combinerMode) << 1;
            h ^= std::hash<bool>()(k.alphaTest) << 2;
            h ^= std::hash<bool>()(k.isWater) << 3;
            h ^= std::hash<uint8_t>()(k.wrapModeS) << 4;
            h ^= std::hash<uint8_t>()(k.wrapModeT) << 5;
            h ^= std::hash<bool>()(k.isDecal) << 6;
            return h;
        }
    };

    std::unordered_map<MaterialKey, uint32_t, MaterialKeyHash> m_opaqueMaterialMap;
    std::unordered_map<MaterialKey, uint32_t, MaterialKeyHash> m_alphaMaterialMap;

    // Persistent storage for OTR path strings resolved from CRC hashes.
    // HandleSetTextureImageOTRHash uses ResourceMgr_GetNameByCRC which writes
    // to a thread_local buffer. We must copy strings here so the pointer stored
    // in m_materialState.textureAddr remains valid until ResolveMaterialTextures
    // runs (which happens after the display list walk completes).
    // NOTE: We use std::deque instead of std::vector because deque does NOT
    // invalidate element pointers/references when new elements are pushed.
    // This is critical since we store c_str() pointers in textureAddr.
    std::deque<std::string> m_persistentPathStrings;

    // Statistics for logging.
    uint32_t m_totalCommandsProcessed;
    uint32_t m_totalTrianglesEmitted;
    uint32_t m_totalVerticesLoaded;
    uint32_t m_totalDLsWalked;
};

} // namespace RTX

#endif // ENABLE_DX12_RTX
#endif // SCENE_GEOMETRY_EXTRACTOR_H
