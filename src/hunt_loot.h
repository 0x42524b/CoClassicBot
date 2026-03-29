#pragma once
#include "base.h"
#include "hunt_settings.h"
#include <functional>
#include <memory>
#include <unordered_map>

class CHero;
class CGameMap;
struct CMapItem;

// ── HuntLootManager ───────────────────────────────────────────────────────────
// Encapsulates loot finding, pickup tracking, and pickup attempt throttling.
// Lives as a member of BaseHuntPlugin and is driven each Update() frame.
class HuntLootManager {
public:
    // Find the best loot item on the current map given the hunt zone and settings.
    std::shared_ptr<CMapItem> FindBestLoot(CHero* hero, CGameMap* map,
        const AutoHuntSettings& settings,
        std::function<bool(OBJID, DWORD)> isLootPickupIgnoredFn,
        std::function<bool(OBJID mapId, const Position&)> isPointInZoneFn) const;

    // Attempt to pick up an item the hero is standing on.
    // updatePendingJumpFn should call plugin's UpdatePendingJumpState and return its result.
    bool TryPickupLootItem(CHero* hero, const AutoHuntSettings& settings,
        const std::shared_ptr<CMapItem>& item, DWORD now,
        std::function<bool(DWORD)> updatePendingJumpFn);

    // ── Pickup-attempt tracking ───────────────────────────────────────────────
    bool IsLootPickupIgnored(OBJID itemId, DWORD now) const;
    void RecordLootPickupAttempt(OBJID itemId, DWORD now,
        const AutoHuntSettings& settings);
    void PruneLootPickupAttempts(CGameMap* map);
    void ResetLootPickupAttempts();

    // ── State accessors ───────────────────────────────────────────────────────
    DWORD GetLastLootTick()   const { return m_lastLootTick; }
    void  SetLastLootTick(DWORD t) { m_lastLootTick = t; }
    OBJID GetLastLootItemId() const { return m_lastLootItemId; }

    // Expose seen-ticks map so callers (e.g., HuntBuffManager) can pass it via lambda.
    mutable std::unordered_map<OBJID, DWORD> m_lootSeenTicks;

private:
    struct LootPickupAttemptState {
        uint8_t attempts       = 0;
        DWORD   ignoreUntilTick = 0;
    };

    std::unordered_map<OBJID, LootPickupAttemptState> m_lootPickupAttempts;
    DWORD m_lastLootTick   = 0;
    OBJID m_lastLootItemId = 0;
};
