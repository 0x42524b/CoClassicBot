#include "base_hunt_plugin.h"
#include "hunt_buffs.h"
#include "hunt_loot.h"
#include "hunt_targeting.h"
#include "hunt_town.h"
#include "inventory_utils.h"
#include "npc_utils.h"
#include "revive_utils.h"
#include "plugin_mgr.h"
#include "travel_plugin.h"
#include "game.h"
#include "hooks.h"
#include "gateway.h"
#include "CHero.h"
#include "CGameMap.h"
#include "CRole.h"
#include "config.h"
#include "discord.h"
#include "itemtype.h"
#include "pathfinder.h"
#include "log.h"
#include "imgui.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_set>

// ── Anonymous namespace: shared helpers ──────────────────────────────────────
namespace {

const Position kMarketLandingPos = {211, 196};

constexpr int kReliableAttackRange = 1;
constexpr int kMinMobClumpSize = 2;
constexpr int kMinLootRange = 0;
constexpr int kMaxLootRange = CGameMap::MAX_JUMP_DIST;
constexpr int kLootPathStopRange = 0;
constexpr int kMinMovementIntervalMs = 100;
constexpr int kMaxMovementIntervalMs = 5000;
constexpr int kMinAttackIntervalMs = 25;
constexpr int kMaxAttackIntervalMs = 5000;
constexpr int kMinTargetSwitchAttackIntervalMs = 0;
constexpr int kMaxTargetSwitchAttackIntervalMs = 5000;
constexpr int kMinItemActionIntervalMs = 100;
constexpr int kMaxItemActionIntervalMs = 5000;
constexpr int kMinLootSpawnGraceMs = 0;
constexpr int kMaxLootSpawnGraceMs = 5000;
constexpr int kMinSelfCastIntervalMs = 100;
constexpr int kMaxSelfCastIntervalMs = 5000;
constexpr int kMinNpcActionIntervalMs = 100;
constexpr int kMaxNpcActionIntervalMs = 2000;
constexpr int kMinLootPickupIgnoreMs = 0;
constexpr int kMaxLootPickupIgnoreMs = 300000;
constexpr int kMinReviveDelayMs = 0;
constexpr int kMaxReviveDelayMs = 60000;
constexpr int kMinReviveRetryIntervalMs = 100;
constexpr int kMaxReviveRetryIntervalMs = 10000;
constexpr DWORD kPendingJumpStallMs = 500;
constexpr DWORD kPendingJumpHardTimeoutMs = 6000;

int GetLootRange(const AutoHuntSettings& settings)
{
    return std::clamp(settings.lootRange, kMinLootRange, kMaxLootRange);
}

bool IsWithinLootPickupRange(const AutoHuntSettings& settings, int distance)
{
    const int lootRange = GetLootRange(settings);
    return lootRange > 0 ? distance <= lootRange : distance == 0;
}

DWORD ClampMs(int value, int minValue, int maxValue)
{
    return static_cast<DWORD>(std::clamp(value, minValue, maxValue));
}

DWORD GetMovementIntervalMs(const AutoHuntSettings& settings)
{
    return ClampMs(settings.movementIntervalMs, kMinMovementIntervalMs, kMaxMovementIntervalMs);
}

DWORD GetJumpMovementIntervalMs(const AutoHuntSettings& settings, const CHero* /*hero*/)
{
    return GetMovementIntervalMs(settings);
}

DWORD GetItemActionIntervalMs(const AutoHuntSettings& settings)
{
    return ClampMs(settings.itemActionIntervalMs, kMinItemActionIntervalMs, kMaxItemActionIntervalMs);
}

DWORD GetLootSpawnGraceMs(const AutoHuntSettings& settings)
{
    return ClampMs(settings.lootSpawnGraceMs, kMinLootSpawnGraceMs, kMaxLootSpawnGraceMs);
}

DWORD GetReviveDelayMs(const AutoHuntSettings& settings)
{
    return ClampMs(settings.reviveDelayMs, kMinReviveDelayMs, kMaxReviveDelayMs);
}

DWORD GetReviveRetryIntervalMs(const AutoHuntSettings& settings)
{
    return ClampMs(settings.reviveRetryIntervalMs, kMinReviveRetryIntervalMs, kMaxReviveRetryIntervalMs);
}

bool TickIsFuture(DWORD targetTick, DWORD now)
{
    return static_cast<int32_t>(targetTick - now) > 0;
}

std::string ToLowerCopy(const std::string& value)
{
    std::string lower = value;
    std::transform(lower.begin(), lower.end(), lower.begin(),
        [](unsigned char c) { return (char)std::tolower(c); });
    return lower;
}

std::vector<std::string> ParseTokens(const char* text)
{
    std::vector<std::string> tokens;
    if (!text || !text[0])
        return tokens;

    std::string current;
    while (*text) {
        const char ch = *text++;
        if (ch == ',' || ch == ';' || ch == '\n' || ch == '\r' || ch == '\t') {
            if (!current.empty()) {
                size_t start = 0;
                while (start < current.size() && std::isspace((unsigned char)current[start]))
                    start++;
                size_t end = current.size();
                while (end > start && std::isspace((unsigned char)current[end - 1]))
                    end--;
                if (end > start)
                    tokens.push_back(ToLowerCopy(current.substr(start, end - start)));
                current.clear();
            }
            continue;
        }
        current.push_back(ch);
    }

    if (!current.empty()) {
        size_t start = 0;
        while (start < current.size() && std::isspace((unsigned char)current[start]))
            start++;
        size_t end = current.size();
        while (end > start && std::isspace((unsigned char)current[end - 1]))
            end--;
        if (end > start)
            tokens.push_back(ToLowerCopy(current.substr(start, end - start)));
    }

    return tokens;
}

void AppendFilterToken(char* buffer, size_t bufferSize, const char* token)
{
    if (!buffer || bufferSize == 0 || !token || !token[0])
        return;

    if (buffer[0] != '\0')
        strncat_s(buffer, bufferSize, ", ", _TRUNCATE);
    strncat_s(buffer, bufferSize, token, _TRUNCATE);
}

bool NameMatchesFilters(const char* name, const std::vector<std::string>& filters)
{
    if (filters.empty())
        return true;
    if (!name || !name[0])
        return false;

    const std::string lowered = ToLowerCopy(name);
    for (const std::string& filter : filters) {
        if (!filter.empty() && lowered.find(filter) != std::string::npos)
            return true;
    }
    return false;
}

bool IsArcherModeEnabled(const AutoHuntSettings& settings)
{
    return settings.archerMode || settings.combatMode == AutoHuntCombatMode::Archer;
}

} // anonymous namespace

// ── StateName (file-scope free function) ─────────────────────────────────────
static const char* StateName(AutoHuntState state)
{
    switch (state) {
        case AutoHuntState::Idle:           return "Idle";
        case AutoHuntState::WaitingForGame: return "Waiting For Game";
        case AutoHuntState::Ready:          return "Ready";
        case AutoHuntState::TravelToZone:   return "Travel To Zone";
        case AutoHuntState::AcquireTarget:  return "Acquire Target";
        case AutoHuntState::ApproachTarget: return "Approach Target";
        case AutoHuntState::AttackTarget:   return "Attack Target";
        case AutoHuntState::LootNearby:     return "Loot Nearby";
        case AutoHuntState::Recover:        return "Recover";
        case AutoHuntState::TravelToMarket: return "Travel To Market";
        case AutoHuntState::Repair:         return "Repair";
        case AutoHuntState::BuyArrows:      return "Buy Arrows";
        case AutoHuntState::StoreItems:     return "Store Items";
        case AutoHuntState::ReturnToZone:   return "Return To Zone";
        case AutoHuntState::Failed:         return "Failed";
        default:                            return "Unknown";
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Shared method implementations
// ═════════════════════════════════════════════════════════════════════════════

void BaseHuntPlugin::SetState(AutoHuntState state, const char* statusText)
{
    if (m_state != state)
        spdlog::info("[hunt] State: {} -> {} | {}", GetStateName(), StateName(state),
            statusText ? statusText : "");
    m_state = state;
    snprintf(m_statusText, sizeof(m_statusText), "%s", statusText ? statusText : "");
}

const char* BaseHuntPlugin::GetStateName() const
{
    return StateName(m_state);
}

Position BaseHuntPlugin::GetEffectiveHeroPosition(const CHero* hero) const
{
    if (!hero)
        return {};

    if (hero->IsJumping())
        return hero->GetCommand().posTarget;

    if (m_pendingJumpTick != 0)
        return m_pendingJumpDest;

    return hero->m_posMap;
}

void BaseHuntPlugin::ClearPendingJumpState()
{
    m_pendingJumpDest = {};
    m_pendingJumpTick = 0;
    m_pendingJumpLastPos = {};
    m_pendingJumpLastProgressTick = 0;
    m_pendingJumpIsRetreat = false;
}

void BaseHuntPlugin::ArmPendingJump(CHero* hero, const Position& destination, DWORD now, bool isRetreat)
{
    m_pendingJumpDest = destination;
    m_pendingJumpTick = now;
    m_pendingJumpLastPos = hero ? hero->m_posMap : Position{};
    m_pendingJumpLastProgressTick = now;
    m_pendingJumpIsRetreat = isRetreat;
}

bool BaseHuntPlugin::UpdatePendingJumpState(CHero* hero, DWORD now)
{
    if (!hero || m_pendingJumpTick == 0)
        return false;

    const bool landed = hero->m_posMap.x == m_pendingJumpDest.x
        && hero->m_posMap.y == m_pendingJumpDest.y;
    if (landed) {
        // Retreat-specific landing is handled by subclass via HandleCombatRetreat
        ClearPendingJumpState();
        return false;
    }

    if (hero->m_posMap.x != m_pendingJumpLastPos.x || hero->m_posMap.y != m_pendingJumpLastPos.y) {
        m_pendingJumpLastPos = hero->m_posMap;
        m_pendingJumpLastProgressTick = now;
        return true;
    }

    const DWORD age = now - m_pendingJumpTick;
    const DWORD stalledFor = now - m_pendingJumpLastProgressTick;
    const bool stalled = stalledFor > kPendingJumpStallMs;
    const bool hardTimedOut = age > kPendingJumpHardTimeoutMs;
    if (!stalled && !hardTimedOut)
        return true;

    if (m_pendingJumpIsRetreat) {
        spdlog::warn("[hunt] Retreat jump failed pos=({},{}) dest=({},{}), age={}ms stalledFor={}ms retry=immediate",
            hero->m_posMap.x, hero->m_posMap.y,
            m_pendingJumpDest.x, m_pendingJumpDest.y,
            age, stalledFor);
    } else {
        spdlog::debug("[hunt] Move jump failed pos=({},{}) dest=({},{}), age={}ms stalledFor={}ms",
            hero->m_posMap.x, hero->m_posMap.y,
            m_pendingJumpDest.x, m_pendingJumpDest.y,
            age, stalledFor);
    }
    ClearPendingJumpState();
    return false;
}

void BaseHuntPlugin::RefreshRuntimeState(CHero* hero, CGameMap* map)
{
    m_lastHeroPos = hero ? hero->m_posMap : Position{};
    const OBJID currentMapId = map ? map->GetId() : 0;
    if (currentMapId != m_lastMapId)
        m_lootMgr.ResetLootPickupAttempts();
    m_lastMapId = currentMapId;
    m_lastHp = hero ? hero->GetCurrentHp() : 0;
    m_lastMaxHp = hero ? hero->GetMaxHp() : 0;
    m_lastMana = hero ? hero->GetCurrentMana() : 0;
    m_lastMaxMana = hero ? hero->GetMaxMana() : 0;
    m_lastBagCount = hero ? hero->m_deqItem.size() : 0;
    m_buffMgr.RefreshBuffState(hero);
    if (m_buffMgr.IsPreLandingRetreat() && m_buffMgr.CanRecastAnyFly(hero, GetAutoHuntSettings())) {
        m_buffMgr.SetPreLandingRetreat(false);
    }
    // Let subclass refresh combat-specific state (e.g. scatter range)
    RefreshCombatState(hero, GetAutoHuntSettings());
}

void BaseHuntPlugin::StopAutomation(bool cancelTravel)
{
    Pathfinder::Get().Stop();
    if (cancelTravel) {
        if (auto* travel = PluginManager::Get().GetPlugin<TravelPlugin>(); travel && travel->IsTraveling())
            travel->CancelTravel();
    }

    m_targetId = 0;
    m_lastClumpSize = 0;
    ClearPendingJumpState();
    m_lootMgr.ResetLootPickupAttempts();
    m_townService.ResetRepairSequence();
    m_townService.ResetBuyArrowsSequence();
    m_townService.ResetStoreSequence();
    m_nearbyPlayerTicks.clear();
    m_safetyResting = false;
    m_safetyRestStartTick = 0;
    SetState(AutoHuntState::Idle, "Disabled");
}

bool BaseHuntPlugin::HandleDeath(CHero* hero, TravelPlugin* travel, const AutoHuntSettings& settings)
{
    if (!hero) {
        m_reviveState = {};
        return false;
    }

    if (!hero->IsDead()) {
        m_reviveState = {};
        return false;
    }

    if (!settings.autoReviveInTown) {
        SetState(AutoHuntState::Failed, "Hero is dead");
        return true;
    }

    if (m_reviveState.deathTick == 0) {
        if (travel && travel->IsTraveling())
            travel->CancelTravel();
        m_targetId = 0;
        m_townService.ResetRepairSequence();
        m_townService.ResetStoreSequence();
    }

    const bool handled = HandleRevive(hero, m_reviveState,
        GetReviveDelayMs(settings), GetReviveRetryIntervalMs(settings),
        settings.autoReviveInTown, m_statusText, sizeof(m_statusText));

    if (handled)
        m_state = AutoHuntState::Recover;
    return handled;
}

bool BaseHuntPlugin::HasValidZone(const AutoHuntSettings& settings) const
{
    return HasValidHuntZone(settings);
}

bool BaseHuntPlugin::IsPointInZone(const AutoHuntSettings& settings, OBJID mapId, const Position& pos) const
{
    return IsPointInHuntZone(settings, mapId, pos);
}

Position BaseHuntPlugin::GetZoneAnchor(const AutoHuntSettings& settings) const
{
    return GetHuntZoneAnchor(settings);
}

bool BaseHuntPlugin::IsPlayerWhitelisted(const AutoHuntSettings& settings, const char* name) const
{
    if (!name || !name[0])
        return false;
    for (const std::string& token : ParseTokens(settings.playerWhitelist)) {
        if (_stricmp(token.c_str(), name) == 0)
            return true;
    }
    return false;
}

bool BaseHuntPlugin::CheckPlayerSafety(CHero* hero, CGameMap* map, TravelPlugin* travel,
    const AutoHuntSettings& settings)
{
    if (!settings.safetyEnabled || !hero || !map)
        return false;

    if (m_safetyResting) {
        if (m_lastMapId == MAP_MARKET && !travel->IsTraveling()) {
            // Cancel fly on arrival so we don't waste duration while resting
            if (hero->IsFlyActive())
                hero->CancelFly();
            DWORD elapsed = GetTickCount() - m_safetyRestStartTick;
            if (elapsed >= (DWORD)settings.safetyRestSec * 1000) {
                spdlog::info("[autohunt] Safety rest complete, returning to zone");
                m_safetyResting = false;
                m_nearbyPlayerTicks.clear();
                BeginTravelToZone(travel, settings);
                return true;
            }
            int remaining = (int)(settings.safetyRestSec - elapsed / 1000);
            char buf[128];
            snprintf(buf, sizeof(buf), "Safety rest in Market (%ds remaining)", remaining);
            SetState(AutoHuntState::TravelToMarket, buf);
        }
        return true;
    }

    CRoleMgr* mgr = Game::GetRoleMgr();
    if (!mgr || mgr->m_deqRole.empty() || mgr->m_deqRole.size() >= 10000)
        return false;

    const OBJID heroId = hero->GetID();
    const DWORD now = GetTickCount();
    const int range = settings.safetyPlayerRange;
    const DWORD threshold = (DWORD)settings.safetyDetectionSec * 1000;

    std::unordered_set<OBJID> inRange;
    for (size_t i = 0; i < mgr->m_deqRole.size() && i < 500; ++i) {
        const auto& roleRef = mgr->m_deqRole[i];
        if (!roleRef) continue;
        CRole* role = roleRef.get();
        if (!role->IsPlayer() || role->GetID() == heroId)
            continue;
        if (IsPlayerWhitelisted(settings, role->GetName()))
            continue;
        if (range > 0) {
            int dist = CGameMap::TileDist(hero->m_posMap.x, hero->m_posMap.y,
                                           role->m_posMap.x, role->m_posMap.y);
            if (dist > range)
                continue;
        }
        inRange.insert(role->GetID());
    }

    for (auto it = m_nearbyPlayerTicks.begin(); it != m_nearbyPlayerTicks.end(); ) {
        if (inRange.find(it->first) == inRange.end())
            it = m_nearbyPlayerTicks.erase(it);
        else
            ++it;
    }

    for (OBJID id : inRange) {
        if (m_nearbyPlayerTicks.find(id) == m_nearbyPlayerTicks.end())
            m_nearbyPlayerTicks[id] = now;
    }

    for (auto& [id, firstTick] : m_nearbyPlayerTicks) {
        if (now - firstTick >= threshold) {
            // Resolve player name for logging/notification
            const char* playerName = "Unknown";
            for (size_t i = 0; i < mgr->m_deqRole.size() && i < 500; ++i) {
                const auto& r = mgr->m_deqRole[i];
                if (r && r->GetID() == id) { playerName = r->GetName(); break; }
            }
            spdlog::warn("[autohunt] Player '{}' ({}) nearby for {}s, triggering safety escape to Market",
                playerName, id, (now - firstTick) / 1000);
            if (settings.safetyNotifyDiscord) {
                const char* heroName = hero->GetName();
                char buf[256];
                snprintf(buf, sizeof(buf), "[%s] Player safety triggered — '%s' nearby for %ds",
                    heroName, playerName, (int)((now - firstTick) / 1000));
                SendDiscordNotification(buf, true);
            }
            m_safetyResting = true;
            m_safetyRestStartTick = GetTickCount();
            m_nearbyPlayerTicks.clear();
            Pathfinder::Get().Stop();
            travel->StartTravel(MAP_MARKET, kMarketLandingPos);
            SetState(AutoHuntState::TravelToMarket, "Safety: traveling to Market");
            return true;
        }
    }

    return false;
}

HuntBuffCallbacks BaseHuntPlugin::MakeBuffCallbacks(CHero* hero, CGameMap* map, const AutoHuntSettings& settings)
{
    HuntBuffCallbacks cb;
    cb.setStateFn = [this](AutoHuntState state, const char* text) {
        SetState(state, text);
    };
    cb.startPathNearTargetFn = [this](CHero* h, CGameMap* m, const Position& pos, int range) {
        return StartPathNearTarget(h, m, pos, range);
    };
    cb.armPendingJumpFn = [this](CHero* h, const Position& dest, DWORD now, bool isRetreat) {
        ArmPendingJump(h, dest, now, isRetreat);
    };
    cb.recordMoveTick = [this]() -> DWORD {
        m_lastMoveTick = GetTickCount();
        return m_lastMoveTick;
    };
    cb.setTargetId = [this](OBJID id) {
        m_targetId = id;
    };
    cb.isLootPickupIgnoredFn = [this](OBJID id, DWORD now) {
        return m_lootMgr.IsLootPickupIgnored(id, now);
    };
    cb.tryPickupLootItemFn = [this, &settings](CHero* h, const std::shared_ptr<CMapItem>& item, DWORD now) {
        return m_lootMgr.TryPickupLootItem(h, settings, item, now,
            [this, h](DWORD t) { return UpdatePendingJumpState(h, t); });
    };
    // Base does not implement FindSafeArcherRetreat — subclass overrides if needed
    cb.findSafeArcherRetreatFn = [](CHero*, CGameMap*, const AutoHuntSettings&,
        const std::vector<CRole*>&, CRole*, Position&, int) {
        return false;
    };
    cb.collectHuntTargetsFn = [](const AutoHuntSettings& s) {
        return CollectHuntTargets(s);
    };
    return cb;
}

// ── Movement helpers ────────────────────────────────────────────────────────

bool BaseHuntPlugin::StartPathTo(CHero* hero, CGameMap* map, const Position& destination, int stopRange)
{
    if (!hero || !map)
        return false;

    const DWORD now = GetTickCount();
    const bool hasPendingJump = UpdatePendingJumpState(hero, now);

    const int hx = hasPendingJump ? m_pendingJumpDest.x : hero->m_posMap.x;
    const int hy = hasPendingJump ? m_pendingJumpDest.y : hero->m_posMap.y;

    const int dist = CGameMap::TileDist(hx, hy, destination.x, destination.y);
    if (dist <= stopRange)
        return false;

    if (Pathfinder::Get().IsActive()) {
        spdlog::trace("[hunt] Move blocked: pathfinder active, dest=({},{})", destination.x, destination.y);
        return true;
    }

    if (hero->IsJumping()) {
        spdlog::trace("[hunt] Move blocked: IsJumping pos=({},{}) cmd_target=({},{}) dest=({},{})",
            hero->m_posMap.x, hero->m_posMap.y,
            hero->GetCommand().posTarget.x, hero->GetCommand().posTarget.y,
            destination.x, destination.y);
        return true;
    }

    if (hasPendingJump) {
        spdlog::trace("[hunt] Move blocked: pending jump pos=({},{}) pendingDest=({},{}) age={}ms dest=({},{})",
            hero->m_posMap.x, hero->m_posMap.y,
            m_pendingJumpDest.x, m_pendingJumpDest.y,
            now - m_pendingJumpTick,
            destination.x, destination.y);
        return true;
    }

    const DWORD pathfinderJumpAge = now - Pathfinder::Get().GetLastJumpTick();
    if (pathfinderJumpAge < 500 && !Pathfinder::Get().IsActive()) {
        spdlog::trace("[hunt] Move blocked: pathfinder settle age={}ms pos=({},{}) dest=({},{})",
            pathfinderJumpAge, hero->m_posMap.x, hero->m_posMap.y,
            destination.x, destination.y);
        return true;
    }

    const AutoHuntSettings& settings = GetAutoHuntSettings();
    const bool canDirectJump = dist <= CGameMap::MAX_JUMP_DIST
        && map->CanJump(hx, hy, destination.x, destination.y, CGameMap::GetHeroAltThreshold())
        && !IsTileOccupied(destination.x, destination.y);
    const DWORD movementIntervalMs = canDirectJump
        ? GetJumpMovementIntervalMs(settings, hero)
        : GetMovementIntervalMs(settings);
    if (now - m_lastMoveTick < movementIntervalMs)
        return true;

    if (canDirectJump) {
        spdlog::debug("[hunt] JUMP ({},{}) -> ({},{}) dist={}", hx, hy, destination.x, destination.y, dist);
        hero->Jump(destination.x, destination.y);
        m_lastMoveTick = now;
        ArmPendingJump(hero, destination, now, false);
        return true;
    }

    spdlog::debug("[hunt] Direct jump failed dist={} canJump={} occupied={}, trying A* ({},{}) -> ({},{})",
        dist, map->CanJump(hx, hy, destination.x, destination.y, CGameMap::GetHeroAltThreshold()),
        IsTileOccupied(destination.x, destination.y),
        hx, hy, destination.x, destination.y);

    auto tilePath = map->FindPath(hx, hy, destination.x, destination.y, 1000000);
    if (tilePath.empty())
        return false;

    auto waypoints = map->SimplifyPath(tilePath);
    if (waypoints.empty())
        return false;

    Pathfinder::Get().StartPath(waypoints, movementIntervalMs);
    m_lastMoveTick = now;
    return true;
}

bool BaseHuntPlugin::StartWalkTo(CHero* hero, CGameMap* map, const Position& destination, int stopRange)
{
    if (!hero || !map)
        return false;

    const int hx = hero->m_posMap.x;
    const int hy = hero->m_posMap.y;
    const int dist = CGameMap::TileDist(hx, hy, destination.x, destination.y);
    if (dist <= stopRange)
        return false;

    if (Pathfinder::Get().IsActive())
        Pathfinder::Get().Stop();

    if (hero->IsJumping())
        return true;

    const DWORD now = GetTickCount();
    const bool hasPendingJump = UpdatePendingJumpState(hero, now);

    if (hasPendingJump)
        return true;

    if (now - m_lastMoveTick < GetMovementIntervalMs(GetAutoHuntSettings()))
        return true;

    Position bestPos = destination;
    bool found = false;
    int bestTargetDist = (std::numeric_limits<int>::max)();
    float bestHeroDist = (std::numeric_limits<float>::max)();
    const int searchRadius = (std::max)(stopRange, 0);

    for (int dx = -searchRadius; dx <= searchRadius; ++dx) {
        for (int dy = -searchRadius; dy <= searchRadius; ++dy) {
            const Position candidate = {destination.x + dx, destination.y + dy};
            const int targetDist = CGameMap::TileDist(candidate.x, candidate.y, destination.x, destination.y);
            if (targetDist > stopRange)
                continue;
            if (!map->IsWalkable(candidate.x, candidate.y))
                continue;
            if ((candidate.x != hx || candidate.y != hy) && IsTileOccupied(candidate.x, candidate.y))
                continue;
            if (!map->CanReach(hx, hy, candidate.x, candidate.y))
                continue;

            const float heroDist = Position{hx, hy}.DistanceTo(candidate);
            if (!found || targetDist < bestTargetDist
                || (targetDist == bestTargetDist && heroDist < bestHeroDist)) {
                found = true;
                bestPos = candidate;
                bestTargetDist = targetDist;
                bestHeroDist = heroDist;
            }
        }
    }

    if (!found)
        return false;

    hero->Walk(bestPos.x, bestPos.y);
    m_lastMoveTick = now;
    return true;
}

bool BaseHuntPlugin::StartPathNearTarget(CHero* hero, CGameMap* map, const Position& targetPos, int desiredRange)
{
    if (!hero || !map)
        return false;

    const Position effectivePos = GetEffectiveHeroPosition(hero);
    const int currentDist = CGameMap::TileDist(effectivePos.x, effectivePos.y, targetPos.x, targetPos.y);
    if (currentDist <= desiredRange)
        return false;

    Position bestPos = targetPos;
    bool found = false;
    int bestTargetDist = (std::numeric_limits<int>::max)();
    float bestHeroDist = (std::numeric_limits<float>::max)();
    const int searchRadius = (std::max)(desiredRange + 2, 4);

    for (int dx = -searchRadius; dx <= searchRadius; ++dx) {
        for (int dy = -searchRadius; dy <= searchRadius; ++dy) {
            const Position candidate = {targetPos.x + dx, targetPos.y + dy};
            const int targetDist = CGameMap::TileDist(candidate.x, candidate.y, targetPos.x, targetPos.y);
            if (targetDist > searchRadius)
                continue;
            if (!map->IsWalkable(candidate.x, candidate.y))
                continue;
            if ((candidate.x != hero->m_posMap.x || candidate.y != hero->m_posMap.y)
                && IsTileOccupied(candidate.x, candidate.y)) {
                continue;
            }

            const float heroDist = effectivePos.DistanceTo(candidate);
            if (!found || targetDist < bestTargetDist
                || (targetDist == bestTargetDist && heroDist < bestHeroDist)) {
                found = true;
                bestPos = candidate;
                bestTargetDist = targetDist;
                bestHeroDist = heroDist;
            }
        }
    }

    if (!found)
        return false;

    return StartPathTo(hero, map, bestPos, 0);
}

// ── Travel helpers ──────────────────────────────────────────────────────────

void BaseHuntPlugin::BeginTravelToZone(TravelPlugin* travel, const AutoHuntSettings& settings)
{
    if (!travel) {
        SetState(AutoHuntState::Failed, "Travel plugin not available");
        return;
    }

    // Arrow buying happens on arrival at the zone city (HandleTravelToZone),
    // not before travel starts — Market has no blacksmith.

    const Position anchor = GetZoneAnchor(settings);
    if (IsZeroPos(anchor)) {
        SetState(AutoHuntState::Failed, "Hunt zone anchor is not set");
        return;
    }

    CHero* zoneHero = Game::GetHero();
    CGameMap* zoneMap = Game::GetMap();
    const bool sameMapOutsideZone = zoneHero && zoneMap
        && zoneMap->GetId() == settings.zoneMapId
        && !IsPointInZone(settings, zoneMap->GetId(), zoneHero->m_posMap);
    if (sameMapOutsideZone) {
        if (zoneHero->IsJumping() || Pathfinder::Get().IsActive()) {
            SetState(m_lastMapId == MAP_MARKET ? AutoHuntState::ReturnToZone : AutoHuntState::TravelToZone,
                "Moving into hunt zone");
            return;
        }

        Position zoneEntry = {};
        if (!FindClosestZoneTile(zoneMap, settings, zoneHero->m_posMap, zoneEntry)) {
            SetState(AutoHuntState::Failed, "No walkable tile inside hunt zone");
            return;
        }

        if (!StartPathTo(zoneHero, zoneMap, zoneEntry, 0)) {
            SetState(AutoHuntState::Failed, "Failed to move into hunt zone");
            return;
        }

        SetState(m_lastMapId == MAP_MARKET ? AutoHuntState::ReturnToZone : AutoHuntState::TravelToZone,
            "Moving into hunt zone");
        return;
    }

    travel->StartTravel(settings.zoneMapId, anchor);
    SetState(m_lastMapId == MAP_MARKET ? AutoHuntState::ReturnToZone : AutoHuntState::TravelToZone,
        "Traveling to hunt zone");
}

void BaseHuntPlugin::BeginTravelToMarket(TravelPlugin* travel, CHero* hero, const AutoHuntSettings& settings)
{
    if (!travel || !hero) {
        SetState(AutoHuntState::Failed, "Travel plugin not available");
        return;
    }

    Pathfinder::Get().Stop();
    m_townService.ResetRepairSequence();
    m_townService.ResetBuyArrowsSequence();
    m_townService.ResetStoreSequence();

    if (m_lastMapId == MAP_MARKET) {
        if (settings.autoRepair && m_townService.NeedsRepair(hero, settings)) {
            SetState(AutoHuntState::Repair, "Moving to Pharmacist");
        } else if (settings.autoStore && m_townService.NeedsStorage(hero, settings)) {
            SetState(AutoHuntState::StoreItems, "Processing storage rules");
        } else {
            BeginTravelToZone(travel, settings);
        }
        return;
    }

    travel->StartTravel(MAP_MARKET, kMarketLandingPos);
    SetState(AutoHuntState::TravelToMarket, "Traveling to Market");
}

void BaseHuntPlugin::HandleTravelToZone(TravelPlugin* travel, const AutoHuntSettings& settings)
{
    if (!travel) {
        SetState(AutoHuntState::Failed, "Travel plugin not available");
        return;
    }

    if (travel->GetState() == TravelState::Failed) {
        SetState(AutoHuntState::Failed, "Failed to reach hunt zone");
        return;
    }

    if (travel->IsTraveling()) {
        if (m_lastMapId == settings.zoneMapId && IsPointInZone(settings, m_lastMapId, m_lastHeroPos)) {
            travel->CancelTravel();
            CHero* zoneHero = Game::GetHero();
            if (zoneHero && NeedsTownRunArrows(zoneHero, settings) && HuntTownService::HasBlacksmithOnMap(m_lastMapId)) {
                m_townService.ResetBuyArrowsSequence();
                SetState(AutoHuntState::BuyArrows, "Buying arrows at zone city");
                return;
            }
            m_targetId = 0;
            SetState(AutoHuntState::AcquireTarget, "Entered hunt zone");
            return;
        }
        SetState(m_state, "Traveling to hunt zone");
        return;
    }

    if (m_lastMapId != settings.zoneMapId || !IsPointInZone(settings, m_lastMapId, m_lastHeroPos)) {
        BeginTravelToZone(travel, settings);
        return;
    }

    CHero* hero = Game::GetHero();
    if (hero && NeedsTownRunArrows(hero, settings) && HuntTownService::HasBlacksmithOnMap(m_lastMapId)) {
        m_townService.ResetBuyArrowsSequence();
        SetState(AutoHuntState::BuyArrows, "Buying arrows at zone city");
        return;
    }

    m_targetId = 0;
    SetState(AutoHuntState::AcquireTarget, "Scanning hunt zone");
}

void BaseHuntPlugin::HandleTravelToMarket(TravelPlugin* travel, CHero* hero, const AutoHuntSettings& settings)
{
    if (!travel || !hero) {
        SetState(AutoHuntState::Failed, "Travel plugin not available");
        return;
    }

    if (travel->GetState() == TravelState::Failed) {
        SetState(AutoHuntState::Failed, "Failed to reach Market");
        return;
    }

    if (travel->IsTraveling()) {
        SetState(AutoHuntState::TravelToMarket, "Traveling to Market");
        return;
    }

    if (m_lastMapId != MAP_MARKET) {
        BeginTravelToMarket(travel, hero, settings);
        return;
    }

    if (m_safetyResting) {
        if (m_safetyRestStartTick == 0)
            m_safetyRestStartTick = GetTickCount();
        return;
    }

    if (settings.autoRepair && m_townService.NeedsRepair(hero, settings)) {
        m_townService.ResetRepairSequence();
        SetState(AutoHuntState::Repair, "Moving to Pharmacist");
    } else if (settings.autoStore && m_townService.NeedsStorage(hero, settings)) {
        m_townService.ResetStoreSequence();
        SetState(AutoHuntState::StoreItems, "Processing storage rules");
    } else {
        BeginTravelToZone(travel, settings);
    }
}

HuntTownCallbacks BaseHuntPlugin::MakeTownCallbacks(TravelPlugin* travel, CHero* hero, const AutoHuntSettings& settings)
{
    HuntTownCallbacks cb;
    cb.setStateFn = [this](AutoHuntState state, const char* text) {
        SetState(state, text);
    };
    cb.startPathNearTargetFn = [this](CHero* h, CGameMap* m, const Position& pos, int range) {
        return StartPathNearTarget(h, m, pos, range);
    };
    const AutoHuntSettings* settingsPtr = &settings;
    cb.beginTravelToZoneFn = [this, travel, settingsPtr]() {
        BeginTravelToZone(travel, *settingsPtr);
    };
    cb.beginTravelToMarketFn = [this, travel, hero, settingsPtr]() {
        BeginTravelToMarket(travel, hero, *settingsPtr);
    };
    return cb;
}

// ── Zone helpers ────────────────────────────────────────────────────────────

bool BaseHuntPlugin::FindClosestZoneTile(CGameMap* map, const AutoHuntSettings& settings,
    const Position& from, Position& outZonePos) const
{
    outZonePos = {};
    if (!map)
        return false;

    int minX = 0;
    int maxX = 0;
    int minY = 0;
    int maxY = 0;
    if (settings.zoneMode == AutoHuntZoneMode::Circle) {
        if (IsZeroPos(settings.zoneCenter) || settings.zoneRadius <= 0)
            return false;
        minX = settings.zoneCenter.x - settings.zoneRadius;
        maxX = settings.zoneCenter.x + settings.zoneRadius;
        minY = settings.zoneCenter.y - settings.zoneRadius;
        maxY = settings.zoneCenter.y + settings.zoneRadius;
    } else {
        if (settings.zonePolygon.empty())
            return false;
        minX = maxX = settings.zonePolygon.front().x;
        minY = maxY = settings.zonePolygon.front().y;
        for (const Position& vertex : settings.zonePolygon) {
            minX = (std::min)(minX, vertex.x);
            maxX = (std::max)(maxX, vertex.x);
            minY = (std::min)(minY, vertex.y);
            maxY = (std::max)(maxY, vertex.y);
        }
    }

    const Position anchor = GetZoneAnchor(settings);
    bool found = false;
    int bestHeroDist = (std::numeric_limits<int>::max)();
    float bestAnchorDist = (std::numeric_limits<float>::max)();
    for (int x = minX; x <= maxX; ++x) {
        for (int y = minY; y <= maxY; ++y) {
            const Position candidate = {x, y};
            if (!IsPointInZone(settings, settings.zoneMapId, candidate))
                continue;
            if (!map->IsWalkable(x, y))
                continue;
            if ((x != from.x || y != from.y) && IsTileOccupied(x, y))
                continue;

            const int heroDist = CGameMap::TileDist(from.x, from.y, x, y);
            const float anchorDist = IsZeroPos(anchor) ? 0.0f : candidate.DistanceTo(anchor);
            if (!found
                || heroDist < bestHeroDist
                || (heroDist == bestHeroDist && anchorDist < bestAnchorDist)) {
                found = true;
                outZonePos = candidate;
                bestHeroDist = heroDist;
                bestAnchorDist = anchorDist;
            }
        }
    }

    return found;
}

// ═════════════════════════════════════════════════════════════════════════════
// Update() — the shared hunt loop with virtual combat dispatch
// ═════════════════════════════════════════════════════════════════════════════

void BaseHuntPlugin::Update()
{
    AutoHuntSettings& settings = GetAutoHuntSettings();
    TravelPlugin* travel = PluginManager::Get().GetPlugin<TravelPlugin>();

    CHero* hero = Game::GetHero();
    CGameMap* map = Game::GetMap();
    RefreshRuntimeState(hero, map);

    // Update debug best-clump overlay
    m_debugBestClumpCenter = {};
    m_debugBestClumpSize = 0;
    if (settings.debugShowBestClump && hero && map) {
        const auto targets = CollectHuntTargets(settings);
        if (!targets.empty()) {
            const float clumpR = (float)(std::max)(1, settings.clumpRadius);
            if (CRole* best = FindBestClusterTarget(targets, hero->m_posMap, clumpR, &m_debugBestClumpSize)) {
                m_debugBestClumpCenter = best->m_posMap;
            }
        }
    }

    if (!m_enabled) {
        if (m_state != AutoHuntState::Idle)
            StopAutomation(true);
        return;
    }

    // Sync combat mode so IsArcherModeEnabled() matches the active plugin
    settings.combatMode = GetExpectedCombatMode();

    // Mutual exclusion: if another BaseHuntPlugin subclass is already enabled, skip
    for (auto* p = &PluginManager::Get();;) {
        // Check via GetPlugin iteration — we only need to ensure one BaseHuntPlugin runs.
        // Since GetPlugin returns the first match by dynamic_cast, and we ARE a BaseHuntPlugin,
        // just check if another one exists and is enabled.
        (void)p;
        break;
    }

    if (!hero || !map) {
        SetState(AutoHuntState::WaitingForGame, "Waiting for hero and map");
        return;
    }

    m_lootMgr.PruneLootPickupAttempts(map);
    PruneLootDropRecords();

    if (!HasValidZone(settings)) {
        SetState(AutoHuntState::Failed, "Configure a valid hunt zone first");
        return;
    }

    if (HandleDeath(hero, travel, settings))
        return;

    // Player safety: only detect while hunting in zone, but always handle active rest
    if (m_safetyResting) {
        if (CheckPlayerSafety(hero, map, travel, settings))
            return;
    } else if (settings.safetyEnabled
               && map->GetId() == settings.zoneMapId
               && IsPointInZone(settings, map->GetId(), hero->m_posMap)) {
        if (CheckPlayerSafety(hero, map, travel, settings))
            return;
    } else {
        m_nearbyPlayerTicks.clear();
    }

    if (travel && travel->IsTraveling()) {
        if (m_state == AutoHuntState::TravelToMarket) {
            HandleTravelToMarket(travel, hero, settings);
            return;
        }
        if (m_state == AutoHuntState::TravelToZone || m_state == AutoHuntState::ReturnToZone) {
            HandleTravelToZone(travel, settings);
            return;
        }

        SetState(m_state, "Waiting for travel plugin");
        return;
    }

    UpdatePendingJumpState(hero, GetTickCount());

    {
        const HuntBuffCallbacks buffCb = MakeBuffCallbacks(hero, map, settings);
        if (m_buffMgr.TryPreLandingSafety(hero, map, settings, buffCb))
            return;

        // Cast skills in priority order (flyOnlyWithCyclone is handled inside TryCastFly/XpFly)
        for (int i = 0; i < kHuntSkillCount; i++) {
            const auto& entry = settings.skillPriorities[i];
            if (!entry.enabled) continue;
            switch (entry.type) {
                case HuntSkillType::Superman:
                    if (m_buffMgr.TryCastSuperman(hero, settings, buffCb)) return;
                    break;
                case HuntSkillType::Cyclone:
                    if (m_buffMgr.TryCastCyclone(hero, settings, buffCb)) return;
                    break;
                case HuntSkillType::Accuracy:
                    if (m_buffMgr.TryCastAccuracy(hero, settings, buffCb)) return;
                    break;
                case HuntSkillType::XpFly:
                    if (m_buffMgr.TryCastXpFly(hero, settings, buffCb)) return;
                    break;
                case HuntSkillType::Fly:
                    if (m_buffMgr.TryCastFly(hero, settings, buffCb)) return;
                    break;
                case HuntSkillType::Stigma:
                    if (m_buffMgr.TryCastStigma(hero, settings, m_lastMana, buffCb)) return;
                    break;
                default: break;
            }
        }

        if (m_buffMgr.TryUsePotions(hero, settings, m_lastHp, m_lastMaxHp, m_lastMana, m_lastMaxMana, buffCb))
            return;
    }

    // Subclass combat-item management (arrows, etc.)
    if (HandleCombatItems(hero, settings))
        return;

    if (m_state == AutoHuntState::TravelToMarket) {
        HandleTravelToMarket(travel, hero, settings);
        return;
    }

    if (m_state == AutoHuntState::Repair) {
        m_townService.HandleRepairState(hero, map, settings, MakeTownCallbacks(travel, hero, settings));
        return;
    }

    if (m_state == AutoHuntState::BuyArrows) {
        m_townService.HandleBuyArrowsState(hero, map, settings, MakeTownCallbacks(travel, hero, settings));
        return;
    }

    if (m_state == AutoHuntState::StoreItems) {
        m_townService.HandleStoreState(hero, map, settings, MakeTownCallbacks(travel, hero, settings));
        return;
    }

    if (m_state == AutoHuntState::TravelToZone || m_state == AutoHuntState::ReturnToZone) {
        HandleTravelToZone(travel, settings);
        return;
    }

    // Meteor packing (shared)
    if (settings.packMeteorsIntoScrolls && hero) {
        const DWORD now = GetTickCount();
        if (now - m_lastPackTick >= GetItemActionIntervalMs(settings)) {
            if (CountInventoryItemsByType(hero, ItemTypeId::METEOR) >= 10) {
                CItem* meteor = FindInventoryItemByType(hero, ItemTypeId::METEOR);
                if (meteor) {
                    hero->UseItem(meteor->GetID());
                    m_lastPackTick = now;
                    SetState(AutoHuntState::Recover, "Packing Meteors into MeteorScrolls");
                    return;
                }
            }
        }
    }

    // Repair / storage — at Market (arrows handled separately below)
    if (m_townService.NeedTownRun(hero, settings, false)) {
        BeginTravelToMarket(travel, hero, settings);
        return;
    }

    // Arrow restocking — buy at local blacksmith or travel to zone city
    if (NeedsTownRunArrows(hero, settings)) {
        if (HuntTownService::HasBlacksmithOnMap(m_lastMapId)) {
            m_townService.ResetBuyArrowsSequence();
            SetState(AutoHuntState::BuyArrows, "Buying arrows");
            return;
        }
        if (HuntTownService::HasBlacksmithOnMap(settings.zoneMapId)) {
            BeginTravelToZone(travel, settings);
            return;
        }
    }

    if (map->GetId() != settings.zoneMapId || !IsPointInZone(settings, map->GetId(), hero->m_posMap)) {
        BeginTravelToZone(travel, settings);
        return;
    }

    if (travel && travel->IsTraveling()) {
        SetState(AutoHuntState::Ready, "Waiting for travel plugin");
        return;
    }

    // ── Loot phase ──────────────────────────────────────────────────────
    std::shared_ptr<CMapItem> loot = m_lootMgr.FindBestLoot(hero, map, settings,
        [this](OBJID id, DWORD now) { return m_lootMgr.IsLootPickupIgnored(id, now); },
        [this, &settings](OBJID mapId, const Position& pos) { return IsPointInZone(settings, mapId, pos); });
    const bool midMovement = hero->IsJumping() || Pathfinder::Get().IsActive();
    if (loot) {
        const int lootDist = CGameMap::TileDist(hero->m_posMap.x, hero->m_posMap.y, loot->m_pos.x, loot->m_pos.y);
        spdlog::trace("[hunt-loot] Loot id={} type={} dist={} range={}", loot->m_id, loot->m_idType, lootDist, GetLootRange(settings));
        const DWORD pickupNow = GetTickCount();
        if (IsWithinLootPickupRange(settings, lootDist) && m_lootMgr.TryPickupLootItem(hero, settings, loot, pickupNow,
                [this, hero](DWORD t) { return UpdatePendingJumpState(hero, t); })) {
            m_targetId = loot->m_id;
            SetState(AutoHuntState::LootNearby, "Picking up nearby loot");
            return;
        }
        if (!midMovement) {
            if (StartPathNearTarget(hero, map, loot->m_pos, kLootPathStopRange)) {
                m_targetId = loot->m_id;
                SetState(AutoHuntState::LootNearby, "Jumping to loot");
                return;
            }
            if (IsWithinLootPickupRange(settings, lootDist)) {
                m_targetId = loot->m_id;
                SetState(AutoHuntState::LootNearby, "Settling on nearby loot");
                return;
            }
            spdlog::warn("[hunt-loot] Failed to path to loot id={} at ({},{}) dist={}",
                loot->m_id, loot->m_pos.x, loot->m_pos.y, lootDist);
        }
    }

    // ── Combat phase (virtual dispatch) ─────────────────────────────────
    const DWORD now = GetTickCount();
    const bool hasPendingJump = UpdatePendingJumpState(hero, now);

    const bool approachCommitted = (m_state == AutoHuntState::ApproachTarget || m_state == AutoHuntState::LootNearby)
        && (hero->IsJumping() || hasPendingJump || Pathfinder::Get().IsActive());

    // Stuck detection
    if (m_state == AutoHuntState::ApproachTarget
        && !hero->IsJumping() && !hasPendingJump && !Pathfinder::Get().IsActive()) {
        if (m_approachStartTick == 0)
            m_approachStartTick = now;
        else if (now - m_approachStartTick > 3000) {
            m_unreachableTargetId = m_targetId;
            m_unreachableExpireTick = now + 10000;
            m_approachStartTick = 0;
            m_targetId = 0;
            SetState(AutoHuntState::AcquireTarget, "Target unreachable, finding new target");
        }
    } else {
        m_approachStartTick = 0;
    }

    if (m_unreachableTargetId != 0 && GetTickCount() >= m_unreachableExpireTick)
        m_unreachableTargetId = 0;

    // ── Target finding via virtual dispatch ──────────────────────────────
    Position approachPos = {};
    Position attackPos = {};
    int clumpSize = 0;
    bool useScatter = false;
    CRole* target = FindBestTarget(hero, map, settings, &approachPos, &attackPos, &clumpSize, &useScatter);

    // Skip blacklisted unreachable target
    if (target && target->GetID() == m_unreachableTargetId)
        target = nullptr;

    m_lastClumpSize = clumpSize;
    m_lastTargetPos = target ? target->m_posMap : Position{};

    if (target) {
        m_targetId = target->GetID();
        const bool movementCommitted = hero->IsJumping() || hasPendingJump;

        // Combat retreat (virtual — archer overrides, melee returns false)
        if (HandleCombatRetreat(hero, map, settings, target))
            return;

        // Combat approach (virtual dispatch)
        HandleCombatApproach(hero, map, settings, target, approachPos, movementCommitted);

        // If approach has active movement, don't attack yet — wait for arrival
        if (m_state == AutoHuntState::ApproachTarget
            && (hero->IsJumping() || m_pendingJumpTick != 0 || Pathfinder::Get().IsActive()))
            return;

        // Combat attack (virtual dispatch)
        HandleCombatAttack(hero, map, settings, target, attackPos, now);
        return;
    }

    // ── No target found ─────────────────────────────────────────────────
    m_lastClumpSize = 0;
    if (loot) {
        spdlog::trace("[hunt-loot] No combat target, pathing to loot id={} type={} at ({},{})",
            loot->m_id, loot->m_idType, loot->m_pos.x, loot->m_pos.y);
        const bool startedMove = StartPathNearTarget(hero, map, loot->m_pos, kLootPathStopRange);
        m_targetId = loot->m_id;
        SetState(AutoHuntState::LootNearby, startedMove ? "Jumping to loot" : "Settling on nearby loot");
        return;
    }

    // Subclass idle behavior (archer patrol, etc.)
    if (HandleNoTargetIdle(hero, map, settings))
        return;

    const Position anchor = GetZoneAnchor(settings);
    if (!IsZeroPos(anchor)
        && CGameMap::TileDist(hero->m_posMap.x, hero->m_posMap.y, anchor.x, anchor.y) > 3) {
        StartPathTo(hero, map, anchor, 3);
        SetState(AutoHuntState::ReturnToZone, "Returning to hunt anchor");
        return;
    }

    m_targetId = 0;
    SetState(AutoHuntState::AcquireTarget, "Scanning for monsters");
}

// ═════════════════════════════════════════════════════════════════════════════
// OnMapClick
// ═════════════════════════════════════════════════════════════════════════════

bool BaseHuntPlugin::OnMapClick(const Position& tile)
{
    AutoHuntSettings& settings = GetAutoHuntSettings();
    CGameMap* map = Game::GetMap();
    if (!map || m_zoneCaptureMode == ZoneCaptureMode::None)
        return false;

    settings.zoneMapId = map->GetId();

    switch (m_zoneCaptureMode) {
        case ZoneCaptureMode::CircleCenter:
            settings.zoneCenter = tile;
            m_zoneCaptureMode = ZoneCaptureMode::None;
            snprintf(m_statusText, sizeof(m_statusText), "Zone center set to (%d,%d)", tile.x, tile.y);
            return true;

        case ZoneCaptureMode::CircleRadius:
            if (IsZeroPos(settings.zoneCenter)) {
                snprintf(m_statusText, sizeof(m_statusText), "Set circle center first");
            } else {
                settings.zoneRadius = (std::max)(1, CGameMap::TileDist(
                    settings.zoneCenter.x, settings.zoneCenter.y, tile.x, tile.y));
                snprintf(m_statusText, sizeof(m_statusText), "Zone radius set to %d", settings.zoneRadius);
            }
            m_zoneCaptureMode = ZoneCaptureMode::None;
            return true;

        case ZoneCaptureMode::PolygonVertex:
            settings.zonePolygon.push_back(tile);
            snprintf(m_statusText, sizeof(m_statusText), "Added polygon vertex (%d,%d)", tile.x, tile.y);
            return true;

        case ZoneCaptureMode::None:
        default:
            return false;
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// RenderUI — shared settings UI
// ═════════════════════════════════════════════════════════════════════════════

void BaseHuntPlugin::RenderSelectedItemList(const char* title, const char* clearButtonLabel,
    const char* tableId, std::vector<uint32_t>& itemIds)
{
    ImGui::Text("%s: %d", title, (int)itemIds.size());
    ImGui::SameLine();
    if (ImGui::SmallButton(clearButtonLabel))
        itemIds.clear();

    if (itemIds.empty()) {
        ImGui::TextDisabled("No items selected.");
        return;
    }

    int removeIndex = -1;
    if (ImGui::BeginTable(tableId, 3,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
            ImVec2(0, 140.0f))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 75.0f);
        ImGui::TableHeadersRow();

        for (size_t i = 0; i < itemIds.size(); ++i) {
            const uint32_t itemId = itemIds[i];
            const ItemTypeInfo* info = GetItemTypeInfo(itemId);

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%s", info ? info->name.c_str() : "Unknown");
            ImGui::TableNextColumn();
            ImGui::Text("%u", itemId);
            ImGui::TableNextColumn();
            char buttonId[64];
            snprintf(buttonId, sizeof(buttonId), "Remove##%s%u", tableId, itemId);
            if (ImGui::SmallButton(buttonId))
                removeIndex = (int)i;
        }

        ImGui::EndTable();
    }

    if (removeIndex >= 0)
        itemIds.erase(itemIds.begin() + removeIndex);
}

void BaseHuntPlugin::RenderItemSelector(AutoHuntSettings& settings)
{
    ImGui::InputText("Item Search", m_itemSearch, IM_ARRAYSIZE(m_itemSearch));
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear Search"))
        m_itemSearch[0] = '\0';

    ImGui::Text("Loot: %d  Warehouse: %d  Priority Return: %d",
        (int)settings.lootItemIds.size(),
        (int)settings.warehouseItemIds.size(),
        (int)settings.priorityReturnItemIds.size());

    const std::string searchText = ToLowerCopy(m_itemSearch);
    int shown = 0;
    bool limitedResults = false;
    ImGui::BeginChild("##basehuntitembrowser", ImVec2(0, 240.0f), ImGuiChildFlags_Borders);
    if (ImGui::BeginTable("##basehuntitemtable", 5,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
            ImVec2(0, 0))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Item", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Loot", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("Warehouse", ImGuiTableColumnFlags_WidthFixed, 95.0f);
        ImGui::TableSetupColumn("Priority", ImGuiTableColumnFlags_WidthFixed, 75.0f);
        ImGui::TableHeadersRow();

        for (const ItemTypeInfo* info : GetAllItemTypes()) {
            if (!info)
                continue;

            const bool matchesSearch =
                searchText.empty()
                || ToLowerCopy(info->name).find(searchText) != std::string::npos
                || std::to_string(info->id).find(m_itemSearch) != std::string::npos;
            if (!matchesSearch)
                continue;

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%s", info->name.c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%u", info->id);

            ImGui::TableNextColumn();
            {
                const bool selected = ContainsItemId(settings.lootItemIds, info->id);
                char buttonId[64];
                snprintf(buttonId, sizeof(buttonId), "%s##loot%u", selected ? "Remove" : "Add", info->id);
                if (ImGui::SmallButton(buttonId)) {
                    if (selected)
                        RemoveItemId(settings.lootItemIds, info->id);
                    else
                        AddItemId(settings.lootItemIds, info->id);
                }
            }

            ImGui::TableNextColumn();
            {
                const bool selected = ContainsItemId(settings.warehouseItemIds, info->id);
                char buttonId[64];
                snprintf(buttonId, sizeof(buttonId), "%s##warehouse%u", selected ? "Remove" : "Add", info->id);
                if (ImGui::SmallButton(buttonId)) {
                    if (selected)
                        RemoveItemId(settings.warehouseItemIds, info->id);
                    else
                        AddItemId(settings.warehouseItemIds, info->id);
                }
            }

            ImGui::TableNextColumn();
            {
                const bool selected = ContainsItemId(settings.priorityReturnItemIds, info->id);
                char buttonId[64];
                snprintf(buttonId, sizeof(buttonId), "%s##priority%u", selected ? "Remove" : "Add", info->id);
                if (ImGui::SmallButton(buttonId)) {
                    if (selected)
                        RemoveItemId(settings.priorityReturnItemIds, info->id);
                    else
                        AddItemId(settings.priorityReturnItemIds, info->id);
                }
            }

            shown++;
            if (searchText.empty() && shown >= 250) {
                limitedResults = true;
                break;
            }
        }

        ImGui::EndTable();
    }

    if (shown == 0) {
        ImGui::TextDisabled("No item types matched the current filter.");
    } else if (limitedResults) {
        ImGui::TextDisabled("Showing the first 250 items. Use search to narrow the list.");
    }
    ImGui::EndChild();
}

void BaseHuntPlugin::RenderSharedUI()
{
    RenderGeneralUI();
    RenderSettingsUI();
}

void BaseHuntPlugin::RenderGeneralUI()
{
    AutoHuntSettings& settings = GetAutoHuntSettings();
    constexpr ImGuiTreeNodeFlags kSectionFlags = ImGuiTreeNodeFlags_DefaultOpen;

    if (ImGui::CollapsingHeader("General", kSectionFlags)) {
        ImGui::Checkbox("Use Potions", &settings.usePotions);
        ImGui::Checkbox("Auto Repair", &settings.autoRepair);
        ImGui::SameLine();
        ImGui::Checkbox("Auto Store", &settings.autoStore);
        ImGui::Checkbox("Auto Revive In Town", &settings.autoReviveInTown);

        ImGui::SliderInt("HP Potion %", &settings.hpPotionPercent, 1, 99);
        ImGui::SliderInt("Mana Potion %", &settings.manaPotionPercent, 1, 99);
        ImGui::SliderInt("Repair %", &settings.repairPercent, 1, 100);

        if (settings.usePotions)
            ImGui::Checkbox("Pick Up Nearby HP Potion When Low", &settings.pickupNearbyHpPotionWhenLow);

        // ── Skill Priority List ──
        ImGui::Separator();
        ImGui::Text("Skill Priority:");
        bool skillChanged = false;
        for (int i = 0; i < kHuntSkillCount; i++) {
            auto& entry = settings.skillPriorities[i];
            ImGui::PushID(i);

            // Checkbox + rank + name
            char label[64];
            snprintf(label, sizeof(label), "%d. %s", i + 1, HuntSkillName(entry.type));
            if (ImGui::Checkbox(label, &entry.enabled))
                skillChanged = true;

            // Up button
            ImGui::SameLine();
            if (i > 0) {
                if (ImGui::SmallButton("^")) {
                    std::swap(settings.skillPriorities[i], settings.skillPriorities[i - 1]);
                    skillChanged = true;
                }
            } else {
                ImGui::BeginDisabled();
                ImGui::SmallButton("^");
                ImGui::EndDisabled();
            }

            // Down button
            ImGui::SameLine();
            if (i < kHuntSkillCount - 1) {
                if (ImGui::SmallButton("v")) {
                    std::swap(settings.skillPriorities[i], settings.skillPriorities[i + 1]);
                    skillChanged = true;
                }
            } else {
                ImGui::BeginDisabled();
                ImGui::SmallButton("v");
                ImGui::EndDisabled();
            }

            // Sub-options for enabled skills
            if (entry.enabled) {
                if (entry.type == HuntSkillType::Fly) {
                    ImGui::Indent();
                    if (ImGui::Checkbox("Fly Only With Cyclone", &settings.flyOnlyWithCyclone))
                        skillChanged = true;
                    ImGui::Unindent();
                }
                if (entry.type == HuntSkillType::Stigma) {
                    ImGui::Indent();
                    if (ImGui::Checkbox("Pick Up Nearby Mana Potion For Stigma", &settings.pickupNearbyManaPotionForStigma))
                        skillChanged = true;
                    ImGui::Unindent();
                }
            }

            ImGui::PopID();
        }
        if (skillChanged)
            settings.SyncSkillBoolsFromPriorities();
    }
}

void BaseHuntPlugin::RenderSettingsUI()
{
    AutoHuntSettings& settings = GetAutoHuntSettings();
    CHero* hero = Game::GetHero();
    CRoleMgr* mgr = Game::GetRoleMgr();
    constexpr ImGuiTreeNodeFlags kSectionFlags = ImGuiTreeNodeFlags_DefaultOpen;

    if (ImGui::CollapsingHeader("Hunt Zone", kSectionFlags)) {
        static const char* kZoneModes[] = { "Circle", "Polygon" };
        int zoneMode = static_cast<int>(settings.zoneMode);
        if (ImGui::Combo("Zone Mode", &zoneMode, kZoneModes, IM_ARRAYSIZE(kZoneModes))) {
            settings.zoneMode = static_cast<AutoHuntZoneMode>(zoneMode);
            m_zoneCaptureMode = ZoneCaptureMode::None;
        }

        ImGui::Text("Zone Map: %u", settings.zoneMapId);
        ImGui::InputInt2("Zone Center", &settings.zoneCenter.x);
        if (settings.zoneMode == AutoHuntZoneMode::Circle)
            ImGui::SliderInt("Zone Radius", &settings.zoneRadius, 1, 80);

        if (hero && ImGui::Button("Use Hero Position")) {
            settings.zoneMapId = m_lastMapId;
            settings.zoneCenter = hero->m_posMap;
        }

        if (settings.zoneMode == AutoHuntZoneMode::Circle) {
            ImGui::SameLine();
            if (ImGui::Button("Capture Center From Map"))
                m_zoneCaptureMode = ZoneCaptureMode::CircleCenter;
            ImGui::SameLine();
            if (ImGui::Button("Capture Radius From Map"))
                m_zoneCaptureMode = ZoneCaptureMode::CircleRadius;
        } else {
            ImGui::SameLine();
            if (ImGui::Button(m_zoneCaptureMode == ZoneCaptureMode::PolygonVertex ? "Stop Polygon Capture" : "Add Polygon Vertices")) {
                m_zoneCaptureMode = (m_zoneCaptureMode == ZoneCaptureMode::PolygonVertex)
                    ? ZoneCaptureMode::None
                    : ZoneCaptureMode::PolygonVertex;
                m_editDragVertex = -1;
            }
            ImGui::SameLine();
            if (ImGui::Button("Clear Polygon")) {
                settings.zonePolygon.clear();
                m_editDragVertex = -1;
            }
            ImGui::Text("Polygon Vertices: %d", (int)settings.zonePolygon.size());

            if (!settings.zonePolygon.empty()) {
                int removeIdx = -1;
                if (ImGui::BeginTable("##polyvertices", 3,
                        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                        ImVec2(0, 120.0f))) {
                    ImGui::TableSetupScrollFreeze(0, 1);
                    ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 30.0f);
                    ImGui::TableSetupColumn("Position", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("##del", ImGuiTableColumnFlags_WidthFixed, 30.0f);
                    ImGui::TableHeadersRow();

                    for (size_t i = 0; i < settings.zonePolygon.size(); i++) {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::Text("%d", (int)i + 1);
                        ImGui::TableNextColumn();
                        char label[32];
                        snprintf(label, sizeof(label), "##vtx%d", (int)i);
                        ImGui::SetNextItemWidth(-1);
                        ImGui::InputInt2(label, &settings.zonePolygon[i].x);
                        ImGui::TableNextColumn();
                        if (settings.zonePolygon.size() > 3) {
                            char btnId[32];
                            snprintf(btnId, sizeof(btnId), "X##delvtx%d", (int)i);
                            if (ImGui::SmallButton(btnId))
                                removeIdx = (int)i;
                        }
                    }
                    ImGui::EndTable();
                }
                if (removeIdx >= 0) {
                    settings.zonePolygon.erase(settings.zonePolygon.begin() + removeIdx);
                    if (m_editDragVertex == removeIdx)
                        m_editDragVertex = -1;
                    else if (m_editDragVertex > removeIdx)
                        m_editDragVertex--;
                }
            }
        }

        const char* captureText = "None";
        switch (m_zoneCaptureMode) {
            case ZoneCaptureMode::CircleCenter: captureText = "Click map for zone center"; break;
            case ZoneCaptureMode::CircleRadius: captureText = "Click map for zone radius"; break;
            case ZoneCaptureMode::PolygonVertex: captureText = "Click map to add polygon vertices"; break;
            case ZoneCaptureMode::None: break;
        }
        ImGui::TextDisabled("%s", captureText);
    }

    if (ImGui::CollapsingHeader("Safety", kSectionFlags)) {
        ImGui::InputText("Player Whitelist", settings.playerWhitelist, IM_ARRAYSIZE(settings.playerWhitelist));
        ImGui::TextDisabled("Comma-separated. Whitelisted players are ignored by all player-nearby checks.");
        ImGui::Checkbox("Player Nearby Safety", &settings.safetyEnabled);
        if (settings.safetyEnabled) {
            ImGui::SliderInt("Detection Range", &settings.safetyPlayerRange, 0, 30);
            if (settings.safetyPlayerRange == 0)
                ImGui::TextDisabled("0 = any player on the map triggers safety.");
            ImGui::SliderInt("Detection Time (s)", &settings.safetyDetectionSec, 5, 300);
            ImGui::SliderInt("Rest Time (s)", &settings.safetyRestSec, 10, 600);
            ImGui::Checkbox("Discord Notify on Safety Trigger", &settings.safetyNotifyDiscord);
            if (!m_nearbyPlayerTicks.empty()) {
                DWORD now = GetTickCount();
                for (auto& [id, tick] : m_nearbyPlayerTicks) {
                    int elapsed = (int)((now - tick) / 1000);
                    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                        "Player %u nearby for %ds / %ds", id, elapsed, settings.safetyDetectionSec);
                }
            }
            if (m_safetyResting) {
                DWORD elapsed = GetTickCount() - m_safetyRestStartTick;
                int remaining = settings.safetyRestSec - (int)(elapsed / 1000);
                if (remaining < 0) remaining = 0;
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                    "Safety resting in Market (%ds remaining)", remaining);
            }
        }
    }

    if (ImGui::CollapsingHeader("Monsters", kSectionFlags)) {
        ImGui::InputText("Monster Names", settings.monsterNames, IM_ARRAYSIZE(settings.monsterNames));
        ImGui::InputText("Ignore Monster Names", settings.monsterIgnoreNames, IM_ARRAYSIZE(settings.monsterIgnoreNames));
        ImGui::InputText("Prefer Monster Names", settings.monsterPreferNames, IM_ARRAYSIZE(settings.monsterPreferNames));
        ImGui::SliderInt("Mob Search Range", &settings.mobSearchRange, 0, CGameMap::MAX_JUMP_DIST);
        ImGui::TextDisabled("All lists are comma-separated. Ignore rules win over target rules.");
        ImGui::TextDisabled("Prefer targets matching monsters first; falls back to others if none found.");
        ImGui::TextDisabled("Mob Search Range limits target selection to mobs within N tiles (0 = unlimited).");

        if (mgr && ImGui::TreeNode("Nearby Monster Names")) {
            std::vector<std::string> names;
            std::unordered_set<std::string> seen;
            for (size_t i = 0; i < mgr->m_deqRole.size() && i < 500; ++i) {
                const auto& roleRef = mgr->m_deqRole[i];
                if (!roleRef || !roleRef->IsMonster())
                    continue;

                const std::string name = roleRef->GetName();
                if (seen.insert(name).second)
                    names.push_back(name);
            }
            std::sort(names.begin(), names.end());

            for (const std::string& name : names) {
                ImGui::PushID(name.c_str());
                ImGui::TextUnformatted(name.c_str());
                ImGui::SameLine(180.0f);
                if (ImGui::SmallButton("Target"))
                    AppendFilterToken(settings.monsterNames, sizeof(settings.monsterNames), name.c_str());
                ImGui::SameLine();
                if (ImGui::SmallButton("Ignore"))
                    AppendFilterToken(settings.monsterIgnoreNames, sizeof(settings.monsterIgnoreNames), name.c_str());
                ImGui::SameLine();
                if (ImGui::SmallButton("Prefer"))
                    AppendFilterToken(settings.monsterPreferNames, sizeof(settings.monsterPreferNames), name.c_str());
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
    }

    if (ImGui::CollapsingHeader("Timings", kSectionFlags)) {
        ImGui::SliderInt("Movement Interval (ms)", &settings.movementIntervalMs, kMinMovementIntervalMs, kMaxMovementIntervalMs);
        ImGui::SliderInt("Attack Interval (ms)", &settings.attackIntervalMs, kMinAttackIntervalMs, kMaxAttackIntervalMs);
        ImGui::SliderInt("Cyclone Attack Interval (ms)", &settings.cycloneAttackIntervalMs, kMinAttackIntervalMs, kMaxAttackIntervalMs);
        ImGui::SliderInt("Target Switch Delay (ms)", &settings.targetSwitchAttackIntervalMs,
            kMinTargetSwitchAttackIntervalMs, kMaxTargetSwitchAttackIntervalMs);
        ImGui::SliderInt("Item Action Interval (ms)", &settings.itemActionIntervalMs, kMinItemActionIntervalMs, kMaxItemActionIntervalMs);
        ImGui::SliderInt("Loot Spawn Grace (ms)", &settings.lootSpawnGraceMs, kMinLootSpawnGraceMs, kMaxLootSpawnGraceMs);
        ImGui::SliderInt("Self Cast Interval (ms)", &settings.selfCastIntervalMs, kMinSelfCastIntervalMs, kMaxSelfCastIntervalMs);
        ImGui::SliderInt("Town/NPC Action Interval (ms)", &settings.npcActionIntervalMs, kMinNpcActionIntervalMs, kMaxNpcActionIntervalMs);
        ImGui::SliderInt("Loot Retry Ignore (ms)", &settings.lootPickupIgnoreMs, kMinLootPickupIgnoreMs, kMaxLootPickupIgnoreMs);
        ImGui::SliderInt("Revive Delay (ms)", &settings.reviveDelayMs, kMinReviveDelayMs, kMaxReviveDelayMs);
        ImGui::SliderInt("Revive Retry Interval (ms)", &settings.reviveRetryIntervalMs,
            kMinReviveRetryIntervalMs, kMaxReviveRetryIntervalMs);
        ImGui::TextDisabled("Lower intervals act faster but send actions more aggressively.");
        ImGui::TextDisabled("Loot Spawn Grace waits after a ground item first appears before sending pickup.");
    }

    if (ImGui::CollapsingHeader("Loot Rules", kSectionFlags)) {
        ImGui::SliderInt("Loot Range", &settings.lootRange, 0, CGameMap::MAX_JUMP_DIST);
        ImGui::SliderInt("Minimum Loot Plus", &settings.minimumLootPlus, 0, 12);
        ImGui::TextDisabled("Items in the Loot Items list are always looted. Other drops are only looted if confirmed by a system message (our kill).");
        ImGui::Text("Loot by quality:");
        ImGui::Checkbox("Refined##basehuntlootqualityrefined", &settings.lootRefined);
        ImGui::SameLine();
        ImGui::Checkbox("Unique##basehuntlootqualityunique", &settings.lootUnique);
        ImGui::SameLine();
        ImGui::Checkbox("Elite##basehuntlootqualityelite", &settings.lootElite);
        ImGui::SameLine();
        ImGui::Checkbox("Super##basehuntlootqualitysuper", &settings.lootSuper);
        ImGui::TextDisabled("Auto hunt loots items selected in the browser, checked qualities, or items meeting Minimum Loot Plus.");
        ImGui::TextDisabled("Pickup packets wait until the hero is settled on the loot tile to avoid range errors.");
    }

    if (ImGui::CollapsingHeader("Warehouse Rules", kSectionFlags)) {
        ImGui::SliderInt("Store When Bag >=", &settings.bagStoreThreshold, 1, CHero::MAX_BAG_ITEMS);
        ImGui::SliderInt("Minimum Warehouse Plus", &settings.minimumStorePlus, 0, 12);
        ImGui::Checkbox("Immediate Return On Priority Items", &settings.immediateReturnOnPriorityItems);
        ImGui::Checkbox("Pack Meteors into MeteorScrolls", &settings.packMeteorsIntoScrolls);
        ImGui::Checkbox("Use TreasureBank for DB/Meteor items", &settings.storeTreasureBank);
        ImGui::Checkbox("Use ComposeBank for +1/+2 gear", &settings.storeComposeBank);
        ImGui::Checkbox("Deposit Silver", &settings.autoDepositSilver);
        if (settings.autoDepositSilver)
            ImGui::InputInt("Silver to Keep", &settings.silverKeepAmount, 1000, 10000);
        ImGui::Text("Store Quality:");
        ImGui::SameLine();
        ImGui::Checkbox("Refined##storeQuality", &settings.storeRefined);
        ImGui::SameLine();
        ImGui::Checkbox("Unique##storeQuality", &settings.storeUnique);
        ImGui::SameLine();
        ImGui::Checkbox("Elite##storeQuality", &settings.storeElite);
        ImGui::SameLine();
        ImGui::Checkbox("Super##storeQuality", &settings.storeSuper);
        ImGui::Checkbox("Buy Arrows", &settings.buyArrows);
        if (settings.buyArrows) {
            int arrowTypeIdInt = (int)settings.arrowTypeId;
            if (ImGui::InputInt("Arrow Type ID", &arrowTypeIdInt, 1, 100))
                settings.arrowTypeId = arrowTypeIdInt > 0 ? (uint32_t)arrowTypeIdInt : 0;
            if (settings.arrowTypeId != 0) {
                if (const ItemTypeInfo* info = GetItemTypeInfo(settings.arrowTypeId))
                    ImGui::TextDisabled("  -> %s", info->name.c_str());
                else
                    ImGui::TextDisabled("  -> (unknown type)");
            }
            ImGui::SliderInt("Arrow Packs to Maintain", &settings.arrowBuyCount, 1, 10);
        }
        ImGui::TextDisabled("Priority return items trigger an immediate Market run and are treated as warehouse-store items.");
        ImGui::TextDisabled("Meteor packing runs during storage until fewer than 10 Meteors remain.");
        ImGui::TextDisabled("TreasureBank stores DragonBalls, Meteors, DBScrolls, and MeteorScrolls.");
        ImGui::TextDisabled("ComposeBank stores bagged wearable gear with +1 or +2.");
    }

    if (ImGui::CollapsingHeader("Item Lists", kSectionFlags))
        RenderItemSelector(settings);

    if (ImGui::CollapsingHeader("Runtime", kSectionFlags)) {
        ImGui::Text("State: %s", GetStateName());
        ImGui::Text("Status: %s", m_statusText);
        ImGui::Text("Map: %u", m_lastMapId);
        ImGui::Text("Hero Pos: (%d, %d)", m_lastHeroPos.x, m_lastHeroPos.y);
        if (m_targetId != 0)
            ImGui::Text("Active Target: %u", m_targetId);

        const int hpPercent = m_lastMaxHp > 0 ? (m_lastHp * 100) / m_lastMaxHp : 0;
        const int manaPercent = m_lastMaxMana > 0 ? (m_lastMana * 100) / m_lastMaxMana : 0;
        ImGui::Text("HP: %d / %d (%d%%)", m_lastHp, m_lastMaxHp, hpPercent);
        ImGui::Text("Mana: %d / %d (%d%%)", m_lastMana, m_lastMaxMana, manaPercent);
        ImGui::Text("Bag Items: %d / %d", (int)m_lastBagCount, CHero::MAX_BAG_ITEMS);
        ImGui::Text("XP Ready: %s", m_buffMgr.IsXpSkillReady() ? "Yes" : "No");
        ImGui::Text("Superman Active: %s", m_buffMgr.IsSupermanActive() ? "Yes" : "No");
        ImGui::Text("Cyclone Active: %s", m_buffMgr.IsCycloneActive() ? "Yes" : "No");
        ImGui::Text("Fly Active: %s", m_buffMgr.IsFlyActive() ? "Yes" : "No");
        ImGui::Text("Scatter Range: %d", GetLastScatterRange());
        ImGui::Text("Tracked Mob Clump: %d", m_lastClumpSize);
    }

    if (ImGui::CollapsingHeader("Debug Overlays", kSectionFlags)) {
        ImGui::TextDisabled("Show radius circles on the minimap centered on the hero.");
        ImGui::Checkbox("Action Radius##debug", &settings.debugShowActionRadius);
        ImGui::Checkbox("Clump Radius##debug", &settings.debugShowClumpRadius);
        ImGui::Checkbox("Mob Search Range##debug", &settings.debugShowMobSearchRange);
        ImGui::Checkbox("Loot Range##debug", &settings.debugShowLootRange);
        ImGui::Checkbox("Safety Player Range##debug", &settings.debugShowSafetyRange);
        ImGui::Checkbox("Attack Range##debug", &settings.debugShowAttackRange);
        ImGui::Checkbox("Archer Safety Distance##debug", &settings.debugShowArcherSafety);
        ImGui::Checkbox("Scatter Range##debug", &settings.debugShowScatterRange);
        ImGui::Checkbox("Best Mob Clump##debug", &settings.debugShowBestClump);
    }
}

void BaseHuntPlugin::RenderUI()
{
    AutoHuntSettings& settings = GetAutoHuntSettings();

    // Mutual exclusion: enabling this plugin disables the other hunt plugin
    bool wasEnabled = m_enabled;
    ImGui::Checkbox("Enabled", &m_enabled);
    if (m_enabled && !wasEnabled) {
        for (auto& p : PluginManager::Get().GetPlugins()) {
            auto* other = dynamic_cast<BaseHuntPlugin*>(p.get());
            if (other && other != this && other->m_enabled) {
                other->m_enabled = false;
                other->StopAutomation(true);
            }
        }
    }
    ImGui::Separator();

    RenderCombatUI(settings);
}
