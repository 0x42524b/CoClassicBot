#pragma once
#include "base.h"
#include <vector>
#include <memory>

// =====================================================================
// LayerInfo — per-cell terrain layer (linked list for multi-layer cells)
//
// Natural alignment (NOT packed) — these are heap-allocated game structs.
// sizeof(LayerInfo) = 16: [terrain:2][mask:2][altitude:2][pad:2][next:8]
// =====================================================================
struct LayerInfo
{
    uint16_t terrain;           // terrain type ID
    uint16_t mask;              // blocking mask (1 = blocked/non-walkable)
    int16_t  altitude;          // height/elevation
    LayerInfo* next;            // next layer in chain (nullptr if last)
};

static_assert(sizeof(LayerInfo) == 16, "LayerInfo must be 16 bytes (natural alignment)");

// =====================================================================
// CellInfo — single map cell (first layer + pathfinding flag)
//
// sizeof(CellInfo) = 24: [LayerInfo:16][searched:1][pad:7]
// =====================================================================
struct CellInfo
{
    LayerInfo layer;            // first (bottom) layer
    bool      searched;         // pathfinding visited flag
};

static_assert(sizeof(CellInfo) == 24, "CellInfo must be 24 bytes (natural alignment)");

// =====================================================================
// CGameMap — map struct for camera/viewport/cell access
//
// Located at base + 0x4E02E0.
// Offsets verified via Cheat Engine + Rust codebase.
// =====================================================================

// =====================================================================
// CMapItem — ground item on the map (dropped loot)
//
// Stored in CGameMap at +0x178 as std::vector<std::shared_ptr<CMapItem>>.
// Offsets from Rust conquer-bot: crates/api/src/items.rs
// =====================================================================
struct CMapItem
{
    OBJID    m_id;              // +0x00  item instance UID
    OBJID    m_idType;          // +0x04  item type ID
    Position m_pos;             // +0x08  tile position (x, y)
    void*    m_pInfo;           // +0x10  MapItemInfo* (rendering data)
    void*    m_pInfoCtrl;       // +0x18  shared_ptr control block

    int  GetQuality() const { return m_idType % 10; }
    // Plus is at MapItemInfo+0x48
    uint8_t GetPlus() const {
        if (!m_pInfo) return 0;
        return *(uint8_t*)((uintptr_t)m_pInfo + 0x48);
    }
};

static_assert(offsetof(CMapItem, m_id)     == 0x00, "CMapItem::m_id");
static_assert(offsetof(CMapItem, m_idType) == 0x04, "CMapItem::m_idType");
static_assert(offsetof(CMapItem, m_pos)    == 0x08, "CMapItem::m_pos");

#pragma pack(push, 1)
class CGameMap
{
private:
    BYTE _pad00[0x30];
public:
    Size m_sizeMap;          // +0x30  map dimensions in tiles
    Size m_sizeWorld;        // +0x38  world dimensions in pixels
private:
    BYTE _pad40[0x04];
public:
    Position m_posCameraPos;      // +0x44  isometric origin offset
    Position m_posViewport;       // +0x4C  camera scroll (viewport)
private:
    BYTE _pad54[0x04];
public:
    CellInfo* m_pCellInfo;      // +0x58  pointer to cell array [width * height]
private:
    BYTE _pad60[0x178 - 0x60];  // +0x60  gap to ground items vector
public:
    std::vector<std::shared_ptr<CMapItem>> m_vecItems; // +0x178 ground items on map
private:
    BYTE _pad198[0x200 - 0x178 - sizeof(std::vector<std::shared_ptr<CMapItem>>)]; // gap to map ID
public:
    OBJID m_idMap;               // +0x200 current map ID (e.g. 1002 = Twin City)
    OBJID m_idDoc;               // +0x204 map document ID

    OBJID GetId() const { return m_idMap; }

    // Get cell at tile coordinates (bounds-checked)
    CellInfo* GetCell(int x, int y) const {
        if (x < 0 || x >= m_sizeMap.iWidth || y < 0 || y >= m_sizeMap.iHeight)
            return nullptr;
        if (!m_pCellInfo) return nullptr;
        return &m_pCellInfo[x + y * m_sizeMap.iWidth];
    }

    // Get the topmost layer for a cell (follows linked list)
    static const LayerInfo* GetLastLayer(const CellInfo* cell) {
        if (!cell) return nullptr;
        const LayerInfo* layer = &cell->layer;
        while (layer->next)
            layer = layer->next;
        return layer;
    }

    // Get mask for a cell (1 = blocked)
    static uint16_t GetMask(const CellInfo* cell) {
        const LayerInfo* layer = GetLastLayer(cell);
        return layer ? layer->mask : 0;
    }

    // Check if a cell is walkable
    bool IsWalkable(int x, int y) const {
        return GetMask(GetCell(x, y)) != 1;
    }

    // Get altitude of a cell (topmost layer)
    static int16_t GetAltitude(const CellInfo* cell) {
        const LayerInfo* layer = GetLastLayer(cell);
        return layer ? layer->altitude : 0;
    }

    // Get terrain type of a cell (topmost layer)
    static uint16_t GetTerrain(const CellInfo* cell) {
        const LayerInfo* layer = GetLastLayer(cell);
        return layer ? layer->terrain : 0;
    }

    // Rounded integer distance (matches game formula, size=1)
    static int TileDist(int x0, int y0, int x1, int y1);

    // Bresenham line-of-sight: check altitude steps along the path
    bool CanReach(int ox, int oy, int tx, int ty, int altThreshold = 200) const;

    // Full jump validation: distance + walkability + altitude path
    static constexpr int MAX_JUMP_DIST = 18;
    static constexpr int ALT_THRESHOLD_NORMAL = 200;
    static constexpr int ALT_THRESHOLD_FLYING = 100;

    // Returns the correct altitude threshold based on the hero's fly state.
    static int GetHeroAltThreshold();

    bool CanJump(int ox, int oy, int tx, int ty, int altThreshold = ALT_THRESHOLD_NORMAL) const;

    // A* tile-level pathfinding (8-directional, respects walkability + altitude)
    // Returns empty vector if no path found. Includes start and goal.
    std::vector<Position> FindPath(int ox, int oy, int tx, int ty, int maxIter = 50000) const;

    // Simplify a tile path into jump waypoints (greedy: largest valid jumps)
    // Returns waypoints excluding the start position.
    std::vector<Position> SimplifyPath(const std::vector<Position>& tilePath) const;

    // Dump map cell data (mask + altitude per cell) to a binary file.
    // Format: [uint32 width][uint32 height][uint32 mapId][per cell: uint16 mask, int16 altitude]
    bool DumpToFile(const char* path) const;
};
#pragma pack(pop)

static_assert(offsetof(CGameMap, m_sizeMap)      == 0x30, "CGameMap::m_sizeMap");
static_assert(offsetof(CGameMap, m_posCameraPos) == 0x44, "CGameMap::m_posCameraPos");
static_assert(offsetof(CGameMap, m_posViewport)  == 0x4C, "CGameMap::m_posViewport");
static_assert(offsetof(CGameMap, m_pCellInfo)    == 0x58, "CGameMap::m_pCellInfo");
static_assert(offsetof(CGameMap, m_vecItems)     == 0x178, "CGameMap::m_vecItems");
static_assert(offsetof(CGameMap, m_idMap)        == 0x200, "CGameMap::m_idMap");
static_assert(offsetof(CGameMap, m_idDoc)        == 0x204, "CGameMap::m_idDoc");
