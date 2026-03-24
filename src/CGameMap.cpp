#include "CGameMap.h"
#include "CRole.h"
#include <cstdlib>
#include <queue>
#include <unordered_map>
#include <algorithm>

// Forward-declare to avoid pulling in game.h (which brings spdlog, breaking test builds).
extern uint64_t g_qwModuleBase;

int CGameMap::GetHeroAltThreshold()
{
    if (g_qwModuleBase == 0)
        return ALT_THRESHOLD_NORMAL;
    // CRoleMgr is at base + 0x4DF588; hero pointer is at CRoleMgr + 0x08.
    auto* mgr = *(void**)(g_qwModuleBase + 0x4DF588);
    if (!mgr)
        return ALT_THRESHOLD_NORMAL;
    auto* hero = *(CRole**)((uintptr_t)mgr + 0x08);
    if (hero && hero->IsFlyActive())
        return ALT_THRESHOLD_FLYING;
    return ALT_THRESHOLD_NORMAL;
}

int CGameMap::TileDist(int x0, int y0, int x1, int y1)
{
    int val = (x0 - x1) * (x0 - x1) + (y0 - y1) * (y0 - y1);
    int i = 1;
    while (val >= i * i) i++;
    int a = (i - 1) * (i - 1);
    return ((i * i - a) > (val - a) * 2) ? i - 1 : i;
}

bool CGameMap::CanReach(int ox, int oy, int tx, int ty, int altThreshold) const
{
    if (ox == tx && oy == ty) return true;

    const CellInfo* originCell = GetCell(ox, oy);
    if (!originCell) return false;
    int16_t originAlt = GetAltitude(originCell);

    int dx = abs(tx - ox);
    int dy = abs(ty - oy);
    int sx = (ox < tx) ? 1 : -1;
    int sy = (oy < ty) ? 1 : -1;
    int err = dx - dy;
    int cx = ox, cy = oy;

    for (;;) {
        if (cx == tx && cy == ty) break;

        int e2 = 2 * err;
        bool stepX = e2 > -dy;
        bool stepY = e2 < dx;

        if (stepX) { err -= dy; cx += sx; }
        if (stepY) { err += dx; cy += sy; }

        const CellInfo* cell = GetCell(cx, cy);
        if (!cell) return false;

        int16_t alt = GetAltitude(cell);
        if (abs(alt - originAlt) > altThreshold) {
            // Diagonal step clipped through a corner — allow only if BOTH
            // cardinal neighbors have compatible altitude. If either is also
            // a wall, the obstruction is a wall edge, not a passable corner.
            if (stepX && stepY) {
                const int16_t altX = GetAltitude(GetCell(cx, cy - sy));
                const int16_t altY = GetAltitude(GetCell(cx - sx, cy));
                if (abs(altX - originAlt) <= altThreshold && abs(altY - originAlt) <= altThreshold)
                    continue;
            }
            return false;
        }
    }
    return true;
}

bool CGameMap::CanJump(int ox, int oy, int tx, int ty, int altThreshold) const
{
    if (ox == tx && oy == ty) return false;
    if (TileDist(ox, oy, tx, ty) > MAX_JUMP_DIST) return false;
    if (!IsWalkable(tx, ty)) return false;

    // Direct endpoint altitude check — CanReach's corner relaxation can
    // forgive the destination cell when it arrives via a diagonal step,
    // so we always verify origin vs destination altitude explicitly.
    // The game only checks upward (destAlt - originAlt), not abs().
    const int16_t originAlt = GetAltitude(GetCell(ox, oy));
    const int16_t destAlt = GetAltitude(GetCell(tx, ty));
    if (destAlt - originAlt > altThreshold) return false;

    if (!CanReach(ox, oy, tx, ty, altThreshold)) return false;
    return true;
}

// =====================================================================
// A* pathfinding (8-directional, tile-level)
// =====================================================================

struct AStarNode {
    int x, y;
    int g;      // cost from start
    int f;      // g + heuristic
};

struct AStarCmp {
    bool operator()(const AStarNode& a, const AStarNode& b) const {
        return a.f > b.f;
    }
};

// Pack (x, y) into a single 64-bit key for the hash map
static inline uint64_t PosKey(int x, int y) {
    return ((uint64_t)(uint32_t)x << 32) | (uint32_t)y;
}

// Octile distance heuristic (scaled by 10)
static inline int Heuristic(int x0, int y0, int x1, int y1) {
    int dx = abs(x0 - x1);
    int dy = abs(y0 - y1);
    // 10 * max(dx,dy) + 4 * min(dx,dy)  ≈ 10*cardinal + 14*diagonal
    return (dx > dy) ? (10 * dx + 4 * dy) : (10 * dy + 4 * dx);
}

static constexpr int DX8[] = { -1, -1, -1,  0, 0,  1, 1, 1 };
static constexpr int DY8[] = { -1,  0,  1, -1, 1, -1, 0, 1 };
static constexpr int COST8[] = { 14, 10, 14, 10, 10, 14, 10, 14 };

std::vector<Position> CGameMap::FindPath(int ox, int oy, int tx, int ty, int maxIter) const
{
    if (ox == tx && oy == ty) return { Position(ox, oy) };
    if (!IsWalkable(tx, ty)) return {};

    std::priority_queue<AStarNode, std::vector<AStarNode>, AStarCmp> open;
    std::unordered_map<uint64_t, uint64_t> cameFrom;  // key → parent key
    std::unordered_map<uint64_t, int>      gScore;

    uint64_t startKey = PosKey(ox, oy);
    uint64_t goalKey  = PosKey(tx, ty);

    open.push({ ox, oy, 0, Heuristic(ox, oy, tx, ty) });
    gScore[startKey] = 0;
    cameFrom[startKey] = startKey;  // sentinel: start is its own parent

    int iterations = 0;
    bool found = false;

    while (!open.empty() && iterations < maxIter) {
        iterations++;
        AStarNode cur = open.top();
        open.pop();

        uint64_t curKey = PosKey(cur.x, cur.y);
        if (curKey == goalKey) { found = true; break; }

        // Skip stale entries
        if (cur.g > gScore[curKey]) continue;

        const CellInfo* curCell = GetCell(cur.x, cur.y);
        int16_t curAlt = GetAltitude(curCell);

        for (int d = 0; d < 8; d++) {
            int nx = cur.x + DX8[d];
            int ny = cur.y + DY8[d];

            if (!IsWalkable(nx, ny)) continue;

            // Altitude check for this single step
            const CellInfo* nCell = GetCell(nx, ny);
            int16_t nAlt = GetAltitude(nCell);
            if (abs(nAlt - curAlt) > 200) continue;

            int ng = cur.g + COST8[d];
            uint64_t nKey = PosKey(nx, ny);

            auto it = gScore.find(nKey);
            if (it != gScore.end() && ng >= it->second) continue;

            gScore[nKey] = ng;
            cameFrom[nKey] = curKey;
            open.push({ nx, ny, ng, ng + Heuristic(nx, ny, tx, ty) });
        }
    }

    if (!found) return {};

    // Reconstruct path
    std::vector<Position> path;
    uint64_t key = goalKey;
    while (key != startKey) {
        int px = (int)(key >> 32);
        int py = (int)(key & 0xFFFFFFFF);
        path.push_back(Position(px, py));
        key = cameFrom[key];
    }
    path.push_back(Position(ox, oy));
    std::reverse(path.begin(), path.end());
    return path;
}

// =====================================================================
// Simplify tile path into jump waypoints (greedy largest jumps)
// =====================================================================
std::vector<Position> CGameMap::SimplifyPath(const std::vector<Position>& tilePath) const
{
    if (tilePath.size() <= 1) return {};

    std::vector<Position> waypoints;
    size_t cur = 0;

    while (cur < tilePath.size() - 1) {
        // Find the furthest tile along the path we can jump to from cur
        size_t best = cur + 1;
        for (size_t ahead = tilePath.size() - 1; ahead > cur + 1; ahead--) {
            if (CanJump(tilePath[cur].x, tilePath[cur].y,
                        tilePath[ahead].x, tilePath[ahead].y, CGameMap::GetHeroAltThreshold())) {
                best = ahead;
                break;
            }
        }
        waypoints.push_back(tilePath[best]);
        cur = best;
    }

    return waypoints;
}

bool CGameMap::DumpToFile(const char* path) const
{
    if (!m_pCellInfo || m_sizeMap.iWidth <= 0 || m_sizeMap.iHeight <= 0)
        return false;

    FILE* f = fopen(path, "wb");
    if (!f) return false;

    const uint32_t w = (uint32_t)m_sizeMap.iWidth;
    const uint32_t h = (uint32_t)m_sizeMap.iHeight;
    const uint32_t id = m_idMap;
    fwrite(&w, 4, 1, f);
    fwrite(&h, 4, 1, f);
    fwrite(&id, 4, 1, f);

    for (uint32_t i = 0; i < w * h; ++i) {
        const CellInfo& cell = m_pCellInfo[i];
        uint16_t mask = GetMask(&cell);
        int16_t alt = GetAltitude(&cell);
        fwrite(&mask, 2, 1, f);
        fwrite(&alt, 2, 1, f);
    }

    fclose(f);
    return true;
}
