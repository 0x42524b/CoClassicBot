#include "hunt_buffs.h"
#include "hunt_targeting.h"
#include "game.h"
#include "CHero.h"
#include "CGameMap.h"
#include "CRole.h"
#include "CItem.h"
#include "CMagic.h"
#include "itemtype.h"
#include "inventory_utils.h"
#include "pathfinder.h"
#include "log.h"
#include <algorithm>
#include <limits>

// ── File-local constants ──────────────────────────────────────────────────────
static constexpr DWORD FLY_XP_DURATION_MS      = 40000;
static constexpr DWORD FLY_STAMINA_DURATION_MS  = 60000;
static constexpr DWORD FLY_RECAST_BUFFER_MS     = 5000;
static constexpr DWORD FLY_SAFETY_BUFFER_MS     = 8000;

static constexpr int kNearbyPotionRange      = 10;
static constexpr int kNearbyPotionCarryLimit = 2;
static constexpr int kLootPathStopRange      = 0;

static constexpr int kMinSelfCastIntervalMs  = 100;
static constexpr int kMaxSelfCastIntervalMs  = 5000;
static constexpr int kMinItemActionIntervalMs = 100;
static constexpr int kMaxItemActionIntervalMs = 5000;
static constexpr int kArcherSafetyBufferTiles = 1;

// ── File-local helpers ────────────────────────────────────────────────────────
namespace {

DWORD ClampMs(int value, int minVal, int maxVal)
{
    return static_cast<DWORD>(std::clamp(value, minVal, maxVal));
}

DWORD GetSelfCastIntervalMs(const AutoHuntSettings& settings)
{
    return ClampMs(settings.selfCastIntervalMs, kMinSelfCastIntervalMs, kMaxSelfCastIntervalMs);
}

DWORD GetItemActionIntervalMs(const AutoHuntSettings& settings)
{
    return ClampMs(settings.itemActionIntervalMs, kMinItemActionIntervalMs, kMaxItemActionIntervalMs);
}

DWORD GetLootSpawnGraceMs(const AutoHuntSettings& settings)
{
    constexpr int kMin = 0;
    constexpr int kMax = 5000;
    return ClampMs(settings.lootSpawnGraceMs, kMin, kMax);
}

bool IsArcherModeEnabled(const AutoHuntSettings& settings)
{
    return settings.archerMode || settings.combatMode == AutoHuntCombatMode::Archer;
}

int GetRequiredArcherThreatDistance(int safetyDist)
{
    return safetyDist > 0 ? (safetyDist + kArcherSafetyBufferTiles) : 0;
}

} // namespace

// ── HuntBuffManager implementation ───────────────────────────────────────────

void HuntBuffManager::RefreshBuffState(CHero* hero)
{
    m_xpSkillReady  = hero && hero->HasXpSkillReady();
    m_supermanKnown = hero && hero->FindMagicByName("Superman") != nullptr;
    m_supermanActive = hero && hero->IsSupermanActive();
    m_cycloneKnown  = hero && hero->FindMagicByName("Cyclone") != nullptr;
    m_cycloneActive  = hero && hero->IsCycloneActive();
    m_flyKnown      = hero && hero->FindMagicByName("Fly") != nullptr;

    const bool wasFlyActive = m_flyActive;
    m_flyActive = hero && hero->IsFlyActive();
    if (wasFlyActive && !m_flyActive) {
        m_flyStartTick   = 0;
        m_flyDurationMs  = 0;
        m_preLandingRetreat = false;
    }
}

DWORD HuntBuffManager::GetRemainingFlyMs() const
{
    if (!m_flyActive || m_flyStartTick == 0 || m_flyDurationMs == 0)
        return 0;
    const DWORD elapsed = GetTickCount() - m_flyStartTick;
    if (elapsed >= m_flyDurationMs)
        return 0;
    return m_flyDurationMs - elapsed;
}

bool HuntBuffManager::CanRecastAnyFly(CHero* hero, const AutoHuntSettings& settings) const
{
    if (!hero || !IsArcherModeEnabled(settings))
        return false;
    for (const auto& magicRef : hero->m_vecMagic) {
        if (!magicRef || !magicRef->IsEnabled())
            continue;
        if (_stricmp(magicRef->GetName(), "Fly") != 0)
            continue;
        if (magicRef->IsXpSkill()) {
            if (settings.castXpFly && hero->HasXpSkillReady())
                return true;
        } else {
            if (settings.castFly && hero->GetStamina() >= static_cast<int>(magicRef->GetStaminaCost()))
                return true;
        }
    }
    return false;
}

bool HuntBuffManager::TryCastSuperman(CHero* hero, const AutoHuntSettings& settings,
    const HuntBuffCallbacks& cb)
{
    if (!settings.castSuperman || !hero)
        return false;
    if (!hero->HasXpSkillReady())
        return false;
    if (hero->IsSupermanActive())
        return false;

    const DWORD now = GetTickCount();
    if (now - m_lastSupermanTick < GetSelfCastIntervalMs(settings))
        return false;

    CMagic* superman = hero->FindMagicByName("Superman");
    if (!superman || !superman->IsEnabled())
        return false;

    hero->MagicAttack(superman->GetMagicType(), hero->GetID(), hero->m_posMap);
    m_lastSupermanTick = now;
    cb.setStateFn(AutoHuntState::Recover, "Casting Superman");
    return true;
}

bool HuntBuffManager::TryCastCyclone(CHero* hero, const AutoHuntSettings& settings,
    const HuntBuffCallbacks& cb)
{
    if (!settings.castCyclone || !hero)
        return false;
    if (!hero->HasXpSkillReady())
        return false;

    const DWORD now = GetTickCount();
    if (now - m_lastCycloneTick < GetSelfCastIntervalMs(settings))
        return false;

    CMagic* cyclone = hero->FindMagicByName("Cyclone");
    if (!cyclone || !cyclone->IsEnabled())
        return false;

    const bool wasActive = hero->IsCycloneActive();
    hero->MagicAttack(cyclone->GetMagicType(), hero->GetID(), hero->m_posMap);
    m_lastCycloneTick = now;
    cb.setStateFn(AutoHuntState::Recover, wasActive ? "Refreshing Cyclone" : "Casting Cyclone");
    return true;
}

bool HuntBuffManager::TryCastAccuracy(CHero* hero, const AutoHuntSettings& settings,
    const HuntBuffCallbacks& cb)
{
    if (!hero)
        return false;
    if (!hero->HasXpSkillReady())
        return false;

    const DWORD now = GetTickCount();
    if (now - m_lastCycloneTick < GetSelfCastIntervalMs(settings))
        return false;

    CMagic* accuracy = hero->FindMagicByName("Accuracy");
    if (!accuracy || !accuracy->IsEnabled())
        return false;

    hero->MagicAttack(accuracy->GetMagicType(), hero->GetID(), hero->m_posMap);
    m_lastCycloneTick = now;  // shares XP skill cooldown
    cb.setStateFn(AutoHuntState::Recover, "Casting Accuracy");
    return true;
}

bool HuntBuffManager::TryCastXpFly(CHero* hero, const AutoHuntSettings& settings,
    const HuntBuffCallbacks& cb)
{
    if (!settings.castXpFly || !hero)
        return false;
    if (!IsArcherModeEnabled(settings))
        return false;
    if (settings.flyOnlyWithCyclone && !m_cycloneActive)
        return false;
    if (!hero->HasXpSkillReady())
        return false;

    // Don't cast XP Fly (40s) if we already have more remaining fly time — it would shorten our flight
    const DWORD remaining = GetRemainingFlyMs();
    if (remaining > FLY_XP_DURATION_MS)
        return false;

    for (const auto& magicRef : hero->m_vecMagic) {
        if (!magicRef || !magicRef->IsEnabled())
            continue;
        if (_stricmp(magicRef->GetName(), "Fly") != 0)
            continue;
        if (!magicRef->IsXpSkill())
            continue;
        hero->MagicAttack(magicRef->GetMagicType(), hero->GetID(), hero->m_posMap);
        m_flyStartTick   = GetTickCount();
        m_flyDurationMs  = FLY_XP_DURATION_MS;
        cb.setStateFn(AutoHuntState::Recover, "Casting XP Fly");
        return true;
    }
    return false;
}

bool HuntBuffManager::TryCastFly(CHero* hero, const AutoHuntSettings& settings,
    const HuntBuffCallbacks& cb)
{
    if (!settings.castFly || !hero)
        return false;
    if (!IsArcherModeEnabled(settings))
        return false;
    if (settings.flyOnlyWithCyclone && !m_cycloneActive)
        return false;

    // Cast when not flying, or re-cast shortly before expiry to maintain uptime
    const DWORD remaining = GetRemainingFlyMs();
    if (hero->IsFlyActive() && remaining > FLY_RECAST_BUFFER_MS)
        return false;

    for (const auto& magicRef : hero->m_vecMagic) {
        if (!magicRef || !magicRef->IsEnabled())
            continue;
        if (_stricmp(magicRef->GetName(), "Fly") != 0)
            continue;
        if (magicRef->IsXpSkill())
            continue;
        if (hero->GetStamina() < static_cast<int>(magicRef->GetStaminaCost()))
            return false;
        hero->MagicAttack(magicRef->GetMagicType(), hero->GetID(), hero->m_posMap);
        m_flyStartTick   = GetTickCount();
        m_flyDurationMs  = FLY_STAMINA_DURATION_MS;
        cb.setStateFn(AutoHuntState::Recover, "Casting Fly");
        return true;
    }
    return false;
}

bool HuntBuffManager::TryCastStigma(CHero* hero, const AutoHuntSettings& settings,
    int lastMana, const HuntBuffCallbacks& cb)
{
    if (!settings.useStigma || !hero)
        return false;
    if (hero->IsStigmaActive())
        return false;
    if (hero->IsMagicShieldActive())
        return false;

    const DWORD now = GetTickCount();
    if (now - m_lastStigmaTick < GetSelfCastIntervalMs(settings))
        return false;

    CMagic* stigma = hero->FindMagicByName("Stigma");
    if (!stigma || !stigma->IsEnabled())
        return false;
    if (stigma->GetMpCost() > 0 && lastMana < (int)stigma->GetMpCost()) {
        if (!settings.pickupNearbyManaPotionForStigma)
            return false;

        CGameMap* map = Game::GetMap();
        if (!map)
            return false;

        std::shared_ptr<CMapItem> manaPotion = FindNearbyPotionLoot(hero, map, settings, true, cb);
        if (!manaPotion)
            return false;

        const int lootDist = CGameMap::TileDist(hero->m_posMap.x, hero->m_posMap.y,
            manaPotion->m_pos.x, manaPotion->m_pos.y);
        const int lootRange = (std::clamp)(settings.lootRange, 0, CGameMap::MAX_JUMP_DIST);
        const bool inRange  = lootRange > 0 ? lootDist <= lootRange : lootDist == 0;
        if (inRange && cb.tryPickupLootItemFn && cb.tryPickupLootItemFn(hero, manaPotion, now)) {
            cb.setTargetId(manaPotion->m_id);
            cb.setStateFn(AutoHuntState::LootNearby, "Picking up nearby mana potion for Stigma");
            return true;
        }

        cb.setTargetId(manaPotion->m_id);
        if (cb.startPathNearTargetFn && cb.startPathNearTargetFn(hero, map, manaPotion->m_pos, kLootPathStopRange)) {
            cb.setStateFn(AutoHuntState::LootNearby, "Moving to nearby mana potion for Stigma");
        } else {
            cb.setStateFn(AutoHuntState::LootNearby, "Settling on nearby mana potion for Stigma");
        }
        return true;
    }

    hero->MagicAttack(stigma->GetMagicType(), hero->GetID(), hero->m_posMap);
    m_lastStigmaTick = now;
    cb.setStateFn(AutoHuntState::Recover, "Casting Stigma");
    return true;
}

int HuntBuffManager::CountPotionInventory(const CHero* hero, bool manaPotion) const
{
    if (!hero)
        return 0;

    int count = 0;
    for (const auto& itemRef : hero->m_deqItem) {
        if (!itemRef)
            continue;
        const ItemTypeInfo* info = GetItemTypeInfo(itemRef->GetTypeID());
        if (!info || !IsConsumablePotionType(*info, manaPotion))
            continue;
        ++count;
    }
    return count;
}

CItem* HuntBuffManager::FindPotion(const CHero* hero, bool manaPotion) const
{
    if (!hero)
        return nullptr;

    CItem* best = nullptr;
    uint32_t bestValue = (std::numeric_limits<uint32_t>::max)();
    for (const auto& itemRef : hero->m_deqItem) {
        if (!itemRef)
            continue;
        const ItemTypeInfo* info = GetItemTypeInfo(itemRef->GetTypeID());
        if (!info)
            continue;
        if (!IsConsumablePotionType(*info, manaPotion))
            continue;
        const uint32_t value = manaPotion ? info->mana : info->life;
        if (value == 0)
            continue;
        if (!best || value < bestValue) {
            best = itemRef.get();
            bestValue = value;
        }
    }
    return best;
}

std::shared_ptr<CMapItem> HuntBuffManager::FindNearbyPotionLoot(
    CHero* hero, CGameMap* map, const AutoHuntSettings& settings, bool manaPotion,
    const HuntBuffCallbacks& cb) const
{
    if (!hero || !map || hero->IsBagFull())
        return nullptr;
    if (CountPotionInventory(hero, manaPotion) >= kNearbyPotionCarryLimit)
        return nullptr;

    const DWORD now = GetTickCount();
    const DWORD spawnGraceMs = GetLootSpawnGraceMs(settings);
    std::shared_ptr<CMapItem> best;
    float bestDist = (std::numeric_limits<float>::max)();
    for (const auto& itemRef : map->m_vecItems) {
        if (!itemRef)
            continue;

        if (cb.isLootPickupIgnoredFn && cb.isLootPickupIgnoredFn(itemRef->m_id, now))
            continue;

        const ItemTypeInfo* info = GetItemTypeInfo(itemRef->m_idType);
        if (!info || !IsConsumablePotionType(*info, manaPotion))
            continue;

        const float dist = hero->m_posMap.DistanceTo(itemRef->m_pos);
        if (dist > (float)kNearbyPotionRange)
            continue;

        if (dist < bestDist) {
            bestDist = dist;
            best = itemRef;
        }
    }

    return best;
}

bool HuntBuffManager::TryUsePotions(CHero* hero, const AutoHuntSettings& settings,
    int lastHp, int lastMaxHp, int lastMana, int lastMaxMana,
    const HuntBuffCallbacks& cb)
{
    if (!settings.usePotions || !hero)
        return false;

    const DWORD now = GetTickCount();
    if (now - m_lastPotionTick < GetItemActionIntervalMs(settings))
        return false;

    const int hpPercent = lastMaxHp > 0 ? (lastHp * 100) / lastMaxHp : 100;
    const int mpPercent = lastMaxMana > 0 ? (lastMana * 100) / lastMaxMana : 100;

    if (hpPercent <= settings.hpPotionPercent) {
        if (CItem* hpPotion = FindPotion(hero, false)) {
            hero->UseItem(hpPotion->GetID());
            m_lastPotionTick = now;
            cb.setStateFn(AutoHuntState::Recover, "Using HP potion");
            return true;
        }

        if (settings.pickupNearbyHpPotionWhenLow) {
            CGameMap* map = Game::GetMap();
            if (map) {
                std::shared_ptr<CMapItem> hpPotionLoot = FindNearbyPotionLoot(hero, map, settings, false, cb);
                if (hpPotionLoot) {
                    const int lootDist = CGameMap::TileDist(hero->m_posMap.x, hero->m_posMap.y,
                        hpPotionLoot->m_pos.x, hpPotionLoot->m_pos.y);
                    const int lootRange = (std::clamp)(settings.lootRange, 0, CGameMap::MAX_JUMP_DIST);
                    const bool inRange  = lootRange > 0 ? lootDist <= lootRange : lootDist == 0;
                    if (inRange && cb.tryPickupLootItemFn && cb.tryPickupLootItemFn(hero, hpPotionLoot, now)) {
                        cb.setTargetId(hpPotionLoot->m_id);
                        cb.setStateFn(AutoHuntState::LootNearby, "Picking up nearby HP potion");
                        return true;
                    }

                    cb.setTargetId(hpPotionLoot->m_id);
                    if (cb.startPathNearTargetFn && cb.startPathNearTargetFn(hero, map, hpPotionLoot->m_pos, kLootPathStopRange)) {
                        cb.setStateFn(AutoHuntState::LootNearby, "Moving to nearby HP potion");
                    } else {
                        cb.setStateFn(AutoHuntState::LootNearby, "Settling on nearby HP potion");
                    }
                    return true;
                }
            }
        }
    }

    if (mpPercent <= settings.manaPotionPercent) {
        if (CItem* mpPotion = FindPotion(hero, true)) {
            hero->UseItem(mpPotion->GetID());
            m_lastPotionTick = now;
            cb.setStateFn(AutoHuntState::Recover, "Using mana potion");
            return true;
        }
    }

    return false;
}

bool HuntBuffManager::TryPreLandingSafety(CHero* hero, CGameMap* map,
    const AutoHuntSettings& settings, const HuntBuffCallbacks& cb)
{
    if (!hero || !map || !IsArcherModeEnabled(settings))
        return false;
    if (!hero->IsFlyActive())
        return false;
    if (settings.archerSafetyDistance <= 0)
        return false;

    const DWORD remaining = GetRemainingFlyMs();
    if (remaining == 0 || remaining > FLY_SAFETY_BUFFER_MS)
        return false;
    if (CanRecastAnyFly(hero, settings))
        return false;

    if (hero->IsJumping())
        return false;

    const int safetyDist = settings.archerSafetyDistance;
    const int requiredThreatDist = GetRequiredArcherThreatDistance(safetyDist);
    const std::vector<CRole*> targets = cb.collectHuntTargetsFn
        ? cb.collectHuntTargetsFn(settings)
        : CollectHuntTargets(settings);

    bool threatened = false;
    for (CRole* t : targets) {
        if (t && CGameMap::TileDist(hero->m_posMap.x, hero->m_posMap.y,
                t->m_posMap.x, t->m_posMap.y) < requiredThreatDist) {
            threatened = true;
            break;
        }
    }
    if (!threatened)
        return false;

    Position retreatPos = {};
    if (cb.findSafeArcherRetreatFn &&
        cb.findSafeArcherRetreatFn(hero, map, settings, targets, nullptr, retreatPos, safetyDist)) {
        if (Pathfinder::Get().IsActive())
            Pathfinder::Get().Stop();
        const DWORD now = GetTickCount();
        hero->Jump(retreatPos.x, retreatPos.y);
        if (cb.recordMoveTick)
            cb.recordMoveTick();
        if (cb.armPendingJumpFn)
            cb.armPendingJumpFn(hero, retreatPos, now, true);
        cb.setStateFn(AutoHuntState::ApproachTarget, "Retreating before Fly expires");
        m_preLandingRetreat = true;
        return true;
    }
    return false;
}
