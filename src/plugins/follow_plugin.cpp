#include "follow_plugin.h"
#include "game.h"
#include "hunt_settings.h"
#include "log.h"
#include "imgui.h"

static FollowSettings g_followSettings;
FollowSettings& GetFollowSettings() { return g_followSettings; }

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

CRole* FollowPlugin::FindTarget() const
{
    const FollowSettings& s = g_followSettings;
    if (!s.targetName[0])
        return nullptr;

    CRoleMgr* mgr = Game::GetRoleMgr();
    if (!mgr || mgr->m_deqRole.empty() || mgr->m_deqRole.size() >= 10000)
        return nullptr;

    for (size_t i = 0; i < mgr->m_deqRole.size() && i < 500; ++i) {
        const auto& ref = mgr->m_deqRole[i];
        if (!ref) continue;
        CRole* role = ref.get();
        if (!role->IsPlayer()) continue;
        if (_stricmp(role->GetName(), s.targetName) == 0)
            return role;
    }
    return nullptr;
}

int FollowPlugin::NearestMobDistance() const
{
    CHero* hero = Game::GetHero();
    if (!hero) return 999;

    CRoleMgr* mgr = Game::GetRoleMgr();
    if (!mgr || mgr->m_deqRole.empty() || mgr->m_deqRole.size() >= 10000)
        return 999;

    int best = 999;
    for (size_t i = 0; i < mgr->m_deqRole.size() && i < 500; ++i) {
        const auto& ref = mgr->m_deqRole[i];
        if (!ref) continue;
        CRole* e = ref.get();
        if (!e->IsMonster() || e->IsDead()) continue;
        int d = CGameMap::TileDist(hero->m_posMap.x, hero->m_posMap.y,
                                   e->m_posMap.x, e->m_posMap.y);
        if (d < best) best = d;
    }
    return best;
}

// ---------------------------------------------------------------------------
// Mob positions collector (shared by dodge and follow)
// ---------------------------------------------------------------------------

struct MobPos { int x, y; };

static int CollectMobs(MobPos* out, int maxCount)
{
    CRoleMgr* mgr = Game::GetRoleMgr();
    if (!mgr) return 0;
    int count = 0;
    for (size_t i = 0; i < mgr->m_deqRole.size() && i < 500; ++i) {
        const auto& ref = mgr->m_deqRole[i];
        if (!ref) continue;
        CRole* e = ref.get();
        if (!e->IsMonster() || e->IsDead()) continue;
        if (count < maxCount)
            out[count++] = { e->m_posMap.x, e->m_posMap.y };
    }
    return count;
}

static int MinMobDist(int tx, int ty, const MobPos* mobs, int mobCount)
{
    int best = 999;
    for (int i = 0; i < mobCount; ++i) {
        int d = CGameMap::TileDist(tx, ty, mobs[i].x, mobs[i].y);
        if (d < best) best = d;
    }
    return best;
}

// ---------------------------------------------------------------------------
// Pick the best jumpable tile — scored by mob avoidance + target proximity
// ---------------------------------------------------------------------------

static Position FindBestJumpTile(CHero* hero, CGameMap* map,
    const MobPos* mobs, int mobCount,
    const Position* targetPos, int followDistance)
{
    const int hx = hero->m_posMap.x;
    const int hy = hero->m_posMap.y;
    const int altThreshold = CGameMap::GetHeroAltThreshold();

    Position best = { hx, hy };
    int bestMinMob = -1;
    int bestTDist = 999;
    int bestHDist = 999;

    for (int dx = -CGameMap::MAX_JUMP_DIST; dx <= CGameMap::MAX_JUMP_DIST; ++dx) {
        for (int dy = -CGameMap::MAX_JUMP_DIST; dy <= CGameMap::MAX_JUMP_DIST; ++dy) {
            const int cx = hx + dx;
            const int cy = hy + dy;
            const int heroDist = CGameMap::TileDist(hx, hy, cx, cy);
            if (heroDist == 0 || heroDist > CGameMap::MAX_JUMP_DIST)
                continue;
            if (!map->IsWalkable(cx, cy))
                continue;
            if (heroDist <= 2) {
                if (!map->CanReach(hx, hy, cx, cy))
                    continue;
            } else {
                if (!map->CanJump(hx, hy, cx, cy, altThreshold))
                    continue;
            }

            // Reject tiles outside follow range of target
            int tDist = targetPos
                ? CGameMap::TileDist(cx, cy, targetPos->x, targetPos->y) : 0;
            if (targetPos && tDist > followDistance)
                continue;

            int minMob = mobCount > 0 ? MinMobDist(cx, cy, mobs, mobCount) : 999;

            // Pick: farthest from mobs, then closest to target, then shortest jump
            if (minMob > bestMinMob
                || (minMob == bestMinMob && tDist < bestTDist)
                || (minMob == bestMinMob && tDist == bestTDist && heroDist < bestHDist)) {
                best = Position(cx, cy);
                bestMinMob = minMob;
                bestTDist = tDist;
                bestHDist = heroDist;
            }
        }
    }
    return best;
}

// ---------------------------------------------------------------------------
// Update — called each frame
// ---------------------------------------------------------------------------

void FollowPlugin::Update()
{
    const FollowSettings& s = g_followSettings;
    if (!s.enabled) {
        m_state = State::Idle;
        return;
    }

    CHero* hero = Game::GetHero();
    CGameMap* map = Game::GetMap();
    if (!hero || !map) return;
    if (hero->IsDead()) return;
    if (hero->IsJumping()) return;

    // Throttle movement commands
    const AutoHuntSettings& ah = GetAutoHuntSettings();
    const DWORD now = GetTickCount();
    const DWORD interval = ah.movementIntervalMs > 0 ? ah.movementIntervalMs : 500;
    if (m_lastJumpTick != 0 && now - m_lastJumpTick < interval)
        return;

    CRole* target = FindTarget();
    const int hx = hero->m_posMap.x;
    const int hy = hero->m_posMap.y;

    // Determine distance to target
    int targetDist = 999;
    Position targetPos = {0, 0};
    if (target) {
        targetPos = target->m_posMap;
        targetDist = CGameMap::TileDist(hx, hy, targetPos.x, targetPos.y);
    }

    // Collect nearby mobs
    MobPos mobs[128];
    int mobCount = CollectMobs(mobs, 128);
    int mobDist = mobCount > 0 ? MinMobDist(hx, hy, mobs, mobCount) : 999;

    // Decide what to do: dodge or follow
    const bool needDodge = mobDist <= s.dodgeRadius;
    const bool needFollow = target && targetDist > s.followDistance;

    if (!needDodge && !needFollow) {
        m_state = State::Idle;
        return;
    }

    // Find best tile considering both mob avoidance and target proximity
    Position dest = FindBestJumpTile(hero, map, mobs, mobCount,
        target ? &targetPos : nullptr, s.followDistance);

    if (dest.x == hx && dest.y == hy)
        return;

    m_state = needDodge ? State::Dodging : State::Following;
    hero->Jump(dest.x, dest.y);
    m_lastJumpTick = now;
    spdlog::debug("[follow] {} to ({},{}) mobDist={} targetDist={}",
        needDodge ? "Dodge" : "Follow", dest.x, dest.y, mobDist, targetDist);
}

// ---------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------

void FollowPlugin::RenderUI()
{
    FollowSettings& s = g_followSettings;
    ImGui::Checkbox("Enabled##follow", &s.enabled);

    ImGui::InputText("Target Player", s.targetName, IM_ARRAYSIZE(s.targetName));

    ImGui::SliderInt("Follow Distance", &s.followDistance, 1, 30);
    ImGui::TextDisabled("Stop moving when within this tile distance of the target.");

    ImGui::SliderInt("Dodge Radius", &s.dodgeRadius, 1, 15);
    ImGui::TextDisabled("Emergency dodge when a mob is within this tile distance.");

    ImGui::Separator();

    // Status display
    const char* stateStr = "Idle";
    if (m_state == State::Following) stateStr = "Following";
    else if (m_state == State::Dodging) stateStr = "Dodging";

    CRole* target = FindTarget();
    if (s.enabled && target) {
        CHero* hero = Game::GetHero();
        int dist = hero ? CGameMap::TileDist(hero->m_posMap.x, hero->m_posMap.y,
                                              target->m_posMap.x, target->m_posMap.y) : 0;
        ImGui::Text("Status: %s | Target: %s (%d,%d) | Distance: %d",
            stateStr, target->GetName(), target->m_posMap.x, target->m_posMap.y, dist);
    } else if (s.enabled && s.targetName[0]) {
        ImGui::Text("Status: %s | Target not found: %s", stateStr, s.targetName);
    } else {
        ImGui::Text("Status: %s", stateStr);
    }
}
