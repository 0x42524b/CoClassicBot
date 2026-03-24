#pragma once
#include "base.h"
#include <vector>

// Check if any entity occupies the given tile
bool IsTileOccupied(int tileX, int tileY);

class CHero;
class CGameMap;

class Pathfinder {
public:
    void StartPath(const std::vector<Position>& waypoints, DWORD movementIntervalMs = 0);
    void Stop();
    void Update();
    bool IsActive() const { return m_active; }
    const std::vector<Position>& GetWaypoints() const { return m_waypoints; }
    size_t GetCurrentIndex() const { return m_index; }
    uint32_t GetGeneration() const { return m_generation; }
    bool GetAvoidMobs() const { return m_avoidMobs; }
    void SetAvoidMobs(bool avoid) { m_avoidMobs = avoid; }
    int GetAvoidMobRadius() const { return m_avoidMobRadius; }
    void SetAvoidMobRadius(int radius) { m_avoidMobRadius = radius; }
    bool GetForceNativeJump() const { return m_forceNativeJump; }
    void SetForceNativeJump(bool force) { m_forceNativeJump = force; }
    DWORD GetLastJumpTick() const { return m_lastJumpTick; }
    static Pathfinder& Get();

private:
    bool IssueMovementToWaypoint(CHero* hero, CGameMap* map, const Position& target);
    bool RepathFrom(CHero* hero, CGameMap* map, const Position& finalDest, bool issueImmediate);
    bool CanIssueMovementCommand(DWORD now) const;
    void LoadTilePath(const std::vector<Position>& tilePath, int hx, int hy);

    Position FindSafeAlternative(CHero* hero, CGameMap* map, const Position& target);

    std::vector<Position> m_waypoints;
    size_t m_index = 0;
    bool m_active = false;
    bool m_avoidMobs = false;
    int m_avoidMobRadius = 5;
    bool m_forceNativeJump = false;
    DWORD m_lastJumpTick = 0;
    Position m_lastProgressPos = {};
    Position m_lastIssuedTarget = {};
    Position m_finalDestination = {};
    bool m_lastIssuedMoveWasImmediate = false;
    DWORD m_movementIntervalMs = 0;
    DWORD m_lastProgressTick = 0;
    uint32_t m_generation = 0;
};
