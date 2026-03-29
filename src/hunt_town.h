#pragma once
#include "base.h"
#include "hunt_settings.h"
#include <functional>
#include <cstddef>

class CHero;
class CGameMap;
class CItem;
struct CMapItem;

// ── Callback bundle passed to HuntTownService handlers ───────────────────────
// The parent plugin fills these lambdas so the service can call back for
// operations that live on BaseHuntPlugin (state transitions, pathing).
struct HuntTownCallbacks {
    // Set the plugin's current state and status text.
    std::function<void(AutoHuntState, const char*)> setStateFn;
    // Walk/path the hero to a tile near targetPos within desiredRange.
    std::function<bool(CHero*, CGameMap*, const Position&, int)> startPathNearTargetFn;
    // Begin traveling to the hunt zone (handles arrows, zone entry, etc.).
    std::function<void()> beginTravelToZoneFn;
    // Begin traveling to Market (resets sequences, decides repair/store).
    std::function<void()> beginTravelToMarketFn;
};

// ── HuntTownService ───────────────────────────────────────────────────────────
// Encapsulates the three town-run state machines: repair, buy arrows, store.
// Lives as a member of BaseHuntPlugin and is driven each Update() frame.
class HuntTownService {
public:
    // ── Phase enums ──────────────────────────────────────────────────────────
    enum class RepairPhase {
        MoveToNpc,
        Unequip,
        WaitUnequip,
        Repair,
        WaitRepair,
        Reequip,
        WaitReequip,
    };

    enum class BuyArrowsPhase {
        MoveToBlacksmith,
        BuyArrow,
        WaitBuy,
        EquipArrows,
        WaitEquip,
    };

    enum class StorePhase {
        PackMeteors,
        WaitPackMeteors,
        MoveToWarehouse,
        OpenWarehouse,
        WaitWarehouseOpen,
        DepositWarehouse,
        WaitWarehouseDeposit,
        DepositSilver,
        WaitSilverDeposit,
        MoveToTreasureBank,
        OpenTreasureBank,
        WaitTreasureBank,
        DepositTreasureMeteors,
        WaitTreasureMeteors,
        DepositTreasureDragonBalls,
        WaitTreasureDragonBalls,
        MoveToComposeBank,
        OpenComposeBank,
        WaitComposeBank,
        DepositComposeBank,
        WaitComposeBankDeposit,
    };

    // ── State machine handlers ────────────────────────────────────────────────
    // Each handler drives one town-service phase per frame.  Calls back into
    // the plugin via the HuntTownCallbacks bundle for state changes and pathing.
    void HandleRepairState(CHero* hero, CGameMap* map,
        const AutoHuntSettings& settings, const HuntTownCallbacks& cb);

    void HandleBuyArrowsState(CHero* hero, CGameMap* map,
        const AutoHuntSettings& settings, const HuntTownCallbacks& cb);

    void HandleStoreState(CHero* hero, CGameMap* map,
        const AutoHuntSettings& settings, const HuntTownCallbacks& cb);

    // ── Reset helpers ─────────────────────────────────────────────────────────
    void ResetRepairSequence();
    void ResetBuyArrowsSequence();
    void ResetStoreSequence();

    // ── Decision helpers ──────────────────────────────────────────────────────
    // NeedTownRun takes a pre-computed needsArrows flag (caller owns archer logic).
    bool NeedTownRun(CHero* hero, const AutoHuntSettings& settings, bool needsArrows) const;
    bool NeedsRepair(const CHero* hero, const AutoHuntSettings& settings) const;
    bool NeedsStorage(const CHero* hero, const AutoHuntSettings& settings) const;

    // ── Arrow helpers ─────────────────────────────────────────────────────────
    bool NeedsArrows(const CHero* hero, const AutoHuntSettings& settings) const;
    int  CountUsableArrowPacks(const CHero* hero) const;

    // ── Item predicates ───────────────────────────────────────────────────────
    static bool IsSelectedLootItem(const AutoHuntSettings& settings, uint32_t typeId);
    static bool IsSelectedWarehouseItem(const AutoHuntSettings& settings, uint32_t typeId);
    static bool IsSelectedPriorityReturnItem(const AutoHuntSettings& settings, uint32_t typeId);
    static bool ShouldLootMapItem(const AutoHuntSettings& settings, const CMapItem& item);

    bool IsTreasureBankDragonBallFamily(const CItem& item) const;
    bool IsTreasureBankMeteorFamily(const CItem& item) const;
    bool IsTreasureBankItem(const CItem& item) const;
    bool IsComposeBankItem(const CItem& item) const;

    bool HasTreasureBankItems(const CHero* hero) const;
    bool HasTreasureBankDragonBallItems(const CHero* hero) const;
    bool HasTreasureBankMeteorItems(const CHero* hero) const;
    bool HasComposeBankItems(const CHero* hero) const;
    bool HasPriorityReturnItems(const CHero* hero, const AutoHuntSettings& settings) const;
    bool HasWarehouseItems(const CHero* hero, const AutoHuntSettings& settings) const;
    bool ShouldStoreWarehouseItem(const AutoHuntSettings& settings, const CItem& item) const;

    // ── Map query helpers ─────────────────────────────────────────────────────
    // Returns true if a known blacksmith NPC entry exists for the given map.
    static bool HasBlacksmithOnMap(OBJID mapId);

    // ── Accessors for state the parent plugin may need ───────────────────────
    RepairPhase    GetRepairPhase()    const { return m_repairPhase; }
    BuyArrowsPhase GetBuyArrowsPhase() const { return m_buyArrowsPhase; }
    StorePhase     GetStorePhase()     const { return m_storePhase; }

    // Shared NPC-action throttle tick: parent plugin reads/writes this so the
    // timer is consistent with other NPC actions the plugin performs.
    DWORD m_lastNpcActionTick = 0;

private:
    RepairPhase    m_repairPhase    = RepairPhase::MoveToNpc;
    BuyArrowsPhase m_buyArrowsPhase = BuyArrowsPhase::MoveToBlacksmith;
    StorePhase     m_storePhase     = StorePhase::PackMeteors;

    OBJID m_repairNpcId        = 0;
    OBJID m_blacksmithNpcId    = 0;
    OBJID m_warehouseNpcId     = 0;
    OBJID m_treasureBankNpcId  = 0;
    OBJID m_composeBankNpcId   = 0;
    OBJID m_repairItemId       = 0;
    OBJID m_storeItemId        = 0;
    int   m_repairSlot         = 0;
    int   m_arrowsBoughtCount  = 0;
    int   m_storeMeteorCountBefore = 0;
};
