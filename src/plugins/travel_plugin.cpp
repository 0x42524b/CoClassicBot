#include "travel_plugin.h"
#include "pathfinder.h"
#include "game.h"
#include "CItem.h"
#include "packets.h"
#include "CEntitySet.h"
#include "config.h"
#include "hunt_settings.h"
#include "log.h"
#include "imgui.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

// =====================================================================
// TravelPlugin — cross-map travel via portals and NPCs
// =====================================================================

namespace {
constexpr DWORD kVipTeleportCooldownMs = 60000;
constexpr int kGatewayApproachSearchRadius = 5;
constexpr int kGatewayApproachArrivalDist = 2;

bool CanUseVipTeleportNow(const CHero* hero)
{
    return hero && hero->IsVip() && !IsVipTeleportOnCooldown(kVipTeleportCooldownMs);
}

bool IsTileOccupiedByRole(int tileX, int tileY)
{
    CRoleMgr* mgr = Game::GetRoleMgr();
    if (!mgr || mgr->m_deqRole.empty() || mgr->m_deqRole.size() >= 10000)
        return false;

    for (size_t i = 0; i < mgr->m_deqRole.size() && i < 500; i++) {
        auto& ref = mgr->m_deqRole[i];
        if (!ref)
            continue;

        CRole* role = ref.get();
        if (role->m_posMap.x == tileX && role->m_posMap.y == tileY)
            return true;
    }

    return false;
}

bool FindOpenTileNear(CGameMap* map, const Position& center, Position& outPos)
{
    if (!map)
        return false;

    if (map->IsWalkable(center.x, center.y) && !IsTileOccupiedByRole(center.x, center.y)) {
        outPos = center;
        return true;
    }

    for (int r = 1; r <= kGatewayApproachSearchRadius; r++) {
        for (int dx = -r; dx <= r; dx++) {
            for (int dy = -r; dy <= r; dy++) {
                if (abs(dx) != r && abs(dy) != r)
                    continue;

                const Position candidate = {center.x + dx, center.y + dy};
                if (!map->IsWalkable(candidate.x, candidate.y))
                    continue;
                if (IsTileOccupiedByRole(candidate.x, candidate.y))
                    continue;

                outPos = candidate;
                return true;
            }
        }
    }

    return false;
}

bool IsItemGatewayType(uint32_t itemTypeId)
{
    switch (itemTypeId) {
        case ItemTypeId::TWIN_CITY_GATE:
        case ItemTypeId::DESERT_CITY_GATE:
        case ItemTypeId::APE_CITY_GATE:
        case ItemTypeId::CASTLE_GATE:
        case ItemTypeId::BIRD_ISLAND_GATE:
        case ItemTypeId::STONE_CITY_GATE:
            return true;
        default:
            return false;
    }
}

CItem* FindInventoryItemByType(const CHero* hero, uint32_t itemTypeId)
{
    if (!hero || itemTypeId == 0)
        return nullptr;

    for (const auto& itemRef : hero->m_deqItem) {
        if (itemRef && itemRef->GetTypeID() == itemTypeId)
            return itemRef.get();
    }
    return nullptr;
}

std::vector<uint32_t> CollectAvailableItemGatewayTypes(const CHero* hero)
{
    std::vector<uint32_t> itemTypes;
    if (!hero)
        return itemTypes;

    for (const auto& itemRef : hero->m_deqItem) {
        if (!itemRef)
            continue;

        const uint32_t itemTypeId = itemRef->GetTypeID();
        if (!IsItemGatewayType(itemTypeId))
            continue;
        if (std::find(itemTypes.begin(), itemTypes.end(), itemTypeId) != itemTypes.end())
            continue;

        itemTypes.push_back(itemTypeId);
    }
    return itemTypes;
}
}

void TravelPlugin::SetState(TravelState state)
{
    m_state = state;
    m_stateStartTick = GetTickCount();
}

DWORD TravelPlugin::ElapsedMs() const
{
    return GetTickCount() - m_stateStartTick;
}

void TravelPlugin::StartPathAndTrack(const std::vector<Position>& waypoints)
{
    Pathfinder::Get().StartPath(
        waypoints,
        static_cast<DWORD>(GetAutoHuntSettings().movementIntervalMs));
    m_pathGeneration = Pathfinder::Get().GetGeneration();
}

void TravelPlugin::BeginFinalPathfind(CHero* hero, CGameMap* map)
{
    int hx = hero->m_posMap.x, hy = hero->m_posMap.y;
    int dist = CGameMap::TileDist(hx, hy, m_destPos.x, m_destPos.y);

    if (dist <= 3) {
        snprintf(m_statusText, sizeof(m_statusText), "Arrived!");
        SetState(TravelState::Complete);
        return;
    }

    // Find walkable tile near destination
    int tx = m_destPos.x, ty = m_destPos.y;
    if (!map->IsWalkable(tx, ty)) {
        bool found = false;
        for (int r = 1; r <= 5 && !found; r++) {
            for (int dx = -r; dx <= r && !found; dx++) {
                for (int dy = -r; dy <= r && !found; dy++) {
                    if (abs(dx) != r && abs(dy) != r) continue;
                    if (map->IsWalkable(m_destPos.x + dx, m_destPos.y + dy)) {
                        tx = m_destPos.x + dx;
                        ty = m_destPos.y + dy;
                        found = true;
                    }
                }
            }
        }
    }

    auto tilePath = map->FindPath(hx, hy, tx, ty, 1000000);
    if (tilePath.empty()) {
        snprintf(m_statusText, sizeof(m_statusText), "No path to destination (%d,%d)", tx, ty);
        SetState(TravelState::Failed);
        return;
    }

    auto waypoints = map->SimplifyPath(tilePath);
    if (waypoints.empty()) {
        snprintf(m_statusText, sizeof(m_statusText), "Path simplification failed");
        SetState(TravelState::Failed);
        return;
    }

    StartPathAndTrack(waypoints);

    snprintf(m_statusText, sizeof(m_statusText), "Walking to destination (%d,%d)...",
             m_destPos.x, m_destPos.y);
    SetState(TravelState::WaitFinalArrival);
}

CRole* TravelPlugin::FindNpcNear(const char* npcName, const Position& expectedPos, int radius)
{
    CRoleMgr* mgr = Game::GetRoleMgr();
    if (!mgr || mgr->m_deqRole.empty() || mgr->m_deqRole.size() >= 10000)
        return nullptr;

    if (npcName != nullptr) {
        CRole* best = nullptr;
        float bestDist = (float)(radius + 1);

        for (size_t i = 0; i < mgr->m_deqRole.size() && i < 500; i++) {
            auto& ref = mgr->m_deqRole[i];
            if (!ref) continue;
            CRole* e = ref.get();
            if (strcmp(e->GetName(), npcName) != 0) continue;
            float d = expectedPos.DistanceTo(e->m_posMap);
            if (d < bestDist) {
                bestDist = d;
                best = e;
            }
        }
        return best;
    }

    // Fallback: closest non-player/non-monster entity near expected pos
    CRole* best = nullptr;
    float bestDist = (float)(radius + 1);

    for (size_t i = 0; i < mgr->m_deqRole.size() && i < 500; i++) {
        auto& ref = mgr->m_deqRole[i];
        if (!ref) continue;
        CRole* e = ref.get();
        if (e->IsPlayer() || e->IsMonster()) continue;
        float d = expectedPos.DistanceTo(e->m_posMap);
        if (d < bestDist) {
            bestDist = d;
            best = e;
        }
    }
    return best;
}

Position TravelPlugin::ResolveGatewayTargetPos(const Gateway& gw)
{
    if (gw.type == GatewayType::Npc) {
        if (CRole* npc = FindNpcNear(gw.npcName, gw.pos))
            return npc->m_posMap;
    }

    return gw.pos;
}

bool TravelPlugin::TryProcessGatewayNpc(CHero* hero, const Gateway& gw)
{
    if (!hero || gw.type != GatewayType::Npc)
        return false;
    if (gw.silverCost > 0 && hero->GetSilver() < gw.silverCost) {
        snprintf(m_statusText, sizeof(m_statusText), "Need %u silver for %s",
                 gw.silverCost, gw.npcName ? gw.npcName : "gateway");
        SetState(TravelState::Failed);
        return true;
    }

    CRole* npc = FindNpcNear(gw.npcName, gw.pos);
    if (!npc)
        return false;

    Pathfinder::Get().Stop();
    m_pathGeneration = Pathfinder::Get().GetGeneration();

    const OBJID npcId = npc->GetID();
    if (hero->IsNpcActive() && hero->GetActiveNpc() == npcId) {
        m_answerIndex = 0;
        m_lastAnswerTick = GetTickCount();
        snprintf(m_statusText, sizeof(m_statusText), "Talking to NPC...");
        SetState(TravelState::AnswerDialog);
        return true;
    }

    snprintf(m_statusText, sizeof(m_statusText), "Activating NPC...");
    SetState(TravelState::ActivateNpc);
    return true;
}

static std::vector<Gateway> BuildGatewayPathForHero(const CHero* hero, OBJID from, OBJID to, Position heroPos,
    uint32_t availableSilver, bool allowVipTeleport, Position finalPos)
{
    return FindGatewayPath(from, to, heroPos, availableSilver, allowVipTeleport, finalPos,
        CollectAvailableItemGatewayTypes(hero));
}

void TravelPlugin::StartTravel(OBJID destMapId, Position destPos)
{
    CGameMap* map = Game::GetMap();
    CHero* hero = Game::GetHero();
    if (!map || !hero) {
        snprintf(m_statusText, sizeof(m_statusText), "Map not available");
        SetState(TravelState::Failed);
        return;
    }

    if (IsTraveling()
        && m_destMapId == destMapId
        && m_destPos.x == destPos.x
        && m_destPos.y == destPos.y) {
        spdlog::trace("[travel] Ignoring duplicate StartTravel request map={} pos=({},{}) state={}",
            destMapId, destPos.x, destPos.y, static_cast<int>(m_state));
        return;
    }

    m_destMapId = destMapId;
    m_destPos = destPos;
    m_gatewayApproachPos = {0, 0};
    Pathfinder::Get().Stop();
    m_pathGeneration = Pathfinder::Get().GetGeneration();

    OBJID currentMap = map->GetId();
    if (currentMap == destMapId) {
        if (HasDestPos()) {
            Position heroPos = {hero->m_posMap.x, hero->m_posMap.y};
            m_gatewayPath = BuildGatewayPathForHero(hero, currentMap, destMapId, heroPos, hero->GetSilver(), false, destPos);
            if (!m_gatewayPath.empty()) {
                m_gatewayIndex = 0;
                spdlog::info("[travel] Same map, {} intra-map gateway hop(s) toward ({},{})",
                    m_gatewayPath.size(), destPos.x, destPos.y);
                snprintf(m_statusText, sizeof(m_statusText), "Using shortcut to destination...");
                SetState(TravelState::PathfindToGateway);
            } else {
                spdlog::info("[travel] Same map, pathfinding to ({},{})", destPos.x, destPos.y);
                BeginFinalPathfind(hero, map);
            }
        } else {
            snprintf(m_statusText, sizeof(m_statusText), "Already on destination map");
            SetState(TravelState::Idle);
        }
        return;
    }

    Position heroPos = {hero->m_posMap.x, hero->m_posMap.y};
    m_gatewayPath = BuildGatewayPathForHero(hero, currentMap, destMapId, heroPos, hero->GetSilver(),
        CanUseVipTeleportNow(hero), destPos);
    if (m_gatewayPath.empty()) {
        snprintf(m_statusText, sizeof(m_statusText), "No route from %s to %s",
                 GetMapName(currentMap), GetMapName(destMapId));
        SetState(TravelState::Failed);
        return;
    }

    m_gatewayIndex = 0;

    spdlog::info("[travel] Starting: {} -> {} ({} hops)",
           GetMapName(currentMap), GetMapName(destMapId), m_gatewayPath.size());

    snprintf(m_statusText, sizeof(m_statusText), "Traveling to %s...", GetMapName(destMapId));
    SetState(TravelState::PathfindToGateway);
}

void TravelPlugin::CancelTravel()
{
    if (m_state == TravelState::Idle) return;
    spdlog::info("[travel] Cancelled");
    Pathfinder::Get().Stop();
    m_pathGeneration = Pathfinder::Get().GetGeneration();
    m_gatewayApproachPos = {0, 0};
    m_gatewayPath.clear();
    snprintf(m_statusText, sizeof(m_statusText), "Travel cancelled");
    SetState(TravelState::Idle);
}

bool TravelPlugin::IsTraveling() const
{
    return m_state != TravelState::Idle
        && m_state != TravelState::Complete
        && m_state != TravelState::Failed;
}

void TravelPlugin::Update()
{
    if (m_state == TravelState::Idle || m_state == TravelState::Complete
        || m_state == TravelState::Failed)
        return;

    CHero* hero = Game::GetHero();
    CGameMap* map = Game::GetMap();
    if (!hero || !map) {
        snprintf(m_statusText, sizeof(m_statusText), "Lost game data");
        SetState(TravelState::Failed);
        return;
    }

    // External actions such as aggro or a forced stop should not kill the whole
    // route. Re-plan from the live hero position instead.
    if ((m_state == TravelState::WaitArrival || m_state == TravelState::WaitFinalArrival)
        && Pathfinder::Get().GetGeneration() != m_pathGeneration) {
        spdlog::warn("[travel] Path interrupted externally, replanning");
        if (m_state == TravelState::WaitFinalArrival)
            BeginFinalPathfind(hero, map);
        else
            SetState(TravelState::PathfindToGateway);
        return;
    }

    // Safety: timeout step after 60 seconds
    if (ElapsedMs() > 60000) {
        snprintf(m_statusText, sizeof(m_statusText), "Step timed out");
        SetState(TravelState::Failed);
        return;
    }

    switch (m_state) {

    case TravelState::PathfindToGateway: {
        if (m_gatewayIndex >= m_gatewayPath.size()) {
            SetState(TravelState::Complete);
            snprintf(m_statusText, sizeof(m_statusText), "Arrived at %s", GetMapName(m_destMapId));
            return;
        }

        const Gateway& gw = m_gatewayPath[m_gatewayIndex];
        if (gw.IsInstant()) {
            m_gatewayApproachPos = {0, 0};
            snprintf(m_statusText, sizeof(m_statusText), gw.type == GatewayType::VipTeleport
                ? "Using VIP teleport to %s..."
                : "Using item gate to %s...", GetMapName(gw.destMapId));
            SetState(TravelState::UseGatewayAction);
            return;
        }

        if (gw.type == GatewayType::Npc && TryProcessGatewayNpc(hero, gw))
            return;

        const Position targetPos = ResolveGatewayTargetPos(gw);
        Position approachPos = targetPos;
        m_gatewayApproachPos = {0, 0};
        if (gw.type == GatewayType::Npc) {
            FindOpenTileNear(map, targetPos, approachPos);
            m_gatewayApproachPos = approachPos;
        }
        int hx = hero->m_posMap.x, hy = hero->m_posMap.y;
        if (gw.type == GatewayType::Portal) {
            const int dist = CGameMap::TileDist(hx, hy, targetPos.x, targetPos.y);
            if (dist <= 3) {
                hero->Walk(gw.pos.x, gw.pos.y);
                snprintf(m_statusText, sizeof(m_statusText), "Walking onto portal...");
                SetState(TravelState::WaitMapChange);
                return;
            }
        } else {
            const int dist = CGameMap::TileDist(hx, hy, approachPos.x, approachPos.y);
            if (dist <= kGatewayApproachArrivalDist) {
                snprintf(m_statusText, sizeof(m_statusText), "Activating NPC...");
                SetState(TravelState::ActivateNpc);
                return;
            }
        }

        snprintf(m_statusText, sizeof(m_statusText), "Pathfinding to %s (%d,%d)...",
                 gw.type == GatewayType::Npc ? "NPC" : "portal", targetPos.x, targetPos.y);

        // For NPC gateways, path to any open adjacent tile instead of a single
        // fixed spot that may be blocked by another entity.
        int tx = targetPos.x, ty = targetPos.y;
        if (gw.type == GatewayType::Npc) {
            tx = approachPos.x;
            ty = approachPos.y;
        } else if (!map->IsWalkable(tx, ty)) {
            bool found = false;
            for (int r = 1; r <= 5 && !found; r++) {
                for (int dx = -r; dx <= r && !found; dx++) {
                    for (int dy = -r; dy <= r && !found; dy++) {
                        if (abs(dx) != r && abs(dy) != r) continue;
                        if (map->IsWalkable(targetPos.x + dx, targetPos.y + dy)) {
                            tx = targetPos.x + dx;
                            ty = targetPos.y + dy;
                            found = true;
                        }
                    }
                }
            }
        }

        if (tx != targetPos.x || ty != targetPos.y) {
            spdlog::debug("[travel] Gateway approach ({},{}) -> ({},{}) walkable={} occupied={}",
                   targetPos.x, targetPos.y, tx, ty, map->IsWalkable(tx, ty), IsTileOccupiedByRole(tx, ty));
        } else if (!map->IsWalkable(tx, ty)) {
            spdlog::warn("[travel] Target tile ({},{}) unavailable, using fallback ({},{}) walkable={}",
                   targetPos.x, targetPos.y, tx, ty, map->IsWalkable(tx, ty));
        }

        spdlog::debug("[travel] FindPath hero=({},{}) -> target=({},{}) mapSize=({},{})",
               hx, hy, tx, ty, map->m_sizeMap.iWidth, map->m_sizeMap.iHeight);

        auto tilePath = map->FindPath(hx, hy, tx, ty, 1000000);
        if (tilePath.empty()) {
            spdlog::error("[travel] FindPath returned empty! walkable(hero)={} walkable(target)={}",
                   map->IsWalkable(hx, hy), map->IsWalkable(tx, ty));
            snprintf(m_statusText, sizeof(m_statusText), "No path to gateway (%d,%d)->(%d,%d)",
                     hx, hy, tx, ty);
            SetState(TravelState::Failed);
            return;
        }

        auto waypoints = map->SimplifyPath(tilePath);
        spdlog::debug("[travel] Path found: {} tiles -> {} waypoints", tilePath.size(), waypoints.size());
        if (waypoints.empty()) {
            snprintf(m_statusText, sizeof(m_statusText), "Path simplification failed");
            SetState(TravelState::Failed);
            return;
        }

        StartPathAndTrack(waypoints);
        if (gw.type == GatewayType::Portal)
            Pathfinder::Get().SetForceNativeJump(true);
        SetState(TravelState::WaitArrival);
        break;
    }

    case TravelState::UseGatewayAction: {
        const Gateway& gw = m_gatewayPath[m_gatewayIndex];
        if (gw.type == GatewayType::VipTeleport) {
            if (!CanUseVipTeleportNow(hero)) {
                Position replanPos = {hero->m_posMap.x, hero->m_posMap.y};
                m_gatewayPath = BuildGatewayPathForHero(hero, map->GetId(), m_destMapId, replanPos, hero->GetSilver(), false, m_destPos);
                if (m_gatewayPath.empty()) {
                    snprintf(m_statusText, sizeof(m_statusText), "VIP teleport unavailable and no alternate route");
                    SetState(TravelState::Failed);
                } else {
                    snprintf(m_statusText, sizeof(m_statusText), "VIP teleport unavailable, replanning...");
                    m_gatewayIndex = 0;
                    SetState(TravelState::PathfindToGateway);
                }
                return;
            }
            if (!hero->VipTeleport(gw.destMapId)) {
                snprintf(m_statusText, sizeof(m_statusText), "Unsupported VIP destination");
                SetState(TravelState::Failed);
                return;
            }
            snprintf(m_statusText, sizeof(m_statusText), "Using VIP teleport to %s...", GetMapName(gw.destMapId));
        } else if (gw.type == GatewayType::Item) {
            CItem* gateItem = FindInventoryItemByType(hero, gw.itemTypeId);
            if (!gateItem) {
                Position replanPos = {hero->m_posMap.x, hero->m_posMap.y};
                m_gatewayPath = BuildGatewayPathForHero(hero, map->GetId(), m_destMapId, replanPos, hero->GetSilver(),
                    CanUseVipTeleportNow(hero), m_destPos);
                if (m_gatewayPath.empty()) {
                    snprintf(m_statusText, sizeof(m_statusText), "Item gate unavailable and no alternate route");
                    SetState(TravelState::Failed);
                } else {
                    snprintf(m_statusText, sizeof(m_statusText), "Item gate unavailable, replanning...");
                    m_gatewayIndex = 0;
                    SetState(TravelState::PathfindToGateway);
                }
                return;
            }

            hero->UseItem(gateItem->GetID());
            snprintf(m_statusText, sizeof(m_statusText), "Using item gate to %s...", GetMapName(gw.destMapId));
        } else {
            snprintf(m_statusText, sizeof(m_statusText), "Unsupported instant gateway");
            SetState(TravelState::Failed);
            return;
        }
        SetState(TravelState::WaitMapChange);
        return;
    }

    case TravelState::WaitArrival: {
        const Gateway& gw = m_gatewayPath[m_gatewayIndex];
        if (gw.type == GatewayType::Npc && TryProcessGatewayNpc(hero, gw))
            return;

        const Position targetPos = ResolveGatewayTargetPos(gw);
        const bool hasApproachPos = m_gatewayApproachPos.x != 0 || m_gatewayApproachPos.y != 0;
        const Position arrivalPos = (gw.type == GatewayType::Npc && hasApproachPos)
            ? m_gatewayApproachPos
            : targetPos;
        int hx = hero->m_posMap.x, hy = hero->m_posMap.y;
        const int dist = CGameMap::TileDist(hx, hy, arrivalPos.x, arrivalPos.y);

        // Reset timeout while hero is still moving (long walks across big maps)
        if (hx != m_lastHeroPos.x || hy != m_lastHeroPos.y) {
            m_lastHeroPos = {hx, hy};
            m_stateStartTick = GetTickCount();
        }

        snprintf(m_statusText, sizeof(m_statusText), "Walking to gateway... (dist=%d)", dist);

        if (gw.type == GatewayType::Portal) {
            if (dist <= 3) {
                Pathfinder::Get().Stop();
                m_pathGeneration = Pathfinder::Get().GetGeneration();
                hero->Walk(gw.pos.x, gw.pos.y);
                snprintf(m_statusText, sizeof(m_statusText), "Walking onto portal...");
                SetState(TravelState::WaitMapChange);
                return;
            }
        } else if (dist <= kGatewayApproachArrivalDist) {
            Pathfinder::Get().Stop();
            m_pathGeneration = Pathfinder::Get().GetGeneration();
            snprintf(m_statusText, sizeof(m_statusText), "Activating NPC...");
            SetState(TravelState::ActivateNpc);
            return;
        }

        // Pathfinding stalled — restart
        if (!Pathfinder::Get().IsActive()) {
            if (gw.type == GatewayType::Npc) {
                snprintf(m_statusText, sizeof(m_statusText), "Activating NPC...");
                SetState(TravelState::ActivateNpc);
            } else {
                SetState(TravelState::PathfindToGateway);
            }
        }
        break;
    }

    case TravelState::ActivateNpc: {
        const Gateway& gw = m_gatewayPath[m_gatewayIndex];

        CRole* npc = FindNpcNear(gw.npcName, gw.pos);
        if (!npc) {
            if (ElapsedMs() > 5000) {
                snprintf(m_statusText, sizeof(m_statusText), "NPC not found near (%d,%d)",
                         gw.pos.x, gw.pos.y);
                SetState(TravelState::Failed);
            }
            return;
        }

        if (hero->IsNpcActive() && hero->GetActiveNpc() == npc->GetID()) {
            m_answerIndex = 0;
            m_lastAnswerTick = GetTickCount();
            snprintf(m_statusText, sizeof(m_statusText), "Talking to NPC...");
            SetState(TravelState::AnswerDialog);
            return;
        }

        spdlog::info("[travel] Activating NPC entity {} at ({},{})",
               npc->GetID(), npc->m_posMap.x, npc->m_posMap.y);
        hero->ActivateNpc(npc->GetID());

        m_answerIndex = 0;
        m_lastAnswerTick = GetTickCount();
        snprintf(m_statusText, sizeof(m_statusText), "Talking to NPC...");
        SetState(TravelState::AnswerDialog);
        break;
    }

    case TravelState::AnswerDialog: {
        const Gateway& gw = m_gatewayPath[m_gatewayIndex];

        if (GetTickCount() - m_lastAnswerTick < 1000)
            return;

        if (gw.optionCount == 0) {
            spdlog::debug("[travel] Sending dialog confirm (taskId={})", gw.npcTaskId);
            hero->AnswerNpcEx(0, gw.npcTaskId);
            snprintf(m_statusText, sizeof(m_statusText), "Waiting for map change...");
            SetState(TravelState::WaitMapChange);
        } else if (m_answerIndex < gw.optionCount) {
            spdlog::debug("[travel] Answering NPC option {}: {} (taskId={})",
                   m_answerIndex, gw.options[m_answerIndex], gw.npcTaskId);
            hero->AnswerNpcEx(gw.options[m_answerIndex], gw.npcTaskId);
            m_answerIndex++;
            m_lastAnswerTick = GetTickCount();
            snprintf(m_statusText, sizeof(m_statusText), "Selecting dialog option %d/%d...",
                     m_answerIndex, gw.optionCount);
        } else {
            snprintf(m_statusText, sizeof(m_statusText), "Waiting for map change...");
            SetState(TravelState::WaitMapChange);
        }
        break;
    }

    case TravelState::WaitMapChange: {
        const Gateway& gw = m_gatewayPath[m_gatewayIndex];
        OBJID currentMap = map->GetId();

        if (gw.IsIntraMap()) {
            if (gw.HasDestPos()) {
                int hx = hero->m_posMap.x, hy = hero->m_posMap.y;
                int dist = CGameMap::TileDist(hx, hy, gw.destPos.x, gw.destPos.y);
                if (dist <= 10) {
                    spdlog::info("[travel] Intra-map warp arrived at ({},{}), dist={}",
                           hx, hy, dist);
                    m_gatewayApproachPos = {0, 0};
                    m_gatewayIndex++;
                    if (m_gatewayIndex >= m_gatewayPath.size()) {
                        snprintf(m_statusText, sizeof(m_statusText), "Arrived!");
                        SetState(TravelState::Complete);
                    } else {
                        SetState(TravelState::PathfindToGateway);
                    }
                    return;
                }
            }
            if (ElapsedMs() > 10000) {
                snprintf(m_statusText, sizeof(m_statusText), "Intra-map warp timed out");
                SetState(TravelState::Failed);
            }
        } else if (currentMap != gw.mapId) {
            spdlog::info("[travel] Map changed: {} -> {} (expected {})",
                   gw.mapId, currentMap, gw.destMapId);
            snprintf(m_statusText, sizeof(m_statusText), "Map loaded, stabilizing...");
            SetState(TravelState::WaitMapStabilize);
            return;
        } else if (ElapsedMs() > 15000) {
            spdlog::warn("[travel] Map change timed out: npc={} taskId={} optionCount={} from={} to={}",
                gw.npcName ? gw.npcName : "<none>", gw.npcTaskId, gw.optionCount, gw.mapId, gw.destMapId);
            snprintf(m_statusText, sizeof(m_statusText), "Map change timed out");
            SetState(TravelState::Failed);
        }
        break;
    }

    case TravelState::WaitMapStabilize: {
        if (ElapsedMs() < 2000) return;

        m_gatewayIndex++;
        m_gatewayApproachPos = {0, 0};
        if (m_gatewayIndex >= m_gatewayPath.size()) {
            OBJID currentMap = map->GetId();
            if (currentMap == m_destMapId) {
                spdlog::info("[travel] Arrived at {}", GetMapName(m_destMapId));
                if (HasDestPos()) {
                    BeginFinalPathfind(hero, map);
                } else {
                    snprintf(m_statusText, sizeof(m_statusText), "Arrived at %s!",
                             GetMapName(m_destMapId));
                    SetState(TravelState::Complete);
                }
            } else {
                spdlog::info("[travel] Re-planning from {}", GetMapName(currentMap));
                Position replanPos = {hero->m_posMap.x, hero->m_posMap.y};
                m_gatewayPath = BuildGatewayPathForHero(hero, currentMap, m_destMapId, replanPos, hero->GetSilver(),
                    CanUseVipTeleportNow(hero), m_destPos);
                if (m_gatewayPath.empty()) {
                    snprintf(m_statusText, sizeof(m_statusText), "No route from %s",
                             GetMapName(currentMap));
                    SetState(TravelState::Failed);
                } else {
                    m_gatewayIndex = 0;
                    SetState(TravelState::PathfindToGateway);
                }
            }
        } else {
            spdlog::info("[travel] Hop {}/{} complete, next gateway",
                   m_gatewayIndex, m_gatewayPath.size());
            SetState(TravelState::PathfindToGateway);
        }
        break;
    }

    case TravelState::WaitFinalArrival: {
        int hx = hero->m_posMap.x, hy = hero->m_posMap.y;
        int dist = CGameMap::TileDist(hx, hy, m_destPos.x, m_destPos.y);

        // Reset timeout while hero is still moving
        if (hx != m_lastHeroPos.x || hy != m_lastHeroPos.y) {
            m_lastHeroPos = {hx, hy};
            m_stateStartTick = GetTickCount();
        }

        snprintf(m_statusText, sizeof(m_statusText), "Walking to destination... (dist=%d)", dist);

        if (dist <= 3) {
            Pathfinder::Get().Stop();
            m_pathGeneration = Pathfinder::Get().GetGeneration();
            snprintf(m_statusText, sizeof(m_statusText), "Arrived!");
            SetState(TravelState::Complete);
            return;
        }

        // Restart pathfinding if it stalled
        if (!Pathfinder::Get().IsActive() && dist > 3) {
            BeginFinalPathfind(hero, map);
        }
        break;
    }

    default:
        break;
    }
}

void TravelPlugin::RenderUI()
{
    CHero* hero = Game::GetHero();
    CGameMap* map = Game::GetMap();
    if (!hero || !map) {
        ImGui::Text("Waiting for game data...");
        return;
    }

    OBJID curMapId = map->GetId();
    int heroTileX = hero->m_posMap.x;
    int heroTileY = hero->m_posMap.y;
    static int selectedDestIndex = 0;

    if (IsTraveling()) {
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.4f, 1.0f), "%s", m_statusText);
        ImGui::SameLine();
        if (ImGui::SmallButton("Cancel##travel")) {
            CancelTravel();
        }
    } else {
        if (m_state == TravelState::Complete) {
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%s", m_statusText);
        } else if (m_state == TravelState::Failed) {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", m_statusText);
        } else {
            ImGui::TextDisabled("Idle");
        }
    }
    ImGui::Text("Current Map: %s (%u)", GetMapName(curMapId), curMapId);
    ImGui::Text("Hero Pos: (%d, %d)", heroTileX, heroTileY);

    {
        TravelSettings& travel = GetTravelSettings();
        ImGui::Checkbox("Speedhack If No Players Nearby##travel", &travel.usePacketJump);
        ImGui::TextDisabled("Send raw jump packets for faster travel. Falls back to normal jumps when players are nearby.");

        auto& pf = Pathfinder::Get();
        bool avoidMobs = pf.GetAvoidMobs();
        if (ImGui::Checkbox("Avoid Mobs While Traveling", &avoidMobs))
            pf.SetAvoidMobs(avoidMobs);
        if (avoidMobs) {
            ImGui::SameLine();
            int radius = pf.GetAvoidMobRadius();
            ImGui::SetNextItemWidth(120.0f);
            if (ImGui::SliderInt("Radius##avoidmob", &radius, 1, 10))
                pf.SetAvoidMobRadius(radius);
        }
    }

    size_t destCount = 0;
    const Destination* dests = GetDestinations(destCount);
    if (!dests || destCount == 0) {
        ImGui::TextDisabled("No destinations available.");
        return;
    }

    if (selectedDestIndex < 0 || selectedDestIndex >= static_cast<int>(destCount))
        selectedDestIndex = 0;

    ImGui::SetNextItemWidth(260.0f);
    if (ImGui::BeginCombo("Destination", dests[selectedDestIndex].name)) {
        for (size_t di = 0; di < destCount; ++di) {
            bool isSelected = selectedDestIndex == static_cast<int>(di);
            if (ImGui::Selectable(dests[di].name, isSelected))
                selectedDestIndex = static_cast<int>(di);

            if (isSelected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    const Destination& selected = dests[selectedDestIndex];
    bool isCurrent = (selected.mapId == curMapId);
    bool isNear = false;
    if (isCurrent && selected.pos.x != 0) {
        int dist = CGameMap::TileDist(heroTileX, heroTileY, selected.pos.x, selected.pos.y);
        isNear = (dist <= 3);
    }

    if (isNear)
        ImGui::BeginDisabled();

    if (ImGui::Button("Travel##destination")) {
        StartTravel(selected.mapId, selected.pos);
    }

    if (isNear)
        ImGui::EndDisabled();

    ImGui::SameLine();
    if (selected.pos.x != 0 || selected.pos.y != 0) {
        ImGui::TextDisabled("Target: (%d, %d)", selected.pos.x, selected.pos.y);
    } else {
        ImGui::TextDisabled("Target: map entry");
    }

    if (isNear) {
        ImGui::TextDisabled("Already near %s.", selected.name);
    } else if (isCurrent) {
        ImGui::TextDisabled("Already on %s.", selected.name);
    } else {
        ImGui::TextDisabled("Selected: %s", selected.name);
    }
}
