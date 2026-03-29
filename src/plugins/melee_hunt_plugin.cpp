#include "melee_hunt_plugin.h"
#include "hunt_targeting.h"
#include "game.h"
#include "CHero.h"
#include "CGameMap.h"
#include "CRole.h"
#include "config.h"
#include "pathfinder.h"
#include "log.h"
#include "imgui.h"
#include <algorithm>
#include <limits>
#include <vector>

namespace {

constexpr int kReliableAttackRange = 1;
constexpr int kMinMobClumpSize = 2;
constexpr int kMinAttackIntervalMs = 25;
constexpr int kMaxAttackIntervalMs = 5000;
constexpr int kMinTargetSwitchAttackIntervalMs = 0;
constexpr int kMaxTargetSwitchAttackIntervalMs = 5000;

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

} // anonymous namespace


// ── FindBestClumpApproach ─────────────────────────────────────────────────────

bool MeleeHuntPlugin::FindBestClumpApproach(CHero* hero, CGameMap* map, const AutoHuntSettings& settings,
    const std::vector<CRole*>& targets, Position& outApproachPos,
    CRole*& outPrimaryTarget, int& outClumpSize) const
{
    outApproachPos = {};
    outPrimaryTarget = nullptr;
    outClumpSize = 0;

    if (!hero || !map || !settings.prioritizeMobClumps)
        return false;

    const int minMobClump = kMinMobClumpSize;
    if ((int)targets.size() < minMobClump)
        return false;

    const float clumpRadius = (float)(std::max)(1, settings.clumpRadius);
    const Position effectivePos = GetEffectiveHeroPosition(hero);
    const float localTargetRadius = (float)(CGameMap::MAX_JUMP_DIST + settings.clumpRadius + kReliableAttackRange);
    std::vector<CRole*> localTargets;
    localTargets.reserve(targets.size());
    for (CRole* target : targets) {
        if (target && effectivePos.DistanceTo(target->m_posMap) <= localTargetRadius)
            localTargets.push_back(target);
    }

    if ((int)localTargets.size() < minMobClump)
        return false;

    const Position jumpOrigin = GetEffectiveHeroPosition(hero);
    bool found = false;
    int bestAttackDist = (std::numeric_limits<int>::max)();
    int bestMinThreatDist = -1;
    float bestCenterDist = (std::numeric_limits<float>::max)();
    float bestMoveDist = (std::numeric_limits<float>::max)();

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

            int clumpSize = 0;
            long long sumX = 0;
            long long sumY = 0;
            CRole* closestAttackable = nullptr;
            int closestAttackDist = (std::numeric_limits<int>::max)();
            float closestTargetDist = (std::numeric_limits<float>::max)();
            for (CRole* target : localTargets) {
                if (!target || candidate.DistanceTo(target->m_posMap) > clumpRadius)
                    continue;

                ++clumpSize;
                sumX += target->m_posMap.x;
                sumY += target->m_posMap.y;

                const int attackDist = CGameMap::TileDist(candidate.x, candidate.y,
                    target->m_posMap.x, target->m_posMap.y);
                if (attackDist > kReliableAttackRange)
                    continue;

                const float targetDist = candidate.DistanceTo(target->m_posMap);
                if (!closestAttackable || attackDist < closestAttackDist
                    || (attackDist == closestAttackDist && targetDist < closestTargetDist)) {
                    closestAttackable = target;
                    closestAttackDist = attackDist;
                    closestTargetDist = targetDist;
                }
            }

            if (clumpSize < minMobClump || !closestAttackable)
                continue;

            const Position centroid = {
                (int)(sumX / (long long)clumpSize),
                (int)(sumY / (long long)clumpSize)
            };
            const float centerDist = candidate.DistanceTo(centroid);
            const float moveDist = effectivePos.DistanceTo(candidate);
            // Melee has no archer safety distance — minThreatDist is always INT_MAX
            constexpr int minThreatDist = (std::numeric_limits<int>::max)();
            if (!found
                || clumpSize > outClumpSize
                || (clumpSize == outClumpSize && closestAttackDist < bestAttackDist)
                || (clumpSize == outClumpSize && closestAttackDist == bestAttackDist && minThreatDist > bestMinThreatDist)
                || (clumpSize == outClumpSize && closestAttackDist == bestAttackDist && minThreatDist == bestMinThreatDist && centerDist < bestCenterDist)
                || (clumpSize == outClumpSize && closestAttackDist == bestAttackDist
                    && minThreatDist == bestMinThreatDist && centerDist == bestCenterDist && moveDist < bestMoveDist)) {
                found = true;
                outApproachPos = candidate;
                outPrimaryTarget = closestAttackable;
                outClumpSize = clumpSize;
                bestAttackDist = closestAttackDist;
                bestMinThreatDist = minThreatDist;
                bestCenterDist = centerDist;
                bestMoveDist = moveDist;
            }
        }
    }

    return found;
}


// ── FindBestMeleeTarget ───────────────────────────────────────────────────────

CRole* MeleeHuntPlugin::FindBestMeleeTarget(CHero* hero, CGameMap* map, const AutoHuntSettings& settings,
    Position* outApproachPos, int* outClumpSize) const
{
    if (outApproachPos)
        *outApproachPos = {};
    if (outClumpSize)
        *outClumpSize = 0;
    if (!hero || !map)
        return nullptr;

    const bool hasPreferFilter = settings.monsterPreferNames[0] != '\0';
    std::vector<CRole*> targets = hasPreferFilter ? CollectHuntTargets(settings, true) : std::vector<CRole*>{};
    if (targets.empty())
        targets = CollectHuntTargets(settings);
    if (targets.empty())
        return nullptr;

    const Position effectivePos = GetEffectiveHeroPosition(hero);
    const int actionRadius = (std::max)(1, settings.actionRadius);
    const int clumpRadius = (std::max)(1, settings.clumpRadius);
    const int currentClumpSize = CountTargetsInRadius(targets, effectivePos, (float)clumpRadius);
    const int configuredClumpSize = (std::max)(kMinMobClumpSize, settings.minimumMobClump);
    Position singleTargetApproachPos;

    // 1. Clear all mobs within action radius first.
    // Randomize target when AoE buffs are active to spread damage across the clump
    const bool randomize = settings.usePacketJump
        && (hero->IsCycloneActive() || hero->IsSupermanActive());
    if (CRole* actionTarget = randomize
            ? FindRandomTarget(targets, effectivePos, actionRadius)
            : FindClosestTarget(targets, effectivePos, actionRadius)) {
        if (FindBestSingleTargetApproach(hero, map, settings, actionTarget, effectivePos, singleTargetApproachPos, kReliableAttackRange)
            && outApproachPos) {
            *outApproachPos = singleTargetApproachPos;
        }
        if (outClumpSize)
            *outClumpSize = currentClumpSize;
        return actionTarget;
    }

    // 2. Already inside a large enough local clump — clear it.
    if (currentClumpSize >= configuredClumpSize) {
        if (CRole* localAoeTarget = FindClosestTarget(targets, effectivePos, clumpRadius)) {
            if (FindBestSingleTargetApproach(hero, map, settings, localAoeTarget, effectivePos, singleTargetApproachPos, kReliableAttackRange)
                && outApproachPos) {
                *outApproachPos = singleTargetApproachPos;
            }
            if (outClumpSize)
                *outClumpSize = currentClumpSize;
            return localAoeTarget;
        }
    }

    // 3. No nearby mobs — jump to the best distant clump (single-jump range).
    Position clumpApproachPos;
    CRole* clumpTarget = nullptr;
    int clumpSize = 0;
    if (settings.prioritizeMobClumps
        && FindBestClumpApproach(hero, map, settings, targets, clumpApproachPos, clumpTarget, clumpSize)
        && clumpSize > currentClumpSize) {
        if (outApproachPos)
            *outApproachPos = clumpApproachPos;
        if (outClumpSize)
            *outClumpSize = clumpSize;
        return clumpTarget;
    }

    // 4. Best clump beyond jump range — pathfind toward it.
    if (settings.prioritizeMobClumps) {
        int bestClusterSize = 0;
        if (CRole* bestCluster = FindBestClusterTarget(targets, effectivePos,
                (float)(std::max)(1, settings.clumpRadius), &bestClusterSize);
            bestCluster && bestClusterSize >= configuredClumpSize) {
            if (outApproachPos)
                *outApproachPos = bestCluster->m_posMap;
            if (outClumpSize)
                *outClumpSize = bestClusterSize;
            return bestCluster;
        }
    }

    // 5. Fallback: closest target anywhere.
    CRole* closest = FindClosestTarget(targets, effectivePos);
    if (!closest)
        return nullptr;

    if (FindBestSingleTargetApproach(hero, map, settings, closest, effectivePos, singleTargetApproachPos, kReliableAttackRange)
        && outApproachPos) {
        *outApproachPos = singleTargetApproachPos;
    }
    if (outClumpSize)
        *outClumpSize = currentClumpSize;

    return closest;
}


// ── FindBestTarget override ───────────────────────────────────────────────────

CRole* MeleeHuntPlugin::FindBestTarget(CHero* hero, CGameMap* map, const AutoHuntSettings& settings,
    Position* outApproachPos, Position* outAttackPos,
    int* outClumpSize, bool* outUseScatter)
{
    if (outAttackPos)
        *outAttackPos = {};
    if (outUseScatter)
        *outUseScatter = false;
    return FindBestMeleeTarget(hero, map, settings, outApproachPos, outClumpSize);
}


// ── HandleCombatApproach override ────────────────────────────────────────────

void MeleeHuntPlugin::HandleCombatApproach(CHero* hero, CGameMap* map, const AutoHuntSettings& settings,
    CRole* target, const Position& approachPos, bool movementCommitted)
{
    if (!hero || !map || !target)
        return;

    const Position effectiveAttackPos = GetEffectiveHeroPosition(hero);
    const int moveDist = CGameMap::TileDist(effectiveAttackPos.x, effectiveAttackPos.y,
        target->m_posMap.x, target->m_posMap.y);
    const int approachDist = !IsZeroPos(approachPos)
        ? CGameMap::TileDist(effectiveAttackPos.x, effectiveAttackPos.y, approachPos.x, approachPos.y)
        : moveDist;
    const int clumpSize = m_lastClumpSize;
    const bool isClumpTarget = settings.prioritizeMobClumps && clumpSize >= kMinMobClumpSize;
    const int actionRadius = (std::max)(1, settings.actionRadius);
    const int clumpRadius = (std::max)(1, settings.clumpRadius);
    const int localWalkRadius = (std::max)(actionRadius, clumpRadius);

    // Legit mode (no speedhack) always walks to nearby mobs
    const bool allowWalkToMob = !settings.usePacketJump;

    // If already in attack range, no approach needed
    if (moveDist <= kReliableAttackRange && IsZeroPos(approachPos))
        return;
    if (!IsZeroPos(approachPos) && approachDist == 0)
        return;

    // Don't re-evaluate while approach is in flight
    if (movementCommitted)
        return;

    if (!IsZeroPos(approachPos)) {
        // Approach to clump position
        const bool usingClumpApproach = isClumpTarget && !IsZeroPos(approachPos);
        const bool startedWalk = allowWalkToMob
            && approachDist <= localWalkRadius
            && StartWalkTo(hero, map, approachPos, 0);
        const bool startedPath = startedWalk || StartPathTo(hero, map, approachPos, 0);
        if (startedPath) {
            SetState(AutoHuntState::ApproachTarget,
                startedWalk
                    ? (usingClumpApproach ? "Walking to mob clump" : "Walking to target")
                    : (usingClumpApproach ? "Jumping to mob clump" : "Jumping to target"));
        } else {
            SetState(AutoHuntState::ApproachTarget, "Unable to reach target");
        }
    } else {
        // No approach pos — path near the target directly
        const bool startedWalk = allowWalkToMob
            && moveDist <= localWalkRadius
            && StartWalkTo(hero, map, target->m_posMap, kReliableAttackRange);
        const bool startedPath = startedWalk || StartPathNearTarget(hero, map, target->m_posMap, kReliableAttackRange);
        if (startedPath) {
            SetState(AutoHuntState::ApproachTarget,
                startedWalk ? "Walking to target" : "Closing distance to target");
        } else {
            SetState(AutoHuntState::ApproachTarget, "Unable to reach target");
        }
    }
}


// ── HandleCombatAttack override ───────────────────────────────────────────────

void MeleeHuntPlugin::HandleCombatAttack(CHero* hero, CGameMap* map, const AutoHuntSettings& settings,
    CRole* target, const Position& /*attackPos*/, DWORD now)
{
    if (!hero || !target)
        return;

    const bool movementCommitted = hero->IsJumping() || (m_pendingJumpTick != 0);
    const int moveDist = CGameMap::TileDist(hero->m_posMap.x, hero->m_posMap.y,
        target->m_posMap.x, target->m_posMap.y);
    const int clumpSize = m_lastClumpSize;
    const bool isClumpTarget = settings.prioritizeMobClumps && clumpSize >= kMinMobClumpSize;

    // Legit mode: only attack when within 1 tile (adjacent).
    // Speedhack mode: attack immediately at any distance.
    if (!settings.usePacketJump) {
        const int actualDist = CGameMap::TileDist(hero->m_posMap.x, hero->m_posMap.y,
            target->m_posMap.x, target->m_posMap.y);
        if (actualDist > 1) {
            SetState(AutoHuntState::AttackTarget,
                movementCommitted
                    ? "Waiting until adjacent before attacking during jump"
                    : "Waiting until adjacent before attacking");
            return;
        }
    }

    // Determine attack interval
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

    if (now - m_lastAttackTick >= nextAttackDelay) {
        hero->AttackTarget(target->GetID(), target->m_posMap);
        m_lastAttackTick = now;
    }

    if (movementCommitted && moveDist > kReliableAttackRange) {
        SetState(AutoHuntState::AttackTarget,
            isClumpTarget
                ? "Attacking mob clump during jump"
                : "Attacking target during jump");
    } else if (isClumpTarget) {
        SetState(AutoHuntState::AttackTarget, "Attacking target inside mob clump");
    } else {
        SetState(AutoHuntState::AttackTarget, "Attacking target");
    }
}


// ── RenderCombatUI override ───────────────────────────────────────────────────

void MeleeHuntPlugin::RenderCombatUI(AutoHuntSettings& settings)
{
    ImGui::Checkbox("Speedhack If No Players Nearby", &settings.usePacketJump);
    ImGui::TextDisabled("Send raw jump packets for faster movement. Falls back to normal jumps when players are nearby.");
    ImGui::SliderInt("Action Radius", &settings.actionRadius, 1, CGameMap::MAX_JUMP_DIST);
    ImGui::Checkbox("Prioritize Mob Clumps", &settings.prioritizeMobClumps);
    ImGui::SliderInt("Clump Radius", &settings.clumpRadius, 1, 18);
    ImGui::SliderInt("Minimum Mob Clump", &settings.minimumMobClump, 2, 12);
    ImGui::TextDisabled("Prefer nearby targets inside Action Radius before chasing distant mob clumps.");
    ImGui::TextDisabled("If enough mobs are already inside Clump Radius, auto hunt clears that local pack first.");
}
