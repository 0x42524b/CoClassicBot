#include "archer_hunt_plugin.h"
#include "hunt_targeting.h"
#include "game.h"
#include "CHero.h"
#include "CGameMap.h"
#include "CRole.h"
#include "CMagic.h"
#include "CItem.h"
#include "config.h"
#include "itemtype.h"
#include "pathfinder.h"
#include "log.h"
#include "imgui.h"
#include <algorithm>
#include <limits>
#include <vector>

namespace {

constexpr int kReliableAttackRange = 1;
constexpr int kMinMobClumpSize = 2;
constexpr int kArcherSafetyBufferTiles = 1;
constexpr DWORD kArcherRetreatHoldMs = 3000;
constexpr DWORD kFailedRetreatDestAvoidMs = 1000;
constexpr int kMinAttackIntervalMs = 25;
constexpr int kMaxAttackIntervalMs = 5000;
constexpr int kMinTargetSwitchAttackIntervalMs = 0;
constexpr int kMaxTargetSwitchAttackIntervalMs = 5000;
constexpr int kMinItemActionIntervalMs = 100;
constexpr int kMaxItemActionIntervalMs = 5000;

DWORD ClampMs(int value, int minValue, int maxValue)
{
    return static_cast<DWORD>(std::clamp(value, minValue, maxValue));
}

DWORD GetAttackIntervalMs(const AutoHuntSettings& settings)
{
    return ClampMs(settings.attackIntervalMs, kMinAttackIntervalMs, kMaxAttackIntervalMs);
}

DWORD GetCycloneAttackIntervalMs(const AutoHuntSettings& settings)
{
    return ClampMs(settings.cycloneAttackIntervalMs, kMinAttackIntervalMs, kMaxAttackIntervalMs);
}

DWORD GetTargetSwitchAttackIntervalMs(const AutoHuntSettings& settings)
{
    return ClampMs(settings.targetSwitchAttackIntervalMs,
        kMinTargetSwitchAttackIntervalMs, kMaxTargetSwitchAttackIntervalMs);
}

DWORD GetItemActionIntervalMs(const AutoHuntSettings& settings)
{
    return ClampMs(settings.itemActionIntervalMs, kMinItemActionIntervalMs, kMaxItemActionIntervalMs);
}

bool TickIsFuture(DWORD targetTick, DWORD now)
{
    return static_cast<int32_t>(targetTick - now) > 0;
}

int GridDistance(const Position& a, const Position& b)
{
    return (std::max)(std::abs(a.x - b.x), std::abs(a.y - b.y));
}

bool IsArcherModeEnabled(const AutoHuntSettings& settings)
{
    return settings.archerMode || settings.combatMode == AutoHuntCombatMode::Archer;
}

bool IsScatterLogicEnabled(const AutoHuntSettings& settings)
{
    return IsArcherModeEnabled(settings) && settings.useScatterLogic;
}

int GetRegularArcherAttackRange(const AutoHuntSettings& settings)
{
    return (std::max)(0, settings.rangedAttackRange);
}

int GetArcherSafetyDistance(const AutoHuntSettings& settings)
{
    if (!IsArcherModeEnabled(settings))
        return 0;
    CHero* hero = Game::GetHero();
    if (hero && hero->IsFlyActive())
        return 0;
    return (std::max)(0, settings.archerSafetyDistance);
}

int GetRequiredArcherThreatDistance(int safetyDist)
{
    return safetyDist > 0 ? (safetyDist + kArcherSafetyBufferTiles) : 0;
}

} // anonymous namespace


// =============================================================================
// Scatter geometry
// =============================================================================

CMagic* ArcherHuntPlugin::FindScatterMagic(const CHero* hero) const
{
    if (!hero)
        return nullptr;

    CMagic* best = nullptr;
    for (const auto& magicRef : hero->m_vecMagic) {
        CMagic* magic = magicRef.get();
        if (!magic || !magic->IsEnabled())
            continue;

        const char* name = magic->GetName();
        if (!name || _strnicmp(name, "Scatter", 7) != 0)
            continue;

        if (!best || magic->GetLevel() > best->GetLevel())
            best = magic;
    }

    return best;
}

int ArcherHuntPlugin::GetScatterRange(const CHero* hero) const
{
    const CMagic* scatter = FindScatterMagic(hero);
    if (!scatter)
        return 0;

    const auto& settings = GetAutoHuntSettings();
    if (settings.scatterRangeOverride > 0)
        return settings.scatterRangeOverride;

    return (int)(std::max)(scatter->GetRange(), scatter->GetDistance());
}

bool ArcherHuntPlugin::IsTargetInScatterSector(
    const Position& origin, const Position& castPos, const Position& targetPos, int range) const
{
    if (range <= 0)
        return false;
    if (castPos.x == origin.x && castPos.y == origin.y)
        return false;
    if (GridDistance(origin, targetPos) > range)
        return false;

    const double dirX = (double)(castPos.x - origin.x);
    const double dirY = (double)(castPos.y - origin.y);
    const double targetX = (double)(targetPos.x - origin.x);
    const double targetY = (double)(targetPos.y - origin.y);
    if ((targetX == 0.0 && targetY == 0.0) || (dirX == 0.0 && dirY == 0.0))
        return false;

    // Scatter is a 180-degree frontal sector, so a non-negative dot product
    // keeps targets inside the facing half-plane.
    return (dirX * targetX + dirY * targetY) >= 0.0;
}

bool ArcherHuntPlugin::FindBestScatterShot(const std::vector<CRole*>& targets, const Position& origin, int range,
    int minimumHits, Position& outCastPos, CRole*& outPrimaryTarget, int& outHitCount) const
{
    outCastPos = {};
    outPrimaryTarget = nullptr;
    outHitCount = 0;

    if (range <= 0 || targets.empty())
        return false;

    const int requiredHits = (std::max)(1, minimumHits);
    bool found = false;
    int bestPrimaryDist = (std::numeric_limits<int>::max)();
    float bestCastDist = (std::numeric_limits<float>::max)();

    for (CRole* directionTarget : targets) {
        if (!directionTarget)
            continue;

        const Position candidateCastPos = directionTarget->m_posMap;
        if (candidateCastPos.x == origin.x && candidateCastPos.y == origin.y)
            continue;
        if (GridDistance(origin, candidateCastPos) > range)
            continue;

        int hitCount = 0;
        CRole* primaryTarget = nullptr;
        int primaryDist = (std::numeric_limits<int>::max)();
        for (CRole* target : targets) {
            if (!target || !IsTargetInScatterSector(origin, candidateCastPos, target->m_posMap, range))
                continue;

            ++hitCount;
            const int targetDist = GridDistance(origin, target->m_posMap);
            if (!primaryTarget || targetDist < primaryDist) {
                primaryTarget = target;
                primaryDist = targetDist;
            }
        }

        if (hitCount < requiredHits || !primaryTarget)
            continue;

        const float castDist = origin.DistanceTo(candidateCastPos);
        if (!found
            || hitCount > outHitCount
            || (hitCount == outHitCount && primaryDist < bestPrimaryDist)
            || (hitCount == outHitCount && primaryDist == bestPrimaryDist && castDist < bestCastDist)) {
            found = true;
            outCastPos = candidateCastPos;
            outPrimaryTarget = primaryTarget;
            outHitCount = hitCount;
            bestPrimaryDist = primaryDist;
            bestCastDist = castDist;
        }
    }

    return found;
}

bool ArcherHuntPlugin::FindBestScatterApproach(CHero* hero, CGameMap* map, const AutoHuntSettings& settings,
    const std::vector<CRole*>& targets, int range, int minimumHits, Position& outApproachPos,
    Position& outCastPos, CRole*& outPrimaryTarget, int& outHitCount,
    bool preferFarTiles) const
{
    outApproachPos = {};
    outCastPos = {};
    outPrimaryTarget = nullptr;
    outHitCount = 0;

    if (!hero || !map || range <= 0 || !IsArcherModeEnabled(settings))
        return false;

    const Position effectivePos = GetEffectiveHeroPosition(hero);
    const int localTargetRadius = range + CGameMap::MAX_JUMP_DIST + 2;
    std::vector<CRole*> localTargets;
    localTargets.reserve(targets.size());
    for (CRole* target : targets) {
        if (target && GridDistance(effectivePos, target->m_posMap) <= localTargetRadius)
            localTargets.push_back(target);
    }

    if (localTargets.empty())
        return false;

    const Position jumpOrigin = GetEffectiveHeroPosition(hero);
    const int safetyDist = GetArcherSafetyDistance(settings);
    const int requiredThreatDist = GetRequiredArcherThreatDistance(safetyDist);
    bool found = false;
    int bestMinThreatDist = -1;
    float bestMoveDist = preferFarTiles ? 0.0f : (std::numeric_limits<float>::max)();
    float bestCastDist = (std::numeric_limits<float>::max)();

    for (int dx = -CGameMap::MAX_JUMP_DIST; dx <= CGameMap::MAX_JUMP_DIST; ++dx) {
        for (int dy = -CGameMap::MAX_JUMP_DIST; dy <= CGameMap::MAX_JUMP_DIST; ++dy) {
            const Position candidate = {jumpOrigin.x + dx, jumpOrigin.y + dy};
            if (candidate.x == jumpOrigin.x && candidate.y == jumpOrigin.y)
                continue;
            if (!IsPointInZone(settings, settings.zoneMapId, candidate))
                continue;
            if (!map->IsWalkable(candidate.x, candidate.y))
                continue;
            if (IsTileOccupied(candidate.x, candidate.y))
                continue;
            if (!map->CanJump(jumpOrigin.x, jumpOrigin.y, candidate.x, candidate.y, CGameMap::GetHeroAltThreshold()))
                continue;
            int minThreatDist = (std::numeric_limits<int>::max)();
            if (requiredThreatDist > 0) {
                for (CRole* t : localTargets) {
                    if (!t)
                        continue;
                    const int threatDist = CGameMap::TileDist(candidate.x, candidate.y, t->m_posMap.x, t->m_posMap.y);
                    if (threatDist < minThreatDist)
                        minThreatDist = threatDist;
                }
                if (minThreatDist < requiredThreatDist)
                    continue;
            }

            Position candidateCastPos = {};
            CRole* candidatePrimaryTarget = nullptr;
            int candidateHitCount = 0;
            if (!FindBestScatterShot(localTargets, candidate, range, minimumHits,
                    candidateCastPos, candidatePrimaryTarget, candidateHitCount)) {
                continue;
            }

            const float moveDist = jumpOrigin.DistanceTo(candidate);
            const float castDist = candidate.DistanceTo(candidateCastPos);
            const bool betterMove = preferFarTiles ? (moveDist > bestMoveDist) : (moveDist < bestMoveDist);
            if (!found
                || candidateHitCount > outHitCount
                || (candidateHitCount == outHitCount && minThreatDist > bestMinThreatDist)
                || (candidateHitCount == outHitCount && minThreatDist == bestMinThreatDist && betterMove)
                || (candidateHitCount == outHitCount && moveDist == bestMoveDist && castDist < bestCastDist)) {
                found = true;
                outApproachPos = candidate;
                outCastPos = candidateCastPos;
                outPrimaryTarget = candidatePrimaryTarget;
                outHitCount = candidateHitCount;
                bestMinThreatDist = minThreatDist;
                bestMoveDist = moveDist;
                bestCastDist = castDist;
            }
        }
    }

    return found;
}


// =============================================================================
// Retreat
// =============================================================================

bool ArcherHuntPlugin::FindSafeArcherRetreat(CHero* hero, CGameMap* map, const AutoHuntSettings& settings,
    const std::vector<CRole*>& threats, CRole* target, Position& outRetreatPos,
    int safetyDistOverride) const
{
    outRetreatPos = {};
    const int safetyDist = safetyDistOverride > 0 ? safetyDistOverride : GetArcherSafetyDistance(settings);
    const int requiredThreatDist = GetRequiredArcherThreatDistance(safetyDist);
    if (!hero || !map || requiredThreatDist <= 0 || threats.empty())
        return false;

    const Position heroPos = GetEffectiveHeroPosition(hero);
    const int attackRange = GetRegularArcherAttackRange(settings);
    const DWORD now = GetTickCount();
    const bool avoidFailedDest = m_lastFailedRetreatTick != 0
        && (now - m_lastFailedRetreatTick) < kFailedRetreatDestAvoidMs;

    bool found = false;
    bool bestSafe = false;
    bool bestInRange = false;
    int bestMinThreatDist = 0;
    float bestMoveDist = (std::numeric_limits<float>::max)();

    for (int dx = -CGameMap::MAX_JUMP_DIST; dx <= CGameMap::MAX_JUMP_DIST; ++dx) {
        for (int dy = -CGameMap::MAX_JUMP_DIST; dy <= CGameMap::MAX_JUMP_DIST; ++dy) {
            const Position candidate = {heroPos.x + dx, heroPos.y + dy};
            if (candidate.x == heroPos.x && candidate.y == heroPos.y)
                continue;
            if (avoidFailedDest
                && candidate.x == m_lastFailedRetreatDest.x
                && candidate.y == m_lastFailedRetreatDest.y) {
                continue;
            }
            if (CGameMap::TileDist(heroPos.x, heroPos.y, candidate.x, candidate.y) > CGameMap::MAX_JUMP_DIST)
                continue;
            if (!IsPointInZone(settings, settings.zoneMapId, candidate))
                continue;
            if (!map->IsWalkable(candidate.x, candidate.y))
                continue;
            if (IsTileOccupied(candidate.x, candidate.y))
                continue;
            if (!map->CanJump(heroPos.x, heroPos.y, candidate.x, candidate.y, CGameMap::GetHeroAltThreshold()))
                continue;

            int minThreatDist = (std::numeric_limits<int>::max)();
            for (CRole* t : threats) {
                if (!t) continue;
                const int d = CGameMap::TileDist(candidate.x, candidate.y, t->m_posMap.x, t->m_posMap.y);
                if (d < minThreatDist)
                    minThreatDist = d;
            }

            const bool isSafe = minThreatDist >= requiredThreatDist;
            const bool inAttackRange = target && attackRange > 0
                && CGameMap::TileDist(candidate.x, candidate.y, target->m_posMap.x, target->m_posMap.y) <= attackRange;
            const float moveDist = heroPos.DistanceTo(candidate);

            const bool better = !found
                || (isSafe && !bestSafe)
                || (isSafe == bestSafe && inAttackRange && !bestInRange)
                || (isSafe == bestSafe && inAttackRange == bestInRange && minThreatDist > bestMinThreatDist)
                || (isSafe == bestSafe && inAttackRange == bestInRange && minThreatDist == bestMinThreatDist && moveDist < bestMoveDist);

            if (better) {
                found = true;
                outRetreatPos = candidate;
                bestSafe = isSafe;
                bestInRange = inAttackRange;
                bestMinThreatDist = minThreatDist;
                bestMoveDist = moveDist;
            }
        }
    }

    return found;
}

bool ArcherHuntPlugin::TryArcherDangerRetreat(CHero* hero, CGameMap* map, const AutoHuntSettings& settings, CRole* target)
{
    if (!hero || !map)
        return false;

    if (m_buffMgr.IsPreLandingRetreat()) {
        spdlog::trace("[hunt] Retreat skip: preLandingRetreat");
        return false;
    }

    const int safetyDist = GetArcherSafetyDistance(settings);
    const int requiredThreatDist = GetRequiredArcherThreatDistance(safetyDist);
    if (requiredThreatDist <= 0)
        return false;

    const DWORD now = GetTickCount();
    if (hero->IsJumping()) {
        spdlog::trace("[hunt] Retreat skip: IsJumping pos=({},{})", hero->m_posMap.x, hero->m_posMap.y);
        return false;
    }
    if (UpdatePendingJumpState(hero, now)) {
        spdlog::trace("[hunt] Retreat skip: pendingJump pos=({},{}) dest=({},{}) age={}ms",
            hero->m_posMap.x, hero->m_posMap.y,
            m_pendingJumpDest.x, m_pendingJumpDest.y, now - m_pendingJumpTick);
        return false;
    }
    if (m_retreatCooldownTick != 0 && now < m_retreatCooldownTick) {
        spdlog::trace("[hunt] Retreat skip: cooldown {}ms remaining", m_retreatCooldownTick - now);
        return false;
    }
    m_retreatCooldownTick = 0;

    const std::vector<CRole*> nearbyTargets = CollectHuntTargets(settings);
    const Position heroPos = GetEffectiveHeroPosition(hero);
    int closestDist = 9999;
    for (CRole* t : nearbyTargets) {
        if (!t)
            continue;
        const int d = CGameMap::TileDist(heroPos.x, heroPos.y, t->m_posMap.x, t->m_posMap.y);
        if (d < closestDist)
            closestDist = d;
    }
    if (closestDist >= requiredThreatDist)
        return false;

    spdlog::debug("[hunt] THREATENED closestMob={} safetyDist={} pos=({},{}) pendingJump={}",
        closestDist, safetyDist, heroPos.x, heroPos.y, m_pendingJumpTick);

    auto urgentRetreatJump = [&](const Position& dest, const char* reason) {
        if (Pathfinder::Get().IsActive())
            Pathfinder::Get().Stop();
        const DWORD jumpNow = GetTickCount();
        spdlog::debug("[hunt] Retreat JUMP ({},{}) -> ({},{}) reason={}",
            hero->m_posMap.x, hero->m_posMap.y, dest.x, dest.y, reason);
        hero->Jump(dest.x, dest.y);
        m_lastMoveTick = jumpNow;
        ArmPendingJump(hero, dest, jumpNow, true);
        SetState(AutoHuntState::ApproachTarget, reason);
    };

    Position retreatPos = {};
    const bool hasSafeRetreat = FindSafeArcherRetreat(hero, map, settings, nearbyTargets, target, retreatPos);
    if (hasSafeRetreat && IsScatterLogicEnabled(settings)) {
        const int scatterRange = GetScatterRange(hero);
        const int minHits = (std::max)(1, settings.minimumScatterHits);
        if (scatterRange > 0) {
            Position scatterPos = {};
            Position scatterCast = {};
            CRole* scatterTarget = nullptr;
            int scatterHits = 0;
            if (FindBestScatterApproach(hero, map, settings, nearbyTargets,
                    scatterRange, minHits, scatterPos, scatterCast,
                    scatterTarget, scatterHits, /*preferFarTiles=*/true)) {
                int scatterMinThreat = (std::numeric_limits<int>::max)();
                int retreatMinThreat = (std::numeric_limits<int>::max)();
                for (CRole* t : nearbyTargets) {
                    if (!t)
                        continue;
                    const int sd = CGameMap::TileDist(scatterPos.x, scatterPos.y, t->m_posMap.x, t->m_posMap.y);
                    const int rd = CGameMap::TileDist(retreatPos.x, retreatPos.y, t->m_posMap.x, t->m_posMap.y);
                    if (sd < scatterMinThreat)
                        scatterMinThreat = sd;
                    if (rd < retreatMinThreat)
                        retreatMinThreat = rd;
                }
                if (scatterMinThreat >= retreatMinThreat) {
                    spdlog::debug("[hunt] Scatter retreat tile ({},{}) minThreat={} >= safe ({},{}) minThreat={}, hits={}",
                        scatterPos.x, scatterPos.y, scatterMinThreat,
                        retreatPos.x, retreatPos.y, retreatMinThreat, scatterHits);
                    urgentRetreatJump(scatterPos, "Retreating to scatter position");
                    return true;
                }

                spdlog::debug("[hunt] Scatter tile ({},{}) minThreat={} < safe ({},{}) minThreat={}, preferring safety",
                    scatterPos.x, scatterPos.y, scatterMinThreat,
                    retreatPos.x, retreatPos.y, retreatMinThreat);
            }
        }
    }

    if (hasSafeRetreat) {
        urgentRetreatJump(retreatPos, "Retreating to safe distance");
        return true;
    }

    spdlog::debug("[hunt] FindSafeArcherRetreat FAILED targets={}", nearbyTargets.size());
    return false;
}


// =============================================================================
// Target finding
// =============================================================================

CRole* ArcherHuntPlugin::FindBestArcherTarget(CHero* hero, CGameMap* map, const AutoHuntSettings& settings,
    Position* outApproachPos, Position* outAttackPos, int* outClumpSize, bool* outUseScatter) const
{
    if (outApproachPos)
        *outApproachPos = {};
    if (outAttackPos)
        *outAttackPos = {};
    if (outClumpSize)
        *outClumpSize = 0;
    if (outUseScatter)
        *outUseScatter = false;
    if (!hero || !map)
        return nullptr;

    const bool hasPreferFilter = settings.monsterPreferNames[0] != '\0';
    std::vector<CRole*> targets = hasPreferFilter ? CollectHuntTargets(settings, true) : std::vector<CRole*>{};
    if (targets.empty())
        targets = CollectHuntTargets(settings);
    if (targets.empty())
        return nullptr;

    const int regularAttackRange = GetRegularArcherAttackRange(settings);
    const int scatterRange = GetScatterRange(hero);
    const int minimumScatterHits = (std::max)(1, settings.minimumScatterHits);
    const Position effectivePos = GetEffectiveHeroPosition(hero);
    const DWORD now = GetTickCount();
    const bool retreatHoldActive = TickIsFuture(m_retreatHoldUntilTick, now);
    if (IsScatterLogicEnabled(settings) && settings.prioritizeScatterClumps && scatterRange > 0) {
        Position scatterCastPos = {};
        CRole* scatterTarget = nullptr;
        int scatterHits = 0;
        const bool hasLocalShot = FindBestScatterShot(targets, effectivePos, scatterRange, minimumScatterHits,
                scatterCastPos, scatterTarget, scatterHits);

        Position scatterApproachPos = {};
        Position scatterApproachCastPos = {};
        CRole* scatterApproachTarget = nullptr;
        int scatterApproachHits = 0;
        bool hasApproachShot = false;
        const bool shouldSearchApproach = !retreatHoldActive
            && (!hasLocalShot || (now - m_lastScatterApproachTick >= 500));
        if (shouldSearchApproach) {
            m_lastScatterApproachTick = now;
            hasApproachShot = FindBestScatterApproach(hero, map, settings, targets, scatterRange, minimumScatterHits,
                    scatterApproachPos, scatterApproachCastPos, scatterApproachTarget, scatterApproachHits);
        }

        if (hasApproachShot && scatterApproachHits > scatterHits) {
            if (outApproachPos)
                *outApproachPos = scatterApproachPos;
            if (outAttackPos)
                *outAttackPos = scatterApproachCastPos;
            if (outClumpSize)
                *outClumpSize = scatterApproachHits;
            if (outUseScatter)
                *outUseScatter = true;
            return scatterApproachTarget;
        }

        if (hasLocalShot) {
            if (outAttackPos)
                *outAttackPos = scatterCastPos;
            if (outClumpSize)
                *outClumpSize = scatterHits;
            if (outUseScatter)
                *outUseScatter = true;
            return scatterTarget;
        }

        if (hasApproachShot) {
            if (outApproachPos)
                *outApproachPos = scatterApproachPos;
            if (outAttackPos)
                *outAttackPos = scatterApproachCastPos;
            if (outClumpSize)
                *outClumpSize = scatterApproachHits;
            if (outUseScatter)
                *outUseScatter = true;
            return scatterApproachTarget;
        }
    }

    // Scatter logic is on but no valid scatter shot was found — roam the zone
    if (IsScatterLogicEnabled(settings) && scatterRange > 0)
        return nullptr;

    // If there's already a target within attack range, hit it
    const int effectiveAttackRange = regularAttackRange > 0 ? regularAttackRange : kReliableAttackRange;
    if (CRole* inRange = FindClosestTarget(targets, effectivePos, effectiveAttackRange))
        return inRange;

    if (retreatHoldActive)
        return nullptr;

    const int scoutStopRange = regularAttackRange > 0
        ? regularAttackRange : (std::max)(1, settings.actionRadius);
    const float scoutClusterRadius = (float)(std::max)(3, scoutStopRange + 2);
    int scoutClusterSize = 0;
    if (CRole* scoutTarget = FindBestClusterTarget(targets, effectivePos, scoutClusterRadius, &scoutClusterSize);
        scoutTarget && scoutClusterSize > 1) {
        Position scoutApproachPos;
        if (FindBestSingleTargetApproach(hero, map, settings, scoutTarget, effectivePos, scoutApproachPos, scoutStopRange) && outApproachPos)
            *outApproachPos = scoutApproachPos;
        if (outClumpSize)
            *outClumpSize = scoutClusterSize;
        return scoutTarget;
    }

    CRole* closest = FindClosestTarget(targets, effectivePos);
    if (!closest)
        return nullptr;

    Position singleTargetApproachPos;
    if (FindBestSingleTargetApproach(hero, map, settings, closest, effectivePos, singleTargetApproachPos, regularAttackRange) && outApproachPos)
        *outApproachPos = singleTargetApproachPos;
    return closest;
}

bool ArcherHuntPlugin::FindArcherPatrolPosition(CHero* hero, CGameMap* map,
    const AutoHuntSettings& settings, Position& outPatrolPos) const
{
    outPatrolPos = {};
    if (!hero || !map)
        return false;

    const Position anchor = GetZoneAnchor(settings);
    if (IsZeroPos(anchor))
        return false;

    const Position heroPos = GetEffectiveHeroPosition(hero);
    const int patrolRadius = settings.zoneMode == AutoHuntZoneMode::Circle
        ? (std::max)(1, settings.zoneRadius - 1)
        : (std::max)(6, settings.actionRadius * 2);
    if (patrolRadius <= 0)
        return false;

    static const Position kPatrolDirs[] = {
        {1, 0},
        {1, 1},
        {0, 1},
        {-1, 1},
        {-1, 0},
        {-1, -1},
        {0, -1},
        {1, -1},
    };

    int baseDir = 0;
    if (heroPos.x != anchor.x || heroPos.y != anchor.y) {
        long long bestDot = (std::numeric_limits<long long>::min)();
        for (int i = 0; i < (int)std::size(kPatrolDirs); ++i) {
            const long long dot =
                (long long)(heroPos.x - anchor.x) * kPatrolDirs[i].x
                + (long long)(heroPos.y - anchor.y) * kPatrolDirs[i].y;
            if (dot > bestDot) {
                bestDot = dot;
                baseDir = i;
            }
        }
    }

    for (int dirOffset = 1; dirOffset <= (int)std::size(kPatrolDirs); ++dirOffset) {
        const Position dir = kPatrolDirs[(baseDir + dirOffset) % (int)std::size(kPatrolDirs)];
        const Position desired = {
            anchor.x + dir.x * patrolRadius,
            anchor.y + dir.y * patrolRadius
        };
        const float currentDesiredDist = heroPos.DistanceTo(desired);
        if (currentDesiredDist <= 2.0f)
            continue;

        bool found = false;
        Position bestCandidate = {};
        float bestDesiredDist = (std::numeric_limits<float>::max)();
        float bestMoveDist = (std::numeric_limits<float>::max)();
        for (int dx = -CGameMap::MAX_JUMP_DIST; dx <= CGameMap::MAX_JUMP_DIST; ++dx) {
            for (int dy = -CGameMap::MAX_JUMP_DIST; dy <= CGameMap::MAX_JUMP_DIST; ++dy) {
                const Position candidate = {heroPos.x + dx, heroPos.y + dy};
                if (candidate.x == heroPos.x && candidate.y == heroPos.y)
                    continue;
                if (CGameMap::TileDist(heroPos.x, heroPos.y, candidate.x, candidate.y) > CGameMap::MAX_JUMP_DIST)
                    continue;
                if (!IsPointInZone(settings, settings.zoneMapId, candidate))
                    continue;
                if (!map->IsWalkable(candidate.x, candidate.y))
                    continue;
                if (IsTileOccupied(candidate.x, candidate.y))
                    continue;
                if (!map->CanJump(heroPos.x, heroPos.y, candidate.x, candidate.y, CGameMap::GetHeroAltThreshold()))
                    continue;

                const float desiredDist = candidate.DistanceTo(desired);
                const float moveDist = heroPos.DistanceTo(candidate);
                if (!found || desiredDist < bestDesiredDist
                    || (desiredDist == bestDesiredDist && moveDist < bestMoveDist)) {
                    found = true;
                    bestCandidate = candidate;
                    bestDesiredDist = desiredDist;
                    bestMoveDist = moveDist;
                }
            }
        }

        if (found && bestDesiredDist + 0.1f < currentDesiredDist) {
            outPatrolPos = bestCandidate;
            return true;
        }
    }

    return false;
}


// =============================================================================
// FindBestTarget override
// =============================================================================

CRole* ArcherHuntPlugin::FindBestTarget(CHero* hero, CGameMap* map, const AutoHuntSettings& settings,
    Position* outApproachPos, Position* outAttackPos,
    int* outClumpSize, bool* outUseScatter)
{
    return FindBestArcherTarget(hero, map, settings, outApproachPos, outAttackPos, outClumpSize, outUseScatter);
}


// =============================================================================
// HandleCombatRetreat override
// =============================================================================

bool ArcherHuntPlugin::HandleCombatRetreat(CHero* hero, CGameMap* map, const AutoHuntSettings& settings,
    CRole* target)
{
    if (!hero || !map)
        return false;

    // Handle pending jump landing for retreat tracking
    if (m_pendingJumpTick != 0 && m_pendingJumpIsRetreat) {
        const bool landed = hero->m_posMap.x == m_pendingJumpDest.x
            && hero->m_posMap.y == m_pendingJumpDest.y;
        if (landed) {
            m_retreatHoldUntilTick = GetTickCount() + kArcherRetreatHoldMs;
            m_lastFailedRetreatDest = {};
            m_lastFailedRetreatTick = 0;
        }
    }

    // Handle failed retreat tracking
    if (m_pendingJumpTick != 0 && m_pendingJumpIsRetreat && !hero->IsJumping()) {
        const DWORD now = GetTickCount();
        const bool landed = hero->m_posMap.x == m_pendingJumpDest.x
            && hero->m_posMap.y == m_pendingJumpDest.y;
        if (!landed) {
            const DWORD age = now - m_pendingJumpTick;
            if (age > 2000) {
                m_lastFailedRetreatDest = m_pendingJumpDest;
                m_lastFailedRetreatTick = now;
                m_retreatCooldownTick = 0;
            }
        }
    }

    const DWORD now = GetTickCount();

    // Try danger retreat first
    if (TryArcherDangerRetreat(hero, map, settings, target))
        return true;

    // Hold position after successful retreat
    if (TickIsFuture(m_retreatHoldUntilTick, now)) {
        const Position effectivePos = GetEffectiveHeroPosition(hero);
        const int regularAttackRange = GetRegularArcherAttackRange(settings);
        const int requiredAttackRange = (std::max)(1, m_lastScatterRange > 0 ? m_lastScatterRange : regularAttackRange);
        const int attackDist = target
            ? CGameMap::TileDist(effectivePos.x, effectivePos.y, target->m_posMap.x, target->m_posMap.y)
            : 9999;
        if (attackDist > requiredAttackRange) {
            SetState(AutoHuntState::AttackTarget, "Holding safe position after retreat");
            return true;
        }
    }

    return false;
}


// =============================================================================
// HandleCombatApproach override
// =============================================================================

void ArcherHuntPlugin::HandleCombatApproach(CHero* hero, CGameMap* map, const AutoHuntSettings& settings,
    CRole* target, const Position& approachPos, bool movementCommitted)
{
    if (!hero || !map || !target)
        return;

    const Position effectiveAttackPos = GetEffectiveHeroPosition(hero);
    const int regularAttackRange = GetRegularArcherAttackRange(settings);
    const bool useRegularArcherRange = !IsZeroPos(approachPos) || regularAttackRange > 0;
    const bool bypassMobRangeChecks = regularAttackRange <= 0;

    // Compute attack distance based on scatter or regular
    // (scatter attackPos is passed through approachPos for scatter-approach tiles)
    const int moveDist = CGameMap::TileDist(effectiveAttackPos.x, effectiveAttackPos.y,
        target->m_posMap.x, target->m_posMap.y);
    const int approachDist = !IsZeroPos(approachPos)
        ? CGameMap::TileDist(effectiveAttackPos.x, effectiveAttackPos.y, approachPos.x, approachPos.y)
        : moveDist;
    const int requiredAttackRange = useRegularArcherRange ? regularAttackRange : kReliableAttackRange;

    if (bypassMobRangeChecks)
        return;  // No approach needed when range checks bypassed

    // If approach is within range, skip
    if (!IsZeroPos(approachPos) && approachDist == 0)
        return;
    if (IsZeroPos(approachPos) && moveDist <= requiredAttackRange)
        return;

    // Pre-landing retreat: don't approach
    if (m_buffMgr.IsPreLandingRetreat())
        return;

    // Don't re-evaluate while movement is in flight
    if (movementCommitted)
        return;

    const int actionRadius = (std::max)(1, settings.actionRadius);
    const int localWalkRadius = regularAttackRange > 0
        ? (std::max)(actionRadius, regularAttackRange)
        : (std::max)(actionRadius, (std::max)(1, m_lastScatterRange));

    if (!IsZeroPos(approachPos)) {
        const bool startedPath = StartPathTo(hero, map, approachPos, 0);
        if (startedPath) {
            SetState(AutoHuntState::ApproachTarget, "Jumping to scatter clump");
        } else {
            SetState(AutoHuntState::ApproachTarget, "Unable to reach target");
        }
    } else {
        const bool startedPath = StartPathNearTarget(hero, map, target->m_posMap, requiredAttackRange);
        if (startedPath) {
            SetState(AutoHuntState::ApproachTarget, "Closing distance to target");
        } else {
            SetState(AutoHuntState::ApproachTarget, "Unable to reach target");
        }
    }
}


// =============================================================================
// HandleCombatAttack override
// =============================================================================

void ArcherHuntPlugin::HandleCombatAttack(CHero* hero, CGameMap* map, const AutoHuntSettings& settings,
    CRole* target, const Position& attackPos, DWORD now)
{
    if (!hero || !target)
        return;

    const bool movementCommitted = hero->IsJumping() || (m_pendingJumpTick != 0);
    const int regularAttackRange = GetRegularArcherAttackRange(settings);
    const bool bypassMobRangeChecks = regularAttackRange <= 0;
    const bool useScatter = !IsZeroPos(attackPos) && IsScatterLogicEnabled(settings);
    const int clumpSize = m_lastClumpSize;
    const bool isScatterClump = useScatter && clumpSize >= (std::max)(1, settings.minimumScatterHits);

    const bool targetChanged = (m_targetId != target->GetID());
    const bool justFinishedApproach = (m_state == AutoHuntState::ApproachTarget);
    const DWORD attackInterval = hero->IsCycloneActive()
        ? GetCycloneAttackIntervalMs(settings)
        : GetAttackIntervalMs(settings);
    const DWORD nextAttackDelay = (targetChanged || justFinishedApproach)
        ? GetTargetSwitchAttackIntervalMs(settings)
        : attackInterval;

    if (Pathfinder::Get().IsActive())
        Pathfinder::Get().Stop();

    // Scatter attack
    if (useScatter) {
        if (CMagic* scatter = FindScatterMagic(hero)) {
            const DWORD scatterDelay = (std::max)(nextAttackDelay, static_cast<DWORD>(350));
            if (!IsZeroPos(attackPos) && now - m_lastAttackTick >= scatterDelay) {
                hero->MagicAttack(scatter->GetMagicType(), attackPos);
                m_lastAttackTick = now;
            }
            if (hero->IsJumping()) {
                SetState(AutoHuntState::AttackTarget,
                    isScatterClump ? "Casting Scatter during jump" : "Casting Scatter at target during jump");
            } else if (isScatterClump) {
                SetState(AutoHuntState::AttackTarget, "Casting Scatter at mob clump");
            } else {
                SetState(AutoHuntState::AttackTarget, "Casting Scatter");
            }
            return;
        }
    }

    // Regular ranged attack
    if (now - m_lastAttackTick >= nextAttackDelay) {
        hero->ShootTarget(target->GetID());
        m_lastAttackTick = now;
    }

    const int moveDist = CGameMap::TileDist(hero->m_posMap.x, hero->m_posMap.y,
        target->m_posMap.x, target->m_posMap.y);
    if (movementCommitted && moveDist > kReliableAttackRange) {
        SetState(AutoHuntState::AttackTarget, "Attacking target during jump");
    } else {
        SetState(AutoHuntState::AttackTarget, "Attacking target");
    }
}


// =============================================================================
// HandleCombatItems override
// =============================================================================

bool ArcherHuntPlugin::HandleCombatItems(CHero* hero, const AutoHuntSettings& settings)
{
    return TryManageArrows(hero, settings);
}


// =============================================================================
// NeedsTownRunArrows override
// =============================================================================

bool ArcherHuntPlugin::NeedsTownRunArrows(const CHero* hero, const AutoHuntSettings& settings) const
{
    return NeedsArrows(hero, settings);
}


// =============================================================================
// HandleNoTargetIdle override
// =============================================================================

bool ArcherHuntPlugin::HandleNoTargetIdle(CHero* hero, CGameMap* map, const AutoHuntSettings& settings)
{
    if (!hero || !map)
        return false;

    // Hold position after retreat even with no target
    if (TickIsFuture(m_retreatHoldUntilTick, GetTickCount())) {
        m_targetId = 0;
        SetState(AutoHuntState::AttackTarget, "Holding safe position after retreat");
        return true;
    }

    Position patrolPos = {};
    if (FindArcherPatrolPosition(hero, map, settings, patrolPos) && StartPathTo(hero, map, patrolPos, 0)) {
        m_targetId = 0;
        SetState(AutoHuntState::AcquireTarget, "Scouting hunt zone for mob clumps");
        return true;
    }

    return false;
}


// =============================================================================
// RefreshCombatState override
// =============================================================================

void ArcherHuntPlugin::RefreshCombatState(CHero* hero, const AutoHuntSettings& /*settings*/)
{
    m_lastScatterRange = GetScatterRange(hero);
}


// =============================================================================
// Arrow management
// =============================================================================

bool ArcherHuntPlugin::TryManageArrows(CHero* hero, const AutoHuntSettings& settings)
{
    if (!hero || !IsArcherModeEnabled(settings))
        return false;

    const DWORD now = GetTickCount();
    if (now - m_lastArrowTick < GetItemActionIntervalMs(settings))
        return false;

    // If equipped arrows are depleted (<= 3), equip a fresh pack from inventory
    CItem* equipped = hero->GetEquip(EquipSlot::LWEAPON);
    if (equipped && equipped->IsArrow() && equipped->GetDurability() <= 3) {
        for (const auto& itemRef : hero->m_deqItem) {
            if (!itemRef || !itemRef->IsArrow())
                continue;
            if (itemRef->GetDurability() <= 3)
                continue;

            hero->EquipItem(itemRef->GetID(), EquipSlot::LWEAPON);
            m_lastArrowTick = now;
            SetState(AutoHuntState::Recover, "Equipping arrows");
            return true;
        }
    }

    // Drop any inventory arrow packs with <= 5 durability
    for (const auto& itemRef : hero->m_deqItem) {
        if (!itemRef || !itemRef->IsArrow())
            continue;
        if (itemRef->GetDurability() > 5)
            continue;

        hero->DropItem(itemRef->GetID(), hero->m_posMap);
        m_lastArrowTick = now;
        SetState(AutoHuntState::Recover, "Dropping depleted arrows");
        return true;
    }

    return false;
}

bool ArcherHuntPlugin::NeedsArrows(const CHero* hero, const AutoHuntSettings& settings) const
{
    if (!IsArcherModeEnabled(settings) || !settings.buyArrows)
        return false;
    return CountUsableArrowPacks(hero) < settings.arrowBuyCount;
}

int ArcherHuntPlugin::CountUsableArrowPacks(const CHero* hero) const
{
    if (!hero)
        return 0;

    int count = 0;
    // Count equipped arrows
    CItem* equipped = hero->GetEquip(EquipSlot::LWEAPON);
    if (equipped && equipped->IsArrow() && equipped->GetDurability() > 3)
        ++count;

    // Count inventory arrow packs
    for (const auto& itemRef : hero->m_deqItem) {
        if (!itemRef || !itemRef->IsArrow())
            continue;
        if (itemRef->GetDurability() > 3)
            ++count;
    }

    return count;
}


// =============================================================================
// RenderCombatUI override
// =============================================================================

void ArcherHuntPlugin::RenderCombatUI(AutoHuntSettings& settings)
{
    ImGui::Checkbox("Use Scatter Logic", &settings.useScatterLogic);
    ImGui::SliderInt("Ranged Attack Range", &settings.rangedAttackRange, 0, CGameMap::MAX_JUMP_DIST);
    ImGui::SliderInt("Safety Distance", &settings.archerSafetyDistance, 0, CGameMap::MAX_JUMP_DIST);
    if (settings.useScatterLogic) {
        ImGui::Checkbox("Prioritize Scatter Clumps", &settings.prioritizeScatterClumps);
        ImGui::SliderInt("Minimum Scatter Hits", &settings.minimumScatterHits, 1, 12);
        ImGui::SliderInt("Scatter Range Override", &settings.scatterRangeOverride, 0, CGameMap::MAX_JUMP_DIST);
        if (settings.scatterRangeOverride == 0)
            ImGui::TextDisabled("0 = auto-detect from skill (current: %d)", m_lastScatterRange);
        ImGui::TextDisabled("Archer mode scores the best 180-degree Scatter sector from the current or next jump position.");
        ImGui::TextDisabled("If no valid Scatter clump is found, auto hunt falls back to regular single-target attacks.");
    } else {
        ImGui::TextDisabled("Archer mode uses regular attacks only when Scatter logic is off.");
    }
    ImGui::TextDisabled("0 disables the regular archer attack range check and attacks immediately.");
    ImGui::TextDisabled("When set above 0, auto hunt stops and attacks from that many tiles away.");
    ImGui::TextDisabled("Safety Distance prevents jumping closer than N tiles to any mob (0 = disabled).");

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

}
