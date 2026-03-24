#pragma once
#include "plugin.h"
#include "base.h"
#include "revive_utils.h"
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

enum class MiningState {
    Idle,
    WaitingForGame,
    TravelToMine,
    MoveToSpot,
    StartMining,
    Mining,
    TravelToMarket,
    StoreItems,
    Recover,
    ReturnToMine,
    Failed,
};

struct MiningSettings
{
    bool enabled = false;
    bool autoReviveInTown = true;
    bool useTwinCityWarehouse = false;
    bool useTwinCityGate = false;
    bool buyTwinCityGates = false;
    bool tradeReturnItemsToMule = false;
    int twinCityGateTargetCount = 1;
    int dropItemThreshold = 36;
    int townBagThreshold = 0;
    int movementIntervalMs = 900;
    OBJID mineMapId = 0;
    Position minePos = {0, 0};
    char muleName[64] = "";
    std::vector<uint32_t> returnItemIds;
    std::vector<uint32_t> depositItemIds;
    std::vector<uint32_t> sellItemIds;
    std::vector<uint32_t> dropItemIds;
};

MiningSettings& GetMiningSettings();

class CHero;
class CGameMap;
class CRole;
class CItem;
class TravelPlugin;

class MiningPlugin : public IPlugin {
public:
    const char* GetName() const override { return "Mining"; }
    void Update() override;
    void RenderUI() override;
    bool OnMapClick(const Position& tile) override;

private:
    enum class StorePhase {
        MoveToSellNpc,
        OpenSellNpc,
        WaitSellNpcOpen,
        SellItem,
        WaitSellItem,
        MoveToWarehouse,
        OpenWarehouse,
        WaitWarehouseOpen,
        WithdrawWarehouse,
        WaitWarehouseWithdraw,
        DepositWarehouse,
        WaitWarehouseDeposit,
        MoveToMule,
        StartTrade,
        WaitTradeStart,
        OfferTradeItem,
        WaitTradeItem,
        ConfirmTrade,
    };

    void SetState(MiningState state, const char* statusText);
    const char* GetStateName() const;
    void RefreshRuntimeState(CHero* hero, CGameMap* map);
    void StopAutomation(bool cancelTravel);
    bool HandleDeath(CHero* hero, TravelPlugin* travel, const MiningSettings& settings);
    bool HasValidMineSpot(const MiningSettings& settings) const;
    bool HasValidMuleName(const MiningSettings& settings) const;
    bool ShouldUseMuleTrade(const MiningSettings& settings) const;
    CItem* FindWarehouseDepositItem(const MiningSettings& settings) const;
    bool IsSelectedReturnItem(const MiningSettings& settings, uint32_t typeId) const;
    bool IsSelectedDepositItem(const MiningSettings& settings, uint32_t typeId) const;
    bool IsSelectedSellItem(const MiningSettings& settings, uint32_t typeId) const;
    bool IsSelectedDropItem(const MiningSettings& settings, uint32_t typeId) const;
    bool HasReturnItems(const CHero* hero, const MiningSettings& settings) const;
    bool HasDepositItems(const CHero* hero, const MiningSettings& settings) const;
    bool HasSellItems(const CHero* hero, const MiningSettings& settings) const;
    bool ShouldStartTownRunByBagThreshold(const CHero* hero, const MiningSettings& settings) const;
    bool ShouldIgnoreReturnTownRun(const CHero* hero, const MiningSettings& settings) const;
    bool ShouldHoldForReturnItem(const CHero* hero, const MiningSettings& settings) const;
    bool ShouldUseTwinCitySellLoop(const CHero* hero, const MiningSettings& settings) const;
    int GetTwinCityGateCount(const CHero* hero) const;
    OBJID GetStoreTargetMapId(const CHero* hero, const MiningSettings& settings) const;
    Position GetStoreTravelPos(const CHero* hero, const MiningSettings& settings) const;
    const char* GetStoreTownName(const CHero* hero, const MiningSettings& settings) const;
    const char* GetStoreEntryStatus(const CHero* hero, const MiningSettings& settings) const;
    bool ShouldStoreItem(const MiningSettings& settings, const CItem& item) const;
    bool ShouldSellItem(const MiningSettings& settings, const CItem& item) const;
    bool ShouldDropItem(const MiningSettings& settings, const CItem& item) const;
    bool HasDroppableItems(const CHero* hero, const MiningSettings& settings) const;
    bool ShouldBuyTwinCityGate(const CHero* hero, const MiningSettings& settings) const;
    bool HasMiningSignal(const CHero* hero) const;
    bool TryUseReturnShortcut(CHero* hero, const MiningSettings& settings);
    bool TryDropFilteredItem(CHero* hero, CGameMap* map, const MiningSettings& settings);
    void BeginManualWarehouseMuleRun(TravelPlugin* travel, CHero* hero, const MiningSettings& settings);
    void ResetMiningSession();
    void ResetReturnShortcut();
    void ResetItemTracking();
    void ResetManualWarehouseMuleRun();
    void SyncInventoryTracker(const CHero* hero, const MiningSettings& settings);
    void ResetTradeSession();
    bool MoveToMineSpot(CHero* hero, CGameMap* map, const Position& destination);
    bool StartPathTo(CHero* hero, CGameMap* map, const Position& destination, int stopRange);
    bool StartPathNearTarget(CHero* hero, CGameMap* map, const Position& targetPos, int desiredRange);
    CRole* FindPlayerNearSpot(const Position& expectedPos, int radius, const char* playerName) const;
    void BeginTravelToMine(TravelPlugin* travel, const MiningSettings& settings);
    void BeginTravelToMarket(TravelPlugin* travel, CHero* hero, const MiningSettings& settings);
    void HandleTravelToMine(TravelPlugin* travel, const MiningSettings& settings);
    void HandleTravelToMarket(TravelPlugin* travel, CHero* hero, const MiningSettings& settings);
    void ResetStoreSequence();
    void CommitPendingTradeTransfers();
    void BeginWarehouseFallback(const char* statusText);
    void HandleStoreState(CHero* hero, CGameMap* map, TravelPlugin* travel, const MiningSettings& settings);
    void RenderItemSelector(MiningSettings& settings);

    MiningState m_state = MiningState::Idle;
    StorePhase m_storePhase = StorePhase::MoveToWarehouse;
    char m_statusText[128] = "Disabled";
    char m_itemSearch[64] = "";
    bool m_captureMinePos = false;
    Position m_lastHeroPos = {0, 0};
    OBJID m_lastMapId = 0;
    size_t m_lastBagCount = 0;
    DWORD m_lastMineTick = 0;
    DWORD m_lastNpcActionTick = 0;
    ReviveState m_reviveState;
    DWORD m_returnShortcutTick = 0;
    DWORD m_lastDropTick = 0;
    OBJID m_twinCityPharmacistId = 0;
    OBJID m_sellNpcId = 0;
    OBJID m_warehouseNpcId = 0;
    OBJID m_storeItemId = 0;
    uint32_t m_storeItemTypeId = 0;
    OBJID m_dropItemId = 0;
    OBJID m_tradePartnerId = 0;
    int m_tradeOfferedCount = 0;
    bool m_miningSessionActive = false;
    bool m_waitingForReturnShortcut = false;
    bool m_dropCleanupActive = false;
    bool m_muleFallbackToWarehouse = false;
    bool m_sellUseTwinCityShopkeeper = false;
    bool m_storageUseMuleTrade = false;
    bool m_manualWarehouseMuleRunActive = false;
    int m_gateCountBeforeBuy = -1;
    DWORD m_manualMuleWaitStartTick = 0;
    DWORD m_tradeSessionTick = 0;
    OBJID m_trackedHeroId = 0;
    std::unordered_map<OBJID, uint32_t> m_lastInventoryTypeByUid;
    std::unordered_map<uint32_t, int> m_obtainedItemCounts;
    std::unordered_map<uint32_t, int> m_depositedItemCounts;
    std::unordered_map<uint32_t, int> m_pendingTradeTransferCounts;
    std::unordered_set<OBJID> m_tradeOfferedItemIds;
};
