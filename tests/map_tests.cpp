#include "CGameMap.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <cmath>

// Stub for base.h global (not used by CGameMap algorithms)
ULONG64 g_qwModuleBase = 0;

// =====================================================================
// TestMap — helper to build a CGameMap with custom cell data
// =====================================================================
class TestMap {
public:
    TestMap(int width, int height)
        : m_width(width), m_height(height)
    {
        m_cells.resize(width * height);
        memset(m_cells.data(), 0, m_cells.size() * sizeof(CellInfo));
        BuildGameMap();
    }

    // Load from a dump file produced by CGameMap::DumpToFile
    static TestMap LoadDump(const char* path) {
        FILE* f = fopen(path, "rb");
        if (!f) {
            fprintf(stderr, "Failed to open dump: %s\n", path);
            exit(1);
        }
        uint32_t w, h, id;
        fread(&w, 4, 1, f);
        fread(&h, 4, 1, f);
        fread(&id, 4, 1, f);

        TestMap tm((int)w, (int)h);
        tm.m_mapId = id;
        for (uint32_t i = 0; i < w * h; ++i) {
            uint16_t mask;
            int16_t alt;
            fread(&mask, 2, 1, f);
            fread(&alt, 2, 1, f);
            tm.m_cells[i].layer.mask = mask;
            tm.m_cells[i].layer.altitude = alt;
            tm.m_cells[i].layer.terrain = 0;
            tm.m_cells[i].layer.next = nullptr;
        }
        fclose(f);

        tm.BuildGameMap();
        return tm;
    }

    void SetCell(int x, int y, uint16_t mask, int16_t altitude) {
        if (x >= 0 && x < m_width && y >= 0 && y < m_height) {
            CellInfo& c = m_cells[x + y * m_width];
            c.layer.mask = mask;
            c.layer.altitude = altitude;
        }
    }

    // Fill all cells as walkable at given altitude
    void FillAll(int16_t altitude, uint16_t mask = 0) {
        for (auto& c : m_cells) {
            c.layer.altitude = altitude;
            c.layer.mask = mask;
        }
        BuildGameMap();
    }

    // Set a rectangular region
    void FillRect(int x0, int y0, int x1, int y1, uint16_t mask, int16_t altitude) {
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
                SetCell(x, y, mask, altitude);
    }

    CGameMap* Get() { return &m_map; }

private:
    void BuildGameMap() {
        memset(&m_map, 0, sizeof(m_map));
        m_map.m_sizeMap.iWidth = m_width;
        m_map.m_sizeMap.iHeight = m_height;
        m_map.m_pCellInfo = m_cells.data();
        m_map.m_idMap = m_mapId;
    }

    int m_width, m_height;
    uint32_t m_mapId = 0;
    std::vector<CellInfo> m_cells;
    CGameMap m_map;
};

// =====================================================================
// Test helpers
// =====================================================================
static int g_testsPassed = 0;
static int g_testsFailed = 0;

#define TEST(name) static void test_##name()
#define RUN(name) do { \
    printf("  %-50s", #name); \
    try { test_##name(); printf("PASS\n"); g_testsPassed++; } \
    catch (...) { printf("FAIL\n"); g_testsFailed++; } \
} while(0)

#define EXPECT(expr) do { if (!(expr)) { \
    fprintf(stderr, "    FAILED: %s (line %d)\n", #expr, __LINE__); \
    throw 0; \
}} while(0)

// =====================================================================
// Unit tests — flat map (no altitude variation)
// =====================================================================

TEST(jump_same_tile_rejected) {
    TestMap tm(50, 50);
    tm.FillAll(0);
    auto* m = tm.Get();
    EXPECT(!m->CanJump(10, 10, 10, 10));
}

TEST(jump_within_range) {
    TestMap tm(50, 50);
    tm.FillAll(0);
    auto* m = tm.Get();
    EXPECT(m->CanJump(10, 10, 20, 10));    // dist 10
    EXPECT(m->CanJump(10, 10, 28, 10));    // dist 18 = MAX_JUMP_DIST
}

TEST(jump_out_of_range) {
    TestMap tm(50, 50);
    tm.FillAll(0);
    auto* m = tm.Get();
    EXPECT(!m->CanJump(0, 0, 19, 0));      // dist 19 > 18
    EXPECT(!m->CanJump(0, 0, 0, 40));      // dist 40
}

TEST(jump_blocked_destination) {
    TestMap tm(50, 50);
    tm.FillAll(0);
    tm.SetCell(20, 10, 1, 0);  // blocked
    auto* m = tm.Get();
    EXPECT(!m->CanJump(10, 10, 20, 10));
}

TEST(jump_out_of_bounds) {
    TestMap tm(50, 50);
    tm.FillAll(0);
    auto* m = tm.Get();
    EXPECT(!m->CanJump(0, 0, -1, 0));
    EXPECT(!m->CanJump(0, 0, 0, 50));
}

// =====================================================================
// Altitude tests — platforms and cliffs
// =====================================================================

TEST(altitude_same_level_ok) {
    TestMap tm(50, 50);
    tm.FillAll(100);
    auto* m = tm.Get();
    EXPECT(m->CanJump(5, 5, 10, 5));
    EXPECT(m->CanReach(5, 5, 10, 5));
}

TEST(altitude_small_diff_ok) {
    TestMap tm(50, 50);
    tm.FillAll(0);
    // Gradual slope: each tile +50
    for (int x = 0; x < 50; ++x)
        for (int y = 0; y < 50; ++y)
            tm.SetCell(x, y, 0, (int16_t)(x * 50));
    auto* m = tm.Get();
    // Adjacent tiles differ by 50 <= 200, so short hops work
    EXPECT(m->CanJump(0, 5, 3, 5));        // delta = 150
    EXPECT(m->CanReach(0, 5, 3, 5));
}

TEST(altitude_cliff_blocks_jump) {
    // Ground at alt 0, platform at alt 500
    TestMap tm(50, 50);
    tm.FillAll(0);
    tm.FillRect(20, 0, 49, 49, 0, 500);  // platform on right half
    auto* m = tm.Get();
    // Jump from ground to platform: origin alt=0, dest alt=500, diff=500 > 200
    EXPECT(!m->CanJump(10, 10, 25, 10));
}

TEST(altitude_cliff_blocks_reach) {
    TestMap tm(50, 50);
    tm.FillAll(0);
    tm.FillRect(20, 0, 49, 49, 0, 500);
    auto* m = tm.Get();
    EXPECT(!m->CanReach(10, 10, 25, 10));
}

TEST(altitude_jump_within_threshold) {
    TestMap tm(50, 50);
    tm.FillAll(0);
    tm.FillRect(20, 0, 49, 49, 0, 200);  // exactly 200
    auto* m = tm.Get();
    // Line from (10,10) to (25,10) crosses into alt=200 at x=20
    // Origin alt=0, cell at x=20 alt=200, diff=200 <= 200: OK
    EXPECT(m->CanJump(10, 10, 25, 10));
}

TEST(altitude_jump_over_threshold) {
    TestMap tm(50, 50);
    tm.FillAll(0);
    tm.FillRect(20, 0, 49, 49, 0, 201);  // just over
    auto* m = tm.Get();
    // diff=201 > 200: blocked
    EXPECT(!m->CanJump(10, 10, 25, 10));
}

// =====================================================================
// Corner-cutting tests — the key fix
// =====================================================================

TEST(corner_diagonal_with_wall) {
    // Layout (5x5):
    //   . . . . .
    //   . H . . .     H=hero at (1,1), alt 0
    //   . . W . .     W=wall at (2,2), alt 500
    //   . . . D .     D=destination at (3,3), alt 0
    //   . . . . .
    //
    // Bresenham (1,1)->(3,3) goes through (2,2) which has alt 500.
    // But cardinal neighbors (2,1) and (1,2) are at alt 0 — corner passage allowed.
    TestMap tm(10, 10);
    tm.FillAll(0);
    tm.SetCell(2, 2, 0, 500);  // wall corner with high altitude
    auto* m = tm.Get();
    EXPECT(m->CanReach(1, 1, 3, 3));
    EXPECT(m->CanJump(1, 1, 3, 3));
}

TEST(corner_both_cardinals_blocked) {
    // Same layout but both cardinal cells also high altitude → blocked
    TestMap tm(10, 10);
    tm.FillAll(0);
    tm.SetCell(2, 2, 0, 500);
    tm.SetCell(2, 1, 0, 500);  // block cardinal X
    tm.SetCell(1, 2, 0, 500);  // block cardinal Y
    auto* m = tm.Get();
    EXPECT(!m->CanReach(1, 1, 3, 3));
}

TEST(corner_one_cardinal_blocked_is_wall_edge) {
    // One cardinal also blocked — this is a wall edge, not a passable corner.
    // Both cardinals must be clear for the diagonal to be relaxed.
    TestMap tm(10, 10);
    tm.FillAll(0);
    tm.SetCell(2, 2, 0, 500);
    tm.SetCell(2, 1, 0, 500);  // block one cardinal
    // (1,2) is still at alt 0, but (2,1) is also a wall → blocked
    auto* m = tm.Get();
    EXPECT(!m->CanReach(1, 1, 3, 3));
}

TEST(corner_long_diagonal_with_wall_corner) {
    // Jump from (0,0) to (10,10) with a wall corner at (5,5)
    TestMap tm(20, 20);
    tm.FillAll(0);
    tm.SetCell(5, 5, 0, 500);
    auto* m = tm.Get();
    // Bresenham goes through (5,5), but (5,4) and (4,5) are alt 0
    EXPECT(m->CanReach(0, 0, 10, 10));
    EXPECT(m->CanJump(0, 0, 10, 10));
}

TEST(wall_edge_blocks_diagonal_clip) {
    // Reproduces the Mystic Castle case: wall along x=339 from y=572..575.
    // Jump from (338,574) to (343,564) — Bresenham clips wall at (339,572).
    // Cardinal (339,573) is also part of the wall → NOT a corner → blocked.
    TestMap tm(20, 20);
    tm.FillAll(0, 0);
    // Set all cells to alt 300
    for (int y = 0; y < 20; ++y)
        for (int x = 0; x < 20; ++x)
            tm.SetCell(x, y, 0, 300);
    // Wall column at x=10 from y=5..12 (alt 5000, mask 1)
    for (int y = 5; y <= 12; ++y)
        tm.SetCell(10, y, 1, 5000);
    auto* m = tm.Get();
    // Jump that clips the wall diagonally: (9,11) to (12,3)
    // Bresenham will hit (10,y) cells that are part of the wall
    EXPECT(!m->CanJump(9, 11, 12, 3));
    // Jump that doesn't cross the wall should still work
    EXPECT(m->CanJump(5, 5, 8, 8));
}

TEST(corner_non_diagonal_step_not_relaxed) {
    // Non-diagonal step (only X or only Y advances) — no corner relaxation
    // Place a high-altitude wall in the direct line
    TestMap tm(20, 20);
    tm.FillAll(0);
    tm.SetCell(5, 5, 0, 500);  // wall in straight horizontal path
    auto* m = tm.Get();
    // Horizontal: (0,5) to (10,5) — Bresenham hits (5,5) as a cardinal step
    EXPECT(!m->CanReach(0, 5, 10, 5));
}

// =====================================================================
// Pathfinding tests
// =====================================================================

TEST(pathfind_straight_line) {
    TestMap tm(50, 50);
    tm.FillAll(0);
    auto* m = tm.Get();
    auto path = m->FindPath(5, 5, 15, 5);
    EXPECT(!path.empty());
    EXPECT(path.front().x == 5 && path.front().y == 5);
    EXPECT(path.back().x == 15 && path.back().y == 5);
}

TEST(pathfind_around_wall) {
    TestMap tm(50, 50);
    tm.FillAll(0);
    // Wall from (10,0) to (10,8) — blocks direct path
    for (int y = 0; y <= 8; ++y)
        tm.SetCell(10, y, 1, 0);
    auto* m = tm.Get();
    auto path = m->FindPath(5, 5, 15, 5);
    EXPECT(!path.empty());
    EXPECT(path.back().x == 15 && path.back().y == 5);
    // Path must go around — should contain a tile with y > 8
    bool wentAround = false;
    for (auto& p : path)
        if (p.y > 8) wentAround = true;
    EXPECT(wentAround);
}

TEST(pathfind_around_altitude_cliff) {
    TestMap tm(50, 50);
    tm.FillAll(0);
    // Altitude wall from (10,0) to (10,8)
    for (int y = 0; y <= 8; ++y)
        tm.SetCell(10, y, 0, 500);
    auto* m = tm.Get();
    auto path = m->FindPath(5, 5, 15, 5);
    EXPECT(!path.empty());
    EXPECT(path.back().x == 15 && path.back().y == 5);
    // None of the path tiles should have alt 500
    for (auto& p : path) {
        int16_t alt = CGameMap::GetAltitude(m->GetCell(p.x, p.y));
        EXPECT(alt != 500);
    }
}

TEST(pathfind_no_path) {
    TestMap tm(50, 50);
    tm.FillAll(0);
    // Enclose destination in a box of blocked tiles (walls on all 4 sides)
    for (int i = 12; i <= 18; ++i) {
        tm.SetCell(i, 2, 1, 0);   // top wall
        tm.SetCell(i, 8, 1, 0);   // bottom wall
    }
    for (int j = 2; j <= 8; ++j) {
        tm.SetCell(12, j, 1, 0);  // left wall
        tm.SetCell(18, j, 1, 0);  // right wall
    }
    auto* m = tm.Get();
    auto path = m->FindPath(5, 5, 15, 5);
    EXPECT(path.empty());
}

// =====================================================================
// SimplifyPath tests
// =====================================================================

TEST(simplify_collapses_straight_line) {
    TestMap tm(50, 50);
    tm.FillAll(0);
    auto* m = tm.Get();
    // Tile path: (5,5) through (15,5) — 11 tiles
    std::vector<Position> tilePath;
    for (int x = 5; x <= 15; ++x)
        tilePath.push_back({x, 5});
    auto waypoints = m->SimplifyPath(tilePath);
    // Should collapse to a single jump since dist=10 < MAX_JUMP_DIST
    EXPECT(waypoints.size() == 1);
    EXPECT(waypoints[0].x == 15 && waypoints[0].y == 5);
}

TEST(simplify_respects_altitude_cliff) {
    TestMap tm(50, 50);
    tm.FillAll(0);
    // Cliff at x=10: altitude jumps to 500
    for (int y = 0; y < 50; ++y) {
        for (int x = 10; x < 50; ++x)
            tm.SetCell(x, y, 0, 500);
    }
    auto* m = tm.Get();
    // A* tile path that goes along the cliff edge (staying at alt 0)
    // then crosses somehow — but actually A* won't cross because per-step
    // alt check would fail. Let's just test that SimplifyPath won't create
    // a jump that crosses the cliff.
    std::vector<Position> tilePath;
    for (int x = 5; x <= 9; ++x)
        tilePath.push_back({x, 5});
    // Manually add a path that wraps around (pretend there's a gradual slope elsewhere)
    // For this test, just verify the jump from alt 0 to alt 500 is rejected
    tilePath.push_back({10, 5});  // alt 500
    auto waypoints = m->SimplifyPath(tilePath);
    // CanJump(5,5 -> 10,5) fails because dest alt=500 vs origin alt=0 (diff=500>200)
    // So it can't collapse to one jump — it must step through (6,5) etc.
    EXPECT(waypoints.size() > 1);
}

TEST(simplify_long_path_needs_multiple_jumps) {
    TestMap tm(100, 50);
    tm.FillAll(0);
    auto* m = tm.Get();
    // Path 40 tiles long
    std::vector<Position> tilePath;
    for (int x = 0; x <= 40; ++x)
        tilePath.push_back({x, 5});
    auto waypoints = m->SimplifyPath(tilePath);
    // MAX_JUMP_DIST=18, so 40 tiles requires at least 3 jumps
    EXPECT(waypoints.size() >= 3);
    EXPECT(waypoints.back().x == 40 && waypoints.back().y == 5);
}

// =====================================================================
// TileDist tests
// =====================================================================

TEST(tiledist_zero) {
    EXPECT(CGameMap::TileDist(5, 5, 5, 5) == 0);
}

TEST(tiledist_cardinal) {
    EXPECT(CGameMap::TileDist(0, 0, 10, 0) == 10);
    EXPECT(CGameMap::TileDist(0, 0, 0, 10) == 10);
}

TEST(tiledist_diagonal) {
    // (0,0) to (10,10): sqrt(200) ≈ 14.14
    int d = CGameMap::TileDist(0, 0, 10, 10);
    EXPECT(d == 14);
}

TEST(tiledist_max_jump) {
    // 18 tiles straight
    EXPECT(CGameMap::TileDist(0, 0, 18, 0) == 18);
    EXPECT(CGameMap::TileDist(0, 0, 19, 0) == 19);
}

// =====================================================================
// Dump file roundtrip test
// =====================================================================

TEST(dump_and_reload) {
    TestMap tm(20, 20);
    tm.FillAll(0);
    tm.SetCell(5, 5, 1, 300);
    tm.SetCell(10, 10, 0, -100);
    auto* m = tm.Get();
    m->m_idMap = 9999;

    const char* path = "test_dump.bin";
    EXPECT(m->DumpToFile(path));

    TestMap loaded = TestMap::LoadDump(path);
    auto* ml = loaded.Get();

    EXPECT(ml->m_sizeMap.iWidth == 20);
    EXPECT(ml->m_sizeMap.iHeight == 20);
    EXPECT(ml->m_idMap == 9999);
    EXPECT(CGameMap::GetMask(ml->GetCell(5, 5)) == 1);
    EXPECT(CGameMap::GetAltitude(ml->GetCell(5, 5)) == 300);
    EXPECT(CGameMap::GetAltitude(ml->GetCell(10, 10)) == -100);
    EXPECT(CGameMap::GetMask(ml->GetCell(0, 0)) == 0);
    EXPECT(CGameMap::GetAltitude(ml->GetCell(0, 0)) == 0);

    remove(path);
}

// =====================================================================
// Dump file integration tests — run only when a dump is provided
// =====================================================================

static void RunDumpTests(const char* dumpPath)
{
    printf("\n=== Dump integration tests: %s ===\n", dumpPath);
    TestMap tm = TestMap::LoadDump(dumpPath);
    auto* m = tm.Get();
    printf("  Map %u: %dx%d\n", m->m_idMap, m->m_sizeMap.iWidth, m->m_sizeMap.iHeight);

    // Collect altitude statistics
    int walkable = 0, blocked = 0;
    int16_t minAlt = 32767, maxAlt = -32768;
    for (int y = 0; y < m->m_sizeMap.iHeight; ++y) {
        for (int x = 0; x < m->m_sizeMap.iWidth; ++x) {
            const CellInfo* cell = m->GetCell(x, y);
            if (CGameMap::GetMask(cell) == 1) { blocked++; continue; }
            walkable++;
            int16_t alt = CGameMap::GetAltitude(cell);
            if (alt < minAlt) minAlt = alt;
            if (alt > maxAlt) maxAlt = alt;
        }
    }
    printf("  Walkable: %d  Blocked: %d  Alt range: [%d, %d]\n",
        walkable, blocked, minAlt, maxAlt);

    // Verify CanJump never approves altitude differences > 200
    printf("  Verifying CanJump altitude invariant...\n");
    int jumpChecked = 0, jumpBlocked = 0;
    // Sample: test jumps from a grid of walkable tiles
    for (int oy = 0; oy < m->m_sizeMap.iHeight; oy += 10) {
        for (int ox = 0; ox < m->m_sizeMap.iWidth; ox += 10) {
            if (!m->IsWalkable(ox, oy)) continue;
            int16_t oAlt = CGameMap::GetAltitude(m->GetCell(ox, oy));

            for (int ty = oy - 18; ty <= oy + 18; ty += 5) {
                for (int tx = ox - 18; tx <= ox + 18; tx += 5) {
                    if (tx < 0 || ty < 0 || tx >= m->m_sizeMap.iWidth || ty >= m->m_sizeMap.iHeight)
                        continue;
                    jumpChecked++;
                    if (!m->CanJump(ox, oy, tx, ty)) {
                        jumpBlocked++;
                        continue;
                    }
                    // If CanJump approved, verify dest altitude is within 200 of origin
                    int16_t tAlt = CGameMap::GetAltitude(m->GetCell(tx, ty));
                    if (abs(tAlt - oAlt) > 200) {
                        printf("  FAIL: CanJump(%d,%d -> %d,%d) approved alt %d -> %d (diff %d)\n",
                            ox, oy, tx, ty, oAlt, tAlt, abs(tAlt - oAlt));
                        g_testsFailed++;
                        return;
                    }
                }
            }
        }
    }
    printf("  Checked %d jumps (%d blocked). Altitude invariant OK.\n", jumpChecked, jumpBlocked);

    // Verify CanReach intermediate cells are altitude-compatible
    printf("  Verifying CanReach consistency with FindPath...\n");
    int pathTests = 0, pathOk = 0;
    // Sample a few pathfinding queries
    for (int oy = 10; oy < m->m_sizeMap.iHeight - 10; oy += 30) {
        for (int ox = 10; ox < m->m_sizeMap.iWidth - 10; ox += 30) {
            if (!m->IsWalkable(ox, oy)) continue;
            int tx = ox + 15, ty = oy + 8;
            if (tx >= m->m_sizeMap.iWidth || ty >= m->m_sizeMap.iHeight) continue;
            if (!m->IsWalkable(tx, ty)) continue;

            auto tilePath = m->FindPath(ox, oy, tx, ty, 20000);
            if (tilePath.empty()) continue;
            pathTests++;

            // Verify SimplifyPath produces only valid jumps
            auto waypoints = m->SimplifyPath(tilePath);
            Position prev = tilePath.front();
            bool allValid = true;
            for (auto& wp : waypoints) {
                if (!m->CanJump(prev.x, prev.y, wp.x, wp.y)) {
                    printf("  FAIL: SimplifyPath jump (%d,%d -> %d,%d) not valid\n",
                        prev.x, prev.y, wp.x, wp.y);
                    allValid = false;
                    break;
                }
                prev = wp;
            }
            if (allValid) pathOk++;
        }
    }
    printf("  Pathfind+Simplify: %d/%d OK\n", pathOk, pathTests);
    if (pathOk < pathTests) g_testsFailed++;
    else g_testsPassed++;
}

// =====================================================================
// Main
// =====================================================================

int main(int argc, char** argv)
{
    printf("=== CGameMap unit tests ===\n");

    // TileDist
    RUN(tiledist_zero);
    RUN(tiledist_cardinal);
    RUN(tiledist_diagonal);
    RUN(tiledist_max_jump);

    // CanJump basics
    RUN(jump_same_tile_rejected);
    RUN(jump_within_range);
    RUN(jump_out_of_range);
    RUN(jump_blocked_destination);
    RUN(jump_out_of_bounds);

    // Altitude
    RUN(altitude_same_level_ok);
    RUN(altitude_small_diff_ok);
    RUN(altitude_cliff_blocks_jump);
    RUN(altitude_cliff_blocks_reach);
    RUN(altitude_jump_within_threshold);
    RUN(altitude_jump_over_threshold);

    // Corner cutting
    RUN(corner_diagonal_with_wall);
    RUN(corner_both_cardinals_blocked);
    RUN(corner_one_cardinal_blocked_is_wall_edge);
    RUN(wall_edge_blocks_diagonal_clip);
    RUN(corner_long_diagonal_with_wall_corner);
    RUN(corner_non_diagonal_step_not_relaxed);

    // Pathfinding
    RUN(pathfind_straight_line);
    RUN(pathfind_around_wall);
    RUN(pathfind_around_altitude_cliff);
    RUN(pathfind_no_path);

    // SimplifyPath
    RUN(simplify_collapses_straight_line);
    RUN(simplify_respects_altitude_cliff);
    RUN(simplify_long_path_needs_multiple_jumps);

    // Dump roundtrip
    RUN(dump_and_reload);

    // Dump integration tests (if dump files provided as args)
    for (int i = 1; i < argc; ++i)
        RunDumpTests(argv[i]);

    printf("\n=== Results: %d passed, %d failed ===\n", g_testsPassed, g_testsFailed);
    return g_testsFailed > 0 ? 1 : 0;
}
