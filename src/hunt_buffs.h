#pragma once
#include "base.h"
#include "hunt_settings.h"
#include <functional>
#include <memory>
#include <vector>

class CHero;
class CGameMap;
class CItem;
class CRole;
struct CMapItem;

// ── HuntBuffCallbacks ─────────────────────────────────────────────────────────
// Callbacks the parent plugin fills so HuntBuffManager can drive state
// transitions and pathing operations that live on BaseHuntPlugin.
struct HuntBuffCallbacks {
    // Set the plugin's current state and status text.
    std::function<void(AutoHuntState, const char*)> setStateFn;
    // Walk/path the hero to a tile near targetPos within desiredRange.
    std::function<bool(CHero*, CGameMap*, const Position&, int)> startPathNearTargetFn;
    // Arm the pending-jump tracker (hero, dest, now, isRetreat).
    std::function<void(CHero*, const Position&, DWORD, bool)> armPendingJumpFn;
    // Write the plugin's m_lastMoveTick and return current tick.
    std::function<DWORD()> recordMoveTick;
    // Receive the target ID for loot pickup side-effects.
    std::function<void(OBJID)> setTargetId;
    // Check whether a loot item is currently ignored (item pickup throttle).
    std::function<bool(OBJID, DWORD)> isLootPickupIgnoredFn;
    // Try to pick up a loot item directly (same-tile pickup).
    std::function<bool(CHero*, const std::shared_ptr<CMapItem>&, DWORD)> tryPickupLootItemFn;
    // Find a safe archer retreat position.
    std::function<bool(CHero*, CGameMap*, const AutoHuntSettings&,
        const std::vector<CRole*>&, CRole*, Position&, int)> findSafeArcherRetreatFn;
    // Collect current hunt targets (mobs).
    std::function<std::vector<CRole*>(const AutoHuntSettings&)> collectHuntTargetsFn;
};

// ── HuntBuffManager ───────────────────────────────────────────────────────────
// Encapsulates buff casting, potion management, and pre-landing safety retreat.
// Lives as a member of BaseHuntPlugin and is driven each Update() frame.
class HuntBuffManager {
public:
    bool TryCastSuperman(CHero* hero, const AutoHuntSettings& settings,
        const HuntBuffCallbacks& cb);
    bool TryCastCyclone(CHero* hero, const AutoHuntSettings& settings,
        const HuntBuffCallbacks& cb);
    bool TryCastAccuracy(CHero* hero, const AutoHuntSettings& settings,
        const HuntBuffCallbacks& cb);
    bool TryCastXpFly(CHero* hero, const AutoHuntSettings& settings,
        const HuntBuffCallbacks& cb);
    bool TryCastFly(CHero* hero, const AutoHuntSettings& settings,
        const HuntBuffCallbacks& cb);
    bool TryCastStigma(CHero* hero, const AutoHuntSettings& settings,
        int lastMana, const HuntBuffCallbacks& cb);
    bool TryUsePotions(CHero* hero, const AutoHuntSettings& settings,
        int lastHp, int lastMaxHp, int lastMana, int lastMaxMana,
        const HuntBuffCallbacks& cb);
    bool TryPreLandingSafety(CHero* hero, CGameMap* map,
        const AutoHuntSettings& settings, const HuntBuffCallbacks& cb);

    bool CanRecastAnyFly(CHero* hero, const AutoHuntSettings& settings) const;
    DWORD GetRemainingFlyMs() const;
    void RefreshBuffState(CHero* hero);

    int CountPotionInventory(const CHero* hero, bool manaPotion) const;
    CItem* FindPotion(const CHero* hero, bool manaPotion) const;
    std::shared_ptr<CMapItem> FindNearbyPotionLoot(CHero* hero, CGameMap* map,
        const AutoHuntSettings& settings, bool manaPotion,
        const HuntBuffCallbacks& cb) const;

    // State accessors
    bool IsXpSkillReady()      const { return m_xpSkillReady; }
    bool IsSupermanActive()    const { return m_supermanActive; }
    bool IsCycloneActive()     const { return m_cycloneActive; }
    bool IsFlyActive()         const { return m_flyActive; }
    bool IsPreLandingRetreat() const { return m_preLandingRetreat; }
    void SetPreLandingRetreat(bool v) { m_preLandingRetreat = v; }

    DWORD GetLastSupermanTick()  const { return m_lastSupermanTick; }
    DWORD GetLastCycloneTick()   const { return m_lastCycloneTick; }
    DWORD GetLastStigmaTick()    const { return m_lastStigmaTick; }
    DWORD GetLastPotionTick()    const { return m_lastPotionTick; }

private:
    bool m_xpSkillReady    = false;
    bool m_supermanKnown   = false;
    bool m_supermanActive  = false;
    bool m_cycloneKnown    = false;
    bool m_cycloneActive   = false;
    bool m_flyKnown        = false;
    bool m_flyActive       = false;
    DWORD m_flyStartTick   = 0;
    DWORD m_flyDurationMs  = 0;
    bool m_preLandingRetreat = false;
    DWORD m_lastSupermanTick = 0;
    DWORD m_lastCycloneTick  = 0;
    DWORD m_lastStigmaTick   = 0;
    DWORD m_lastPotionTick   = 0;
};
