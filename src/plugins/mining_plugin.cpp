#include "mining_plugin.h"
#include "inventory_utils.h"
#include "npc_utils.h"
#include "revive_utils.h"
#include "plugin_mgr.h"
#include "travel_plugin.h"
#include "discord.h"
#include "game.h"
#include "gateway.h"
#include "CHero.h"
#include "CEntityInfo.h"
#include "CGameMap.h"
#include "CRole.h"
#include "itemtype.h"
#include "packets.h"
#include "pathfinder.h"
#include "log.h"
#include "imgui.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <limits>
#include <string>

namespace {
const Position kMarketLandingPos = {211, 196};
const Position kWarehousePos = {182, 180};
const Position kMarketPharmacistPos = {198, 181};
const Position kTwinCityShopkeeperPos = {415, 351};
const Position kTwinCityWarehousePos = {409, 351};
const Position kTwinCityPharmacistPos = {466, 327};
constexpr uint32_t kMarketTravelSilverCost = 100;
constexpr uint32_t kTwinCityGateSilverCost = 200;
constexpr int kMinMovementIntervalMs = 100;
constexpr int kMaxMovementIntervalMs = 5000;
constexpr DWORD kMineActionIntervalMs = 1000;
constexpr DWORD kNpcActionIntervalMs = 400;
constexpr DWORD kDropItemIntervalMs = 800;
constexpr DWORD kDropItemRetryTimeoutMs = 2500;
constexpr DWORD kTradeStartIntervalMs = 1000;
constexpr DWORD kTradeOfferIntervalMs = 400;
constexpr DWORD kTradeConfirmIntervalMs = 1200;
constexpr DWORD kTradeBatchTimeoutMs = 8000;
constexpr DWORD kReviveDelayMs = 20000;
constexpr DWORD kReviveRetryIntervalMs = 1000;
constexpr DWORD kReturnShortcutTimeoutMs = 8000;
constexpr DWORD kManualMuleWaitTimeoutMs = 60000;
constexpr int kObservedMineStartCommand = 8;

DWORD GetMovementIntervalMs(const MiningSettings& settings)
{
    return static_cast<DWORD>(std::clamp(settings.movementIntervalMs, kMinMovementIntervalMs, kMaxMovementIntervalMs));
}

OBJID GetStorageMapId(const MiningSettings& settings, bool useMuleTrade)
{
    if (useMuleTrade)
        return MAP_MARKET;
    return settings.useTwinCityWarehouse ? MAP_TWIN_CITY : MAP_MARKET;
}

Position GetStorageTravelPos(const MiningSettings& settings, bool useMuleTrade)
{
    if (useMuleTrade)
        return kMarketLandingPos;
    return settings.useTwinCityWarehouse ? kTwinCityWarehousePos : kMarketLandingPos;
}

Position GetStorageWarehousePos(const MiningSettings& settings)
{
    return settings.useTwinCityWarehouse ? kTwinCityWarehousePos : kWarehousePos;
}

const char* GetStorageTownName(const MiningSettings& settings, bool useMuleTrade)
{
    if (useMuleTrade)
        return "Market";
    return settings.useTwinCityWarehouse ? "Twin City" : "Market";
}


CItem* FindTwinCityGate(const CHero* hero)
{
    if (!hero)
        return nullptr;

    for (const auto& itemRef : hero->m_deqItem) {
        if (itemRef && itemRef->IsTwinCityGate())
            return itemRef.get();
    }
    return nullptr;
}

std::string ToLowerCopy(const std::string& value)
{
    std::string lower = value;
    std::transform(lower.begin(), lower.end(), lower.begin(),
        [](unsigned char c) { return (char)std::tolower(c); });
    return lower;
}

bool IsZeroPos(const Position& pos)
{
    return pos.x == 0 && pos.y == 0;
}

bool IsMovementCommand(const CCommand& cmd)
{
    return cmd.iType == _COMMAND_WALK
        || cmd.iType == _COMMAND_RUN
        || cmd.iType == _COMMAND_WALKFORWARD
        || cmd.iType == _COMMAND_RUNFORWARD
        || cmd.iType == _COMMAND_JUMP;
}

bool IsMovementCommandStillAdvancing(const CHero* hero)
{
    if (!hero)
        return false;

    const CCommand& cmd = hero->GetCommand();
    if (!IsMovementCommand(cmd))
        return false;

    return cmd.posTarget.x != hero->m_posMap.x || cmd.posTarget.y != hero->m_posMap.y;
}

bool HasGroundItemOnTile(const CGameMap* map, const Position& pos)
{
    if (!map)
        return false;

    for (const auto& itemRef : map->m_vecItems) {
        if (!itemRef)
            continue;
        if (itemRef->m_pos.x == pos.x && itemRef->m_pos.y == pos.y)
            return true;
    }
    return false;
}

int CountFreeDropTilesAround(const CGameMap* map, const Position& origin)
{
    if (!map)
        return 0;

    int count = 0;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            const Position candidate = {origin.x + dx, origin.y + dy};
            if (!map->GetCell(candidate.x, candidate.y))
                continue;
            if (!map->IsWalkable(candidate.x, candidate.y))
                continue;
            if (IsTileOccupied(candidate.x, candidate.y))
                continue;
            if (HasGroundItemOnTile(map, candidate))
                continue;
            ++count;
        }
    }
    return count;
}

bool FindFreeDropTileAround(const CGameMap* map, const Position& origin, Position& out)
{
    if (!map)
        return false;

    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            const Position candidate = {origin.x + dx, origin.y + dy};
            if (!map->GetCell(candidate.x, candidate.y))
                continue;
            if (!map->IsWalkable(candidate.x, candidate.y))
                continue;
            if (IsTileOccupied(candidate.x, candidate.y))
                continue;
            if (HasGroundItemOnTile(map, candidate))
                continue;

            out = candidate;
            return true;
        }
    }

    return false;
}

bool FindNextDropAnchor(const CHero* hero, const CGameMap* map, Position& out)
{
    if (!hero || !map)
        return false;

    const Position origin = hero->m_posMap;
    struct DropAnchorCandidate {
        Position pos = {};
        int freeTiles = 0;
        int dist = (std::numeric_limits<int>::max)();
        bool valid = false;
    };

    auto considerCandidate = [&](DropAnchorCandidate& best, const Position& candidate, bool requireJump) {
        if (!map->GetCell(candidate.x, candidate.y))
            return;
        if (!map->IsWalkable(candidate.x, candidate.y))
            return;
        if (IsTileOccupied(candidate.x, candidate.y))
            return;

        const int dist = CGameMap::TileDist(origin.x, origin.y, candidate.x, candidate.y);
        if (dist <= 0)
            return;
        if (requireJump && (!map->CanJump(origin.x, origin.y, candidate.x, candidate.y, CGameMap::GetHeroAltThreshold()) || dist > CGameMap::MAX_JUMP_DIST))
            return;

        const int freeTiles = CountFreeDropTilesAround(map, candidate);
        if (freeTiles <= 0)
            return;

        if (!best.valid || freeTiles > best.freeTiles || (freeTiles == best.freeTiles && dist < best.dist)) {
            best.pos = candidate;
            best.freeTiles = freeTiles;
            best.dist = dist;
            best.valid = true;
        }
    };

    auto findBestAnchor = [&](int maxRadius, bool requireJump, Position& bestOut) {
        DropAnchorCandidate best = {};
        for (int dy = -maxRadius; dy <= maxRadius; ++dy) {
            for (int dx = -maxRadius; dx <= maxRadius; ++dx) {
                const Position candidate = {origin.x + dx, origin.y + dy};
                const int dist = CGameMap::TileDist(origin.x, origin.y, candidate.x, candidate.y);
                if (dist <= 0 || dist > maxRadius)
                    continue;
                considerCandidate(best, candidate, requireJump);
            }
        }

        if (!best.valid)
            return false;

        bestOut = best.pos;
        return true;
    };

    if (findBestAnchor(CGameMap::MAX_JUMP_DIST, true, out))
        return true;

    return findBestAnchor(CGameMap::MAX_JUMP_DIST + 6, false, out);
}

bool FindMineStandTile(const CHero* hero, const CGameMap* map, const Position& target, Position& out)
{
    if (!map)
        return false;

    auto isAvailable = [hero, map](const Position& pos) {
        if (!map->GetCell(pos.x, pos.y))
            return false;
        if (!map->IsWalkable(pos.x, pos.y))
            return false;
        if (hero && hero->m_posMap.x == pos.x && hero->m_posMap.y == pos.y)
            return true;
        return !IsTileOccupied(pos.x, pos.y);
    };

    for (int radius = 0; radius <= 4; ++radius) {
        Position best = {};
        int bestHeroDist = (std::numeric_limits<int>::max)();
        bool found = false;

        for (int dy = -radius; dy <= radius; ++dy) {
            for (int dx = -radius; dx <= radius; ++dx) {
                if (radius != 0 && (std::max)(abs(dx), abs(dy)) != radius)
                    continue;

                const Position candidate = {target.x + dx, target.y + dy};
                if (!isAvailable(candidate))
                    continue;

                const int heroDist = hero
                    ? CGameMap::TileDist(hero->m_posMap.x, hero->m_posMap.y, candidate.x, candidate.y)
                    : 0;
                if (!found || heroDist < bestHeroDist) {
                    best = candidate;
                    bestHeroDist = heroDist;
                    found = true;
                }
            }
        }

        if (found) {
            out = best;
            return true;
        }
    }

    return false;
}
}

static MiningSettings g_miningSettings;
MiningSettings& GetMiningSettings() { return g_miningSettings; }

static const char* StateName(MiningState state)
{
    switch (state) {
        case MiningState::Idle:           return "Idle";
        case MiningState::WaitingForGame: return "Waiting For Game";
        case MiningState::TravelToMine:   return "Travel To Mine";
        case MiningState::MoveToSpot:     return "Move To Spot";
        case MiningState::StartMining:    return "Start Mining";
        case MiningState::Mining:         return "Mining";
        case MiningState::TravelToMarket: return "Travel To Town";
        case MiningState::StoreItems:     return "Store Items";
        case MiningState::Recover:        return "Recover";
        case MiningState::ReturnToMine:   return "Return To Mine";
        case MiningState::Failed:         return "Failed";
        default:                          return "Unknown";
    }
}

void MiningPlugin::SetState(MiningState state, const char* statusText)
{
    if (m_state != state)
        spdlog::info("[mining] State: {} -> {} | {}", GetStateName(), StateName(state),
            statusText ? statusText : "");
    m_state = state;
    snprintf(m_statusText, sizeof(m_statusText), "%s", statusText ? statusText : "");
}

const char* MiningPlugin::GetStateName() const
{
    return StateName(m_state);
}

void MiningPlugin::RefreshRuntimeState(CHero* hero, CGameMap* map)
{
    m_lastHeroPos = hero ? hero->m_posMap : Position{};
    m_lastMapId = map ? map->GetId() : 0;
    m_lastBagCount = hero ? hero->m_deqItem.size() : 0;
}

void MiningPlugin::ResetItemTracking()
{
    m_lastInventoryTypeByUid.clear();
    m_obtainedItemCounts.clear();
    m_depositedItemCounts.clear();
    m_trackedHeroId = 0;
}

void MiningPlugin::ResetManualWarehouseMuleRun()
{
    m_manualWarehouseMuleRunActive = false;
    m_manualMuleWaitStartTick = 0;
}

void MiningPlugin::SyncInventoryTracker(const CHero* hero, const MiningSettings& settings)
{
    if (!hero || hero->GetID() == 0)
        return;

    if (m_trackedHeroId != hero->GetID()) {
        m_lastInventoryTypeByUid.clear();
        m_obtainedItemCounts.clear();
        m_depositedItemCounts.clear();
        m_trackedHeroId = hero->GetID();
        for (const auto& itemRef : hero->m_deqItem) {
            if (itemRef)
                m_lastInventoryTypeByUid[itemRef->GetID()] = itemRef->GetTypeID();
        }
        return;
    }

    std::unordered_map<OBJID, uint32_t> currentInventory;
    currentInventory.reserve(hero->m_deqItem.size());

    for (const auto& itemRef : hero->m_deqItem) {
        if (!itemRef)
            continue;

        currentInventory[itemRef->GetID()] = itemRef->GetTypeID();
        if (m_lastInventoryTypeByUid.find(itemRef->GetID()) == m_lastInventoryTypeByUid.end()) {
            ++m_obtainedItemCounts[itemRef->GetTypeID()];
        }
    }

    m_lastInventoryTypeByUid = std::move(currentInventory);
}

void MiningPlugin::StopAutomation(bool cancelTravel)
{
    Pathfinder::Get().Stop();
    if (cancelTravel) {
        if (auto* travel = PluginManager::Get().GetPlugin<TravelPlugin>(); travel && travel->IsTraveling())
            travel->CancelTravel();
    }

    m_reviveState = {};
    m_dropCleanupActive = false;
    m_dropItemId = 0;
    m_storageUseMuleTrade = false;
    ResetManualWarehouseMuleRun();
    ResetMiningSession();
    ResetReturnShortcut();
    ResetStoreSequence();
    ResetTradeSession();
    SetState(MiningState::Idle, "Disabled");
}

bool MiningPlugin::HandleDeath(CHero* hero, TravelPlugin* travel, const MiningSettings& settings)
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
        SetState(MiningState::Failed, "Hero is dead");
        return true;
    }

    // Plugin-specific first-death cleanup (before HandleRevive records deathTick)
    if (m_reviveState.deathTick == 0) {
        m_dropCleanupActive = false;
        m_dropItemId = 0;
        m_storageUseMuleTrade = false;
        ResetManualWarehouseMuleRun();
        ResetMiningSession();
        ResetReturnShortcut();
        if (travel && travel->IsTraveling())
            travel->CancelTravel();
        ResetStoreSequence();
        ResetTradeSession();
    }

    const bool handled = HandleRevive(hero, m_reviveState,
        kReviveDelayMs, kReviveRetryIntervalMs,
        settings.autoReviveInTown, m_statusText, sizeof(m_statusText));

    if (handled)
        m_state = MiningState::Recover;
    return handled;
}

bool MiningPlugin::HasValidMineSpot(const MiningSettings& settings) const
{
    return settings.mineMapId != 0 && !IsZeroPos(settings.minePos);
}

bool MiningPlugin::HasValidMuleName(const MiningSettings& settings) const
{
    return settings.muleName[0] != '\0';
}

bool MiningPlugin::ShouldUseMuleTrade(const MiningSettings& settings) const
{
    return settings.tradeReturnItemsToMule && !m_muleFallbackToWarehouse;
}

CItem* MiningPlugin::FindWarehouseDepositItem(const MiningSettings& settings) const
{
    CEntityInfo* entityInfo = Game::GetEntityInfo();
    if (!entityInfo)
        return nullptr;

    for (CItem* item : entityInfo->GetWarehouseItems()) {
        if (item && IsSelectedDepositItem(settings, item->GetTypeID()))
            return item;
    }

    return nullptr;
}

bool MiningPlugin::IsSelectedReturnItem(const MiningSettings& settings, uint32_t typeId) const
{
    return ContainsItemId(settings.returnItemIds, typeId);
}

bool MiningPlugin::IsSelectedDepositItem(const MiningSettings& settings, uint32_t typeId) const
{
    return ContainsItemId(settings.depositItemIds, typeId);
}

bool MiningPlugin::IsSelectedSellItem(const MiningSettings& settings, uint32_t typeId) const
{
    return ContainsItemId(settings.sellItemIds, typeId);
}

bool MiningPlugin::IsSelectedDropItem(const MiningSettings& settings, uint32_t typeId) const
{
    return ContainsItemId(settings.dropItemIds, typeId);
}

bool MiningPlugin::HasReturnItems(const CHero* hero, const MiningSettings& settings) const
{
    return InventoryHasMatchingItem(hero, [this, &settings](const CItem& item) {
        return IsSelectedReturnItem(settings, item.GetTypeID());
    });
}

bool MiningPlugin::HasDepositItems(const CHero* hero, const MiningSettings& settings) const
{
    return InventoryHasMatchingItem(hero, [this, &settings](const CItem& item) {
        return IsSelectedDepositItem(settings, item.GetTypeID());
    });
}

bool MiningPlugin::HasSellItems(const CHero* hero, const MiningSettings& settings) const
{
    return InventoryHasMatchingItem(hero, [this, &settings](const CItem& item) {
        return IsSelectedSellItem(settings, item.GetTypeID());
    });
}

bool MiningPlugin::ShouldStartTownRunByBagThreshold(const CHero* hero, const MiningSettings& settings) const
{
    if (!hero || settings.townBagThreshold <= 0)
        return false;

    const int bagThreshold = std::clamp(settings.townBagThreshold, 1, CHero::MAX_BAG_ITEMS);
    if ((int)hero->m_deqItem.size() < bagThreshold && !hero->IsBagFull())
        return false;

    return ShouldHoldForReturnItem(hero, settings)
        || HasDepositItems(hero, settings)
        || HasSellItems(hero, settings);
}

bool MiningPlugin::ShouldIgnoreReturnTownRun(const CHero* hero, const MiningSettings& settings) const
{
    return hero
        && HasReturnItems(hero, settings)
        && !HasSellItems(hero, settings)
        && hero->GetSilver() < kMarketTravelSilverCost;
}

bool MiningPlugin::ShouldHoldForReturnItem(const CHero* hero, const MiningSettings& settings) const
{
    return HasReturnItems(hero, settings) && !ShouldIgnoreReturnTownRun(hero, settings);
}

bool MiningPlugin::ShouldUseTwinCitySellLoop(const CHero* hero, const MiningSettings& settings) const
{
    return hero && HasSellItems(hero, settings) && hero->GetSilver() < kMarketTravelSilverCost;
}

int MiningPlugin::GetTwinCityGateCount(const CHero* hero) const
{
    if (!hero)
        return 0;

    int count = 0;
    for (const auto& itemRef : hero->m_deqItem) {
        if (itemRef && itemRef->IsTwinCityGate())
            ++count;
    }
    return count;
}

OBJID MiningPlugin::GetStoreTargetMapId(const CHero* hero, const MiningSettings& settings) const
{
    if (m_manualWarehouseMuleRunActive)
        return MAP_MARKET;
    if (HasSellItems(hero, settings))
        return m_sellUseTwinCityShopkeeper ? MAP_TWIN_CITY : MAP_MARKET;
    return GetStorageMapId(settings, m_storageUseMuleTrade);
}

Position MiningPlugin::GetStoreTravelPos(const CHero* hero, const MiningSettings& settings) const
{
    if (m_manualWarehouseMuleRunActive)
        return kMarketLandingPos;
    if (HasSellItems(hero, settings))
        return m_sellUseTwinCityShopkeeper ? kTwinCityShopkeeperPos : kMarketLandingPos;
    return GetStorageTravelPos(settings, m_storageUseMuleTrade);
}

const char* MiningPlugin::GetStoreTownName(const CHero* hero, const MiningSettings& settings) const
{
    if (m_manualWarehouseMuleRunActive)
        return "Market";
    if (HasSellItems(hero, settings))
        return m_sellUseTwinCityShopkeeper ? "Twin City" : "Market";
    return GetStorageTownName(settings, m_storageUseMuleTrade);
}

const char* MiningPlugin::GetStoreEntryStatus(const CHero* hero, const MiningSettings& settings) const
{
    if (m_manualWarehouseMuleRunActive)
        return "Checking warehouse for mule items";
    if (HasSellItems(hero, settings))
        return m_sellUseTwinCityShopkeeper ? "Moving to Shopkeeper" : "Moving to Pharmacist";
    return m_storageUseMuleTrade ? "Moving to mule" : "Moving to Warehouseman";
}

void MiningPlugin::BeginManualWarehouseMuleRun(
    TravelPlugin* travel, CHero* hero, const MiningSettings& settings)
{
    if (!travel || !hero) {
        SetState(MiningState::Failed, "Travel plugin not available");
        return;
    }

    Pathfinder::Get().Stop();
    ResetMiningSession();
    ResetReturnShortcut();
    ResetStoreSequence();
    ResetTradeSession();
    ResetManualWarehouseMuleRun();
    m_dropCleanupActive = false;
    m_dropItemId = 0;
    m_storageUseMuleTrade = false;
    m_sellUseTwinCityShopkeeper = false;
    m_manualWarehouseMuleRunActive = true;
    m_storePhase = StorePhase::MoveToWarehouse;

    if (m_lastMapId == MAP_MARKET) {
        SetState(MiningState::StoreItems, "Checking warehouse for mule items");
        return;
    }

    travel->StartTravel(MAP_MARKET, kMarketLandingPos);
    SetState(MiningState::TravelToMarket, "Traveling to Market for muling");
}


bool MiningPlugin::ShouldStoreItem(const MiningSettings& settings, const CItem& item) const
{
    return IsSelectedDepositItem(settings, item.GetTypeID());
}

bool MiningPlugin::ShouldSellItem(const MiningSettings& settings, const CItem& item) const
{
    return IsSelectedSellItem(settings, item.GetTypeID());
}

bool MiningPlugin::ShouldDropItem(const MiningSettings& settings, const CItem& item) const
{
    return IsSelectedDropItem(settings, item.GetTypeID())
        && !IsSelectedReturnItem(settings, item.GetTypeID())
        && !IsSelectedDepositItem(settings, item.GetTypeID())
        && !IsSelectedSellItem(settings, item.GetTypeID());
}

bool MiningPlugin::HasDroppableItems(const CHero* hero, const MiningSettings& settings) const
{
    return InventoryHasMatchingItem(hero, [this, &settings](const CItem& item) {
        return ShouldDropItem(settings, item);
    });
}

bool MiningPlugin::ShouldBuyTwinCityGate(const CHero* hero, const MiningSettings& settings) const
{
    return settings.useTwinCityGate
        && settings.buyTwinCityGates
        && settings.twinCityGateTargetCount > 0
        && GetTwinCityGateCount(hero) < settings.twinCityGateTargetCount
        && hero
        && (hero->GetSilver() >= kTwinCityGateSilverCost || HasSellItems(hero, settings));
}

bool MiningPlugin::HasMiningSignal(const CHero* hero) const
{
    if (!hero)
        return false;

    const CCommand& cmd = hero->GetCommand();
    if (cmd.iType == _COMMAND_MINE)
        return true;

    // Manual verification in the live client: mining transitions through
    // multiple ACCOMPLISH states while the pickaxe loop keeps running.
    return cmd.iStatus == _CMDSTATUS_ACCOMPLISH
        && (cmd.iType == kObservedMineStartCommand
            || cmd.iType == _COMMAND_ACTION);
}

void MiningPlugin::ResetMiningSession()
{
    m_miningSessionActive = false;
}

void MiningPlugin::ResetTradeSession()
{
    m_tradePartnerId = 0;
    m_tradeSessionTick = 0;
    m_tradeOfferedCount = 0;
    m_storeItemId = 0;
    m_storeItemTypeId = 0;
    m_pendingTradeTransferCounts.clear();
    m_tradeOfferedItemIds.clear();
}

void MiningPlugin::ResetReturnShortcut()
{
    m_waitingForReturnShortcut = false;
    m_returnShortcutTick = 0;
    m_twinCityPharmacistId = 0;
    m_gateCountBeforeBuy = -1;
}

bool MiningPlugin::TryUseReturnShortcut(CHero* hero, const MiningSettings& settings)
{
    if (!hero)
        return false;

    if (m_lastMapId == MAP_TWIN_CITY || m_lastMapId == MAP_MARKET
        || m_lastMapId == GetStorageMapId(settings, m_storageUseMuleTrade)) {
        return false;
    }

    if (settings.useTwinCityGate) {
        if (CItem* gate = FindTwinCityGate(hero)) {
            hero->UseItem(gate->GetID());
            m_waitingForReturnShortcut = true;
            m_returnShortcutTick = GetTickCount();
            SetState(MiningState::TravelToMarket, "Using TwinCityGate");
            return true;
        }
    }

    return false;
}

bool MiningPlugin::TryDropFilteredItem(CHero* hero, CGameMap* map, const MiningSettings& settings)
{
    if (!hero || !map)
        return false;

    const int dropThreshold = std::clamp(settings.dropItemThreshold, 1, CHero::MAX_BAG_ITEMS);
    const bool hasDroppableItems = HasDroppableItems(hero, settings);
    if (!m_dropCleanupActive) {
        if ((int)hero->m_deqItem.size() < dropThreshold || !hasDroppableItems)
            return false;
        m_dropCleanupActive = true;
    }

    if (!hasDroppableItems) {
        m_dropCleanupActive = false;
        m_dropItemId = 0;
        return false;
    }

    if (Pathfinder::Get().IsActive() || hero->IsJumping() || IsMovementCommandStillAdvancing(hero)) {
        snprintf(m_statusText, sizeof(m_statusText), "Moving to next drop tile");
        return true;
    }

    const DWORD now = GetTickCount();
    CItem* pendingDrop = m_dropItemId != 0 ? FindInventoryItemById(hero, m_dropItemId) : nullptr;
    if (m_dropItemId != 0 && !pendingDrop) {
        m_dropItemId = 0;
    } else if (pendingDrop) {
        if (now - m_lastDropTick <= kDropItemRetryTimeoutMs) {
            snprintf(m_statusText, sizeof(m_statusText), "Waiting for dropped item to leave inventory");
            return true;
        }

        m_dropItemId = 0;
    }
    if (now - m_lastDropTick < kDropItemIntervalMs)
        return true;

    CItem* candidate = nullptr;
    for (const auto& itemRef : hero->m_deqItem) {
        if (!itemRef || !ShouldDropItem(settings, *itemRef))
            continue;
        candidate = itemRef.get();
        break;
    }
    if (!candidate)
        return false;

    Position dropPos;
    if (!FindFreeDropTileAround(map, hero->m_posMap, dropPos)) {
        Position nextAnchor;
        if (FindNextDropAnchor(hero, map, nextAnchor) && StartPathTo(hero, map, nextAnchor, 0)) {
            snprintf(m_statusText, sizeof(m_statusText), "Moving to next drop tile");
            return true;
        }

        snprintf(m_statusText, sizeof(m_statusText), "No free drop tiles nearby");
        return true;
    }

    hero->DropItem(candidate->GetID(), dropPos);
    m_dropItemId = candidate->GetID();
    m_lastDropTick = now;
    snprintf(m_statusText, sizeof(m_statusText), "Dropping %s at (%d,%d) during cleanup", candidate->GetName(), dropPos.x, dropPos.y);
    return true;
}

bool MiningPlugin::MoveToMineSpot(CHero* hero, CGameMap* map, const Position& destination)
{
    if (!hero || !map)
        return false;

    const int dist = CGameMap::TileDist(hero->m_posMap.x, hero->m_posMap.y, destination.x, destination.y);
    if (dist <= 0)
        return false;

    const DWORD now = GetTickCount();
    if (now - m_lastMineTick < GetMovementIntervalMs(GetMiningSettings()))
        return true;

    if (Pathfinder::Get().IsActive())
        Pathfinder::Get().Stop();

    hero->Walk(destination.x, destination.y);
    m_lastMineTick = now;
    return true;
}

bool MiningPlugin::StartPathTo(CHero* hero, CGameMap* map, const Position& destination, int stopRange)
{
    if (!hero || !map)
        return false;

    const int hx = hero->m_posMap.x;
    const int hy = hero->m_posMap.y;
    const int dist = CGameMap::TileDist(hx, hy, destination.x, destination.y);
    if (dist <= stopRange)
        return false;

    if (Pathfinder::Get().IsActive() || hero->IsJumping())
        return true;

    const DWORD now = GetTickCount();
    if (now - m_lastMineTick < GetMovementIntervalMs(GetMiningSettings()))
        return true;

    if (dist <= CGameMap::MAX_JUMP_DIST
        && map->CanJump(hx, hy, destination.x, destination.y, CGameMap::GetHeroAltThreshold())
        && !IsTileOccupied(destination.x, destination.y)) {
        hero->Jump(destination.x, destination.y);
        m_lastMineTick = now;
        return true;
    }

    auto tilePath = map->FindPath(hx, hy, destination.x, destination.y, 1000000);
    if (tilePath.empty())
        return false;

    auto waypoints = map->SimplifyPath(tilePath);
    if (waypoints.empty())
        return false;

    Pathfinder::Get().StartPath(waypoints, GetMovementIntervalMs(GetMiningSettings()));
    m_lastMineTick = now;
    return true;
}

bool MiningPlugin::StartPathNearTarget(CHero* hero, CGameMap* map, const Position& targetPos, int desiredRange)
{
    if (!hero || !map)
        return false;

    const int currentDist = CGameMap::TileDist(hero->m_posMap.x, hero->m_posMap.y, targetPos.x, targetPos.y);
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

            const float heroDist = hero->m_posMap.DistanceTo(candidate);
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
        return StartPathTo(hero, map, targetPos, desiredRange);

    return StartPathTo(hero, map, bestPos, 0);
}


CRole* MiningPlugin::FindPlayerNearSpot(const Position& expectedPos, int radius, const char* playerName) const
{
    CRoleMgr* mgr = Game::GetRoleMgr();
    if (!mgr || mgr->m_deqRole.empty() || mgr->m_deqRole.size() >= 10000)
        return nullptr;

    CHero* hero = Game::GetHero();
    CRole* best = nullptr;
    float bestDist = (float)(radius + 1);
    for (size_t i = 0; i < mgr->m_deqRole.size() && i < 500; ++i) {
        const auto& roleRef = mgr->m_deqRole[i];
        if (!roleRef)
            continue;

        CRole* role = roleRef.get();
        if (!role->IsPlayer())
            continue;
        if (hero && role->GetID() == hero->GetID())
            continue;
        if (playerName && playerName[0] && _stricmp(role->GetName(), playerName) != 0)
            continue;

        const float dist = expectedPos.DistanceTo(role->m_posMap);
        if (dist < bestDist) {
            bestDist = dist;
            best = role;
        }
    }

    return best;
}

void MiningPlugin::BeginTravelToMine(TravelPlugin* travel, const MiningSettings& settings)
{
    if (!travel) {
        SetState(MiningState::Failed, "Travel plugin not available");
        return;
    }

    CHero* hero = Game::GetHero();
    const bool useMuleTrade = m_storageUseMuleTrade;
    ResetManualWarehouseMuleRun();
    ResetMiningSession();
    m_dropCleanupActive = false;
    m_dropItemId = 0;
    if (hero) {
        if (m_lastMapId == MAP_TWIN_CITY && ShouldBuyTwinCityGate(hero, settings)) {
            ResetReturnShortcut();
            SetState(MiningState::ReturnToMine, "Moving to Twin City Pharmacist");
            return;
        }

        if (m_lastMapId == GetStorageMapId(settings, useMuleTrade)
            && GetStorageMapId(settings, useMuleTrade) != MAP_TWIN_CITY
            && ShouldBuyTwinCityGate(hero, settings)) {
            ResetReturnShortcut();
            travel->StartTravel(MAP_TWIN_CITY, kTwinCityPharmacistPos);
            SetState(MiningState::ReturnToMine, "Traveling to Twin City Pharmacist");
            return;
        }
    }

    if (m_lastMapId == settings.mineMapId) {
        SetState(MiningState::MoveToSpot, "Moving to mine spot");
        return;
    }

    ResetReturnShortcut();
    travel->StartTravel(settings.mineMapId);
    SetState((m_lastMapId == GetStorageMapId(settings, useMuleTrade) || m_lastMapId == MAP_TWIN_CITY)
            ? MiningState::ReturnToMine
            : MiningState::TravelToMine,
        "Traveling to mine map");
}

void MiningPlugin::BeginTravelToMarket(TravelPlugin* travel, CHero* hero, const MiningSettings& settings)
{
    if (!travel || !hero) {
        SetState(MiningState::Failed, "Travel plugin not available");
        return;
    }

    Pathfinder::Get().Stop();
    ResetMiningSession();
    ResetReturnShortcut();
    ResetStoreSequence();
    ResetTradeSession();
    m_dropCleanupActive = false;
    m_dropItemId = 0;
    m_storageUseMuleTrade = ShouldUseMuleTrade(settings) && HasDepositItems(hero, settings);
    m_sellUseTwinCityShopkeeper = ShouldUseTwinCitySellLoop(hero, settings);

    if (m_lastMapId == GetStoreTargetMapId(hero, settings)) {
        SetState(MiningState::StoreItems, GetStoreEntryStatus(hero, settings));
        return;
    }

    if (TryUseReturnShortcut(hero, settings))
        return;

    travel->StartTravel(GetStoreTargetMapId(hero, settings), GetStoreTravelPos(hero, settings));
    char status[128];
    snprintf(status, sizeof(status), "Traveling to %s", GetStoreTownName(hero, settings));
    SetState(MiningState::TravelToMarket, status);
}

void MiningPlugin::HandleTravelToMine(TravelPlugin* travel, const MiningSettings& settings)
{
    if (!travel) {
        SetState(MiningState::Failed, "Travel plugin not available");
        return;
    }

    CHero* hero = Game::GetHero();
    CGameMap* map = Game::GetMap();
    if (!hero || !map) {
        SetState(MiningState::WaitingForGame, "Waiting for hero and map");
        return;
    }

    if (travel->GetState() == TravelState::Failed) {
        SetState(MiningState::Failed, "Failed to reach mine");
        return;
    }

    if (m_state == MiningState::ReturnToMine && m_lastMapId == MAP_TWIN_CITY
        && ShouldBuyTwinCityGate(hero, settings)) {
        if (travel->IsTraveling())
            travel->CancelTravel();
        if (Pathfinder::Get().IsActive())
            Pathfinder::Get().Stop();

        CRole* pharmacist = FindNpcByName("Pharmacist", kTwinCityPharmacistPos, 20);
        if (pharmacist)
            m_twinCityPharmacistId = pharmacist->GetID();

        const Position pharmacistPos = pharmacist ? pharmacist->m_posMap : kTwinCityPharmacistPos;
        const int npcDist = CGameMap::TileDist(hero->m_posMap.x, hero->m_posMap.y, pharmacistPos.x, pharmacistPos.y);
        if (npcDist > 5) {
            StartPathNearTarget(hero, map, pharmacistPos, 4);
            SetState(MiningState::ReturnToMine, "Moving to Twin City Pharmacist");
            return;
        }

        if (m_twinCityPharmacistId == 0) {
            SetState(MiningState::ReturnToMine, "Waiting for Twin City Pharmacist");
            return;
        }

        const DWORD now = GetTickCount();
        const int currentGateCount = GetTwinCityGateCount(hero);
        if (m_gateCountBeforeBuy >= 0) {
            if (currentGateCount > m_gateCountBeforeBuy) {
                m_gateCountBeforeBuy = -1;
            } else if (now - m_lastNpcActionTick <= 2500) {
                SetState(MiningState::ReturnToMine, "Waiting for TwinCityGate purchase");
                return;
            } else {
                m_gateCountBeforeBuy = -1;
                BeginTravelToMine(travel, settings);
                return;
            }
        }

        const int missingGateCount = settings.twinCityGateTargetCount - currentGateCount;
        if (missingGateCount <= 0) {
            m_gateCountBeforeBuy = -1;
            BeginTravelToMine(travel, settings);
            return;
        }

        if (m_storeItemId != 0) {
            if (!FindInventoryItemById(hero, m_storeItemId)) {
                m_storeItemId = 0;
                m_storeItemTypeId = 0;
            } else if (now - m_lastNpcActionTick <= 2500) {
                SetState(MiningState::ReturnToMine, "Waiting for sold item to leave inventory");
                return;
            } else {
                m_storeItemId = 0;
                m_storeItemTypeId = 0;
            }
        }

        if (HasSellItems(hero, settings)) {
            if (now - m_lastNpcActionTick < kNpcActionIntervalMs) {
                SetState(MiningState::ReturnToMine, "Preparing to sell at Twin City Pharmacist");
                return;
            }

            for (const auto& itemRef : hero->m_deqItem) {
                if (!itemRef || !ShouldSellItem(settings, *itemRef))
                    continue;

                m_storeItemId = itemRef->GetID();
                m_storeItemTypeId = itemRef->GetTypeID();
                hero->SellItem(m_twinCityPharmacistId, m_storeItemId);
                m_lastNpcActionTick = now;
                SetState(MiningState::ReturnToMine, "Selling item to Twin City Pharmacist");
                return;
            }
        }

        if (hero->GetSilver() < kTwinCityGateSilverCost) {
            m_gateCountBeforeBuy = -1;
            BeginTravelToMine(travel, settings);
            return;
        }

        if (now - m_lastNpcActionTick < kNpcActionIntervalMs) {
            SetState(MiningState::ReturnToMine, "Preparing to buy TwinCityGate");
            return;
        }

        if (now - m_lastNpcActionTick >= 1200) {
            m_gateCountBeforeBuy = currentGateCount;
            hero->BuyItem(m_twinCityPharmacistId, ItemTypeId::TWIN_CITY_GATE);
            m_lastNpcActionTick = now;
        }
        char status[128];
        snprintf(status, sizeof(status), "Buying TwinCityGate x%d", missingGateCount);
        SetState(MiningState::ReturnToMine, status);
        return;
    }

    if (travel->IsTraveling()) {
        SetState(m_state, "Traveling to mine");
        return;
    }

    if (m_lastMapId != settings.mineMapId) {
        BeginTravelToMine(travel, settings);
        return;
    }

    SetState(MiningState::MoveToSpot, "Moving to mine spot");
}

void MiningPlugin::HandleTravelToMarket(TravelPlugin* travel, CHero* hero, const MiningSettings& settings)
{
    if (!travel || !hero) {
        SetState(MiningState::Failed, "Travel plugin not available");
        return;
    }

    if (travel->GetState() == TravelState::Failed) {
        if (ShouldIgnoreReturnTownRun(hero, settings)) {
            ResetStoreSequence();
            ResetTradeSession();
            BeginTravelToMine(travel, settings);
            return;
        }
        char status[128];
        snprintf(status, sizeof(status), "Failed to reach %s", GetStoreTownName(hero, settings));
        SetState(MiningState::Failed, status);
        return;
    }

    if (m_waitingForReturnShortcut) {
        if (m_lastMapId == MAP_TWIN_CITY) {
            ResetReturnShortcut();

            if (GetStoreTargetMapId(hero, settings) == MAP_TWIN_CITY) {
                ResetStoreSequence();
                ResetTradeSession();
                SetState(MiningState::StoreItems, GetStoreEntryStatus(hero, settings));
                return;
            }

            travel->StartTravel(GetStoreTargetMapId(hero, settings), GetStoreTravelPos(hero, settings));
            char status[128];
            snprintf(status, sizeof(status), "Traveling to %s", GetStoreTownName(hero, settings));
            SetState(MiningState::TravelToMarket, status);
            return;
        }

        if (GetTickCount() - m_returnShortcutTick < kReturnShortcutTimeoutMs) {
            SetState(MiningState::TravelToMarket, "Waiting for town shortcut");
            return;
        }

        ResetReturnShortcut();
    }

    if (travel->IsTraveling()) {
        char status[128];
        snprintf(status, sizeof(status), "Traveling to %s", GetStoreTownName(hero, settings));
        SetState(MiningState::TravelToMarket, status);
        return;
    }

    if (m_lastMapId != GetStoreTargetMapId(hero, settings)) {
        BeginTravelToMarket(travel, hero, settings);
        return;
    }

    if (m_manualWarehouseMuleRunActive
        || ShouldHoldForReturnItem(hero, settings)
        || HasDepositItems(hero, settings)
        || HasSellItems(hero, settings)) {
        ResetStoreSequence();
        ResetTradeSession();
        if (m_manualWarehouseMuleRunActive)
            m_storePhase = StorePhase::MoveToWarehouse;
        SetState(MiningState::StoreItems, GetStoreEntryStatus(hero, settings));
    } else {
        BeginTravelToMine(travel, settings);
    }
}

void MiningPlugin::ResetStoreSequence()
{
    m_storePhase = StorePhase::MoveToSellNpc;
    m_muleFallbackToWarehouse = false;
    m_sellNpcId = 0;
    m_warehouseNpcId = 0;
    m_storeItemId = 0;
    m_storeItemTypeId = 0;
}

void MiningPlugin::CommitPendingTradeTransfers()
{
    for (const auto& [typeId, count] : m_pendingTradeTransferCounts) {
        if (count > 0)
            m_depositedItemCounts[typeId] += count;
    }
    m_pendingTradeTransferCounts.clear();
}

void MiningPlugin::BeginWarehouseFallback(const char* statusText)
{
    ResetTradeSession();
    m_muleFallbackToWarehouse = true;
    m_storePhase = StorePhase::MoveToWarehouse;
    m_warehouseNpcId = 0;
    SetState(MiningState::StoreItems, statusText);
}

void MiningPlugin::HandleStoreState(CHero* hero, CGameMap* map, TravelPlugin* travel, const MiningSettings& settings)
{
    if (!hero || !map) {
        SetState(MiningState::WaitingForGame, "Waiting for hero and map");
        return;
    }

    if (map->GetId() != GetStoreTargetMapId(hero, settings)) {
        BeginTravelToMarket(travel, hero, settings);
        return;
    }

    if (m_manualWarehouseMuleRunActive && !HasValidMuleName(settings)) {
        SetState(MiningState::Failed, "Configure a mule name first");
        return;
    }

    if (m_storageUseMuleTrade && !m_muleFallbackToWarehouse && !m_manualWarehouseMuleRunActive) {
        if (!HasValidMuleName(settings)) {
            SetState(MiningState::Failed, "Configure a mule name first");
            return;
        }

        if (m_storePhase == StorePhase::MoveToWarehouse
            || m_storePhase == StorePhase::OpenWarehouse
            || m_storePhase == StorePhase::WaitWarehouseOpen
            || m_storePhase == StorePhase::DepositWarehouse
            || m_storePhase == StorePhase::WaitWarehouseDeposit) {
            m_storePhase = StorePhase::MoveToMule;
        }
    }

    const DWORD now = GetTickCount();
    switch (m_storePhase) {
        case StorePhase::MoveToSellNpc: {
            if (!HasSellItems(hero, settings)) {
                m_storePhase = StorePhase::MoveToWarehouse;
                return;
            }

            const bool useTwinCityShopkeeper = m_sellUseTwinCityShopkeeper;
            const char* sellNpcName = useTwinCityShopkeeper ? "Shopkeeper" : "Pharmacist";
            const Position expectedSellNpcPos = useTwinCityShopkeeper ? kTwinCityShopkeeperPos : kMarketPharmacistPos;
            CRole* sellNpc = FindNpcByName(sellNpcName, expectedSellNpcPos, 16);
            if (sellNpc)
                m_sellNpcId = sellNpc->GetID();

            const Position sellNpcPos = sellNpc ? sellNpc->m_posMap : expectedSellNpcPos;
            const int npcDist = CGameMap::TileDist(hero->m_posMap.x, hero->m_posMap.y, sellNpcPos.x, sellNpcPos.y);
            if (npcDist > 5) {
                StartPathNearTarget(hero, map, sellNpcPos, 4);
                SetState(MiningState::StoreItems, useTwinCityShopkeeper ? "Moving to Shopkeeper" : "Moving to Pharmacist");
                return;
            }

            if (m_sellNpcId == 0) {
                SetState(MiningState::StoreItems, useTwinCityShopkeeper ? "Waiting for Shopkeeper" : "Waiting for Pharmacist");
                return;
            }

            m_storePhase = StorePhase::SellItem;
            return;
        }

        case StorePhase::OpenSellNpc:
            if (!HasSellItems(hero, settings)) {
                m_storePhase = StorePhase::MoveToWarehouse;
                return;
            }
            if (m_sellNpcId == 0) {
                m_storePhase = StorePhase::MoveToSellNpc;
                return;
            }
            m_storePhase = StorePhase::SellItem;
            return;

        case StorePhase::WaitSellNpcOpen:
            if (!HasSellItems(hero, settings)) {
                m_storePhase = StorePhase::MoveToWarehouse;
                return;
            }
            m_storePhase = StorePhase::SellItem;
            return;

        case StorePhase::SellItem: {
            if (!HasSellItems(hero, settings)) {
                m_storeItemId = 0;
                m_storeItemTypeId = 0;
                m_storePhase = StorePhase::MoveToWarehouse;
                return;
            }

            if (m_sellNpcId == 0) {
                m_storePhase = StorePhase::MoveToSellNpc;
                return;
            }

            if (now - m_lastNpcActionTick < kNpcActionIntervalMs)
                return;

            for (const auto& itemRef : hero->m_deqItem) {
                if (!itemRef || !ShouldSellItem(settings, *itemRef))
                    continue;

                m_storeItemId = itemRef->GetID();
                m_storeItemTypeId = itemRef->GetTypeID();
                hero->SellItem(m_sellNpcId, m_storeItemId);
                m_storePhase = StorePhase::WaitSellItem;
                m_lastNpcActionTick = now;
                SetState(MiningState::StoreItems, m_sellUseTwinCityShopkeeper ? "Selling item to Shopkeeper" : "Selling item to Pharmacist");
                return;
            }

            m_storePhase = StorePhase::MoveToWarehouse;
            return;
        }

        case StorePhase::WaitSellItem:
            if (!FindInventoryItemById(hero, m_storeItemId)) {
                m_storeItemId = 0;
                m_storeItemTypeId = 0;
                m_storePhase = StorePhase::SellItem;
                return;
            }
            if (now - m_lastNpcActionTick > 2500)
                m_storePhase = StorePhase::SellItem;
            return;

        case StorePhase::MoveToWarehouse: {
            if (!m_manualWarehouseMuleRunActive && !HasDepositItems(hero, settings)) {
                if (ShouldHoldForReturnItem(hero, settings)) {
                    SetState(MiningState::StoreItems, "Return item in bag, waiting in town");
                } else {
                    ResetStoreSequence();
                    BeginTravelToMine(travel, settings);
                }
                return;
            }

            const Position expectedWarehousePos = GetStorageWarehousePos(settings);
            CRole* warehouseman = FindNpcByName("Warehouseman", expectedWarehousePos, 16);
            if (warehouseman)
                m_warehouseNpcId = warehouseman->GetID();

            const Position warehousePos = warehouseman ? warehouseman->m_posMap : expectedWarehousePos;
            const int npcDist = CGameMap::TileDist(hero->m_posMap.x, hero->m_posMap.y, warehousePos.x, warehousePos.y);
            if (npcDist > 5) {
                StartPathNearTarget(hero, map, warehousePos, 4);
                SetState(MiningState::StoreItems, "Moving to Warehouseman");
                return;
            }

            if (m_warehouseNpcId == 0) {
                SetState(MiningState::StoreItems, "Waiting for Warehouseman");
                return;
            }

            m_storePhase = StorePhase::OpenWarehouse;
            return;
        }

        case StorePhase::OpenWarehouse:
            if (now - m_lastNpcActionTick < kNpcActionIntervalMs)
                return;
            hero->OpenWarehouse(m_warehouseNpcId);
            m_storePhase = StorePhase::WaitWarehouseOpen;
            m_lastNpcActionTick = now;
            SetState(MiningState::StoreItems, "Opening warehouse");
            return;

        case StorePhase::WaitWarehouseOpen:
            if ((hero->IsNpcActive() && hero->GetActiveNpc() == m_warehouseNpcId)
                || now - m_lastNpcActionTick > 1200) {
                m_storePhase = (m_manualWarehouseMuleRunActive && !m_muleFallbackToWarehouse)
                    ? StorePhase::WithdrawWarehouse
                    : StorePhase::DepositWarehouse;
            }
            return;

        case StorePhase::WithdrawWarehouse: {
            if (m_warehouseNpcId == 0) {
                m_storePhase = StorePhase::MoveToWarehouse;
                return;
            }

            if (hero->IsBagFull()) {
                m_storePhase = StorePhase::MoveToMule;
                return;
            }

            CItem* warehouseItem = FindWarehouseDepositItem(settings);
            if (!warehouseItem) {
                if (HasDepositItems(hero, settings)) {
                    m_storePhase = StorePhase::MoveToMule;
                    return;
                }

                ResetManualWarehouseMuleRun();
                ResetStoreSequence();
                BeginTravelToMine(travel, settings);
                return;
            }

            if (now - m_lastNpcActionTick < kNpcActionIntervalMs)
                return;

            m_storeItemId = warehouseItem->GetID();
            m_storeItemTypeId = warehouseItem->GetTypeID();
            hero->WithdrawWarehouseItem(m_warehouseNpcId, m_storeItemId);
            m_storePhase = StorePhase::WaitWarehouseWithdraw;
            m_lastNpcActionTick = now;
            SetState(MiningState::StoreItems, "Withdrawing warehouse item for mule");
            return;
        }

        case StorePhase::WaitWarehouseWithdraw: {
            CEntityInfo* entityInfo = Game::GetEntityInfo();
            const bool stillInWarehouse = entityInfo && entityInfo->FindWarehouseItemById(m_storeItemId) != nullptr;
            const bool nowInBag = FindInventoryItemById(hero, m_storeItemId) != nullptr;
            if (!stillInWarehouse || nowInBag) {
                m_storeItemId = 0;
                m_storeItemTypeId = 0;
                m_storePhase = hero->IsBagFull() ? StorePhase::MoveToMule : StorePhase::WithdrawWarehouse;
                return;
            }
            if (now - m_lastNpcActionTick > 2500)
                m_storePhase = StorePhase::WithdrawWarehouse;
            return;
        }

        case StorePhase::DepositWarehouse: {
            for (const auto& itemRef : hero->m_deqItem) {
                if (!itemRef || !ShouldStoreItem(settings, *itemRef))
                    continue;
                if (m_warehouseNpcId == 0)
                    return;
                if (now - m_lastNpcActionTick < kNpcActionIntervalMs)
                    return;

                m_storeItemId = itemRef->GetID();
                m_storeItemTypeId = itemRef->GetTypeID();
                hero->DepositWarehouseItem(m_warehouseNpcId, m_storeItemId);
                m_storePhase = StorePhase::WaitWarehouseDeposit;
                m_lastNpcActionTick = now;
                SetState(MiningState::StoreItems, "Depositing item");
                return;
            }

            if (m_manualWarehouseMuleRunActive) {
                ResetManualWarehouseMuleRun();
                ResetStoreSequence();
                BeginTravelToMine(travel, settings);
            } else if (ShouldHoldForReturnItem(hero, settings)) {
                SetState(MiningState::StoreItems, "Return item in bag, waiting in town");
            } else {
                ResetStoreSequence();
                BeginTravelToMine(travel, settings);
            }
            return;
        }

        case StorePhase::WaitWarehouseDeposit:
            if (!FindInventoryItemById(hero, m_storeItemId)) {
                if (m_storeItemTypeId != 0)
                    ++m_depositedItemCounts[m_storeItemTypeId];
                m_storeItemId = 0;
                m_storeItemTypeId = 0;
                m_storePhase = StorePhase::DepositWarehouse;
                return;
            }
            if (now - m_lastNpcActionTick > 2500)
                m_storePhase = StorePhase::DepositWarehouse;
            return;

        case StorePhase::MoveToMule: {
            if (!HasDepositItems(hero, settings)) {
                if (m_manualWarehouseMuleRunActive) {
                    if (FindWarehouseDepositItem(settings)) {
                        m_storePhase = StorePhase::MoveToWarehouse;
                        SetState(MiningState::StoreItems, "Checking warehouse for more mule items");
                    } else {
                        ResetManualWarehouseMuleRun();
                        ResetStoreSequence();
                        ResetTradeSession();
                        BeginTravelToMine(travel, settings);
                    }
                } else if (ShouldHoldForReturnItem(hero, settings)) {
                    SetState(MiningState::StoreItems, "Return item in bag, waiting in town");
                } else {
                    ResetStoreSequence();
                    ResetTradeSession();
                    BeginTravelToMine(travel, settings);
                }
                return;
            }

            CRole* warehouseman = FindNpcByName("Warehouseman", kWarehousePos, 16);
            const Position anchorPos = warehouseman ? warehouseman->m_posMap : kWarehousePos;
            CRole* mule = FindPlayerNearSpot(anchorPos, 8, settings.muleName);
            if (mule)
                m_tradePartnerId = mule->GetID();

            const int muleDist = CGameMap::TileDist(hero->m_posMap.x, hero->m_posMap.y,
                anchorPos.x, anchorPos.y);
            if (muleDist > 4) {
                StartPathNearTarget(hero, map, anchorPos, 3);
                SetState(MiningState::StoreItems, "Moving to Warehouseman area");
                return;
            }

            if (m_tradePartnerId == 0) {
                if (m_manualWarehouseMuleRunActive) {
                    if (m_manualMuleWaitStartTick == 0)
                        m_manualMuleWaitStartTick = now;
                    if (now - m_manualMuleWaitStartTick < kManualMuleWaitTimeoutMs) {
                        char status[128];
                        const DWORD remaining = (kManualMuleWaitTimeoutMs - (now - m_manualMuleWaitStartTick) + 999) / 1000;
                        snprintf(status, sizeof(status), "Waiting for mule near Warehouseman (%lu s)", (unsigned long)remaining);
                        SetState(MiningState::StoreItems, status);
                        return;
                    }

                    BeginWarehouseFallback("Mule not nearby after 60s, returning items to warehouse");
                    return;
                }

                char status[128];
                snprintf(status, sizeof(status), "Mule '%s' not nearby, using warehouse", settings.muleName);
                BeginWarehouseFallback(status);
                return;
            }

            m_manualMuleWaitStartTick = 0;
            m_storePhase = StorePhase::StartTrade;
            return;
        }

        case StorePhase::StartTrade:
            if (!HasDepositItems(hero, settings)) {
                if (m_manualWarehouseMuleRunActive) {
                    if (FindWarehouseDepositItem(settings)) {
                        m_storePhase = StorePhase::MoveToWarehouse;
                        SetState(MiningState::StoreItems, "Checking warehouse for more mule items");
                    } else {
                        ResetManualWarehouseMuleRun();
                        ResetStoreSequence();
                        ResetTradeSession();
                        BeginTravelToMine(travel, settings);
                    }
                } else if (ShouldHoldForReturnItem(hero, settings)) {
                    SetState(MiningState::StoreItems, "Return item in bag, waiting in town");
                } else {
                    ResetStoreSequence();
                    ResetTradeSession();
                    BeginTravelToMine(travel, settings);
                }
                return;
            }
            if (m_tradePartnerId == 0) {
                if (m_manualWarehouseMuleRunActive) {
                    m_storePhase = StorePhase::MoveToMule;
                    return;
                }
                BeginWarehouseFallback("Mule not nearby, using warehouse");
                return;
            }
            if (now - m_lastNpcActionTick < kTradeStartIntervalMs)
                return;

            hero->StartTrade(m_tradePartnerId);
            m_lastNpcActionTick = now;
            m_tradeSessionTick = now;
            m_storePhase = StorePhase::WaitTradeStart;
            SetState(MiningState::StoreItems, "Requesting trade with mule");
            return;

        case StorePhase::WaitTradeStart:
            if (!HasDepositItems(hero, settings)) {
                if (m_manualWarehouseMuleRunActive) {
                    if (FindWarehouseDepositItem(settings)) {
                        m_storePhase = StorePhase::MoveToWarehouse;
                        SetState(MiningState::StoreItems, "Checking warehouse for more mule items");
                    } else {
                        ResetManualWarehouseMuleRun();
                        ResetStoreSequence();
                        ResetTradeSession();
                        BeginTravelToMine(travel, settings);
                    }
                } else if (ShouldHoldForReturnItem(hero, settings)) {
                    SetState(MiningState::StoreItems, "Return item in bag, waiting in town");
                } else {
                    ResetStoreSequence();
                    ResetTradeSession();
                    BeginTravelToMine(travel, settings);
                }
                return;
            }
            if (now - m_lastNpcActionTick >= kTradeStartIntervalMs)
                m_storePhase = StorePhase::OfferTradeItem;
            SetState(MiningState::StoreItems, "Waiting for mule trade window");
            return;

        case StorePhase::OfferTradeItem: {
            if (!HasDepositItems(hero, settings)) {
                if (m_tradeOfferedCount > 0) {
                    m_storePhase = StorePhase::ConfirmTrade;
                } else if (m_manualWarehouseMuleRunActive) {
                    if (FindWarehouseDepositItem(settings)) {
                        m_storePhase = StorePhase::MoveToWarehouse;
                        SetState(MiningState::StoreItems, "Checking warehouse for more mule items");
                    } else {
                        ResetManualWarehouseMuleRun();
                        ResetStoreSequence();
                        ResetTradeSession();
                        BeginTravelToMine(travel, settings);
                    }
                } else if (ShouldHoldForReturnItem(hero, settings)) {
                    SetState(MiningState::StoreItems, "Return item in bag, waiting in town");
                } else {
                    ResetStoreSequence();
                    ResetTradeSession();
                    BeginTravelToMine(travel, settings);
                }
                return;
            }

            if (m_tradeOfferedCount >= 20) {
                m_storePhase = StorePhase::ConfirmTrade;
                return;
            }

            if (now - m_lastNpcActionTick < kTradeOfferIntervalMs)
                return;

            for (const auto& itemRef : hero->m_deqItem) {
                if (!itemRef || !ShouldStoreItem(settings, *itemRef))
                    continue;
                if (m_tradeOfferedItemIds.find(itemRef->GetID()) != m_tradeOfferedItemIds.end())
                    continue;

                m_storeItemId = itemRef->GetID();
                m_storeItemTypeId = itemRef->GetTypeID();
                hero->OfferTradeItem(m_storeItemId);
                m_tradeOfferedItemIds.insert(m_storeItemId);
                ++m_pendingTradeTransferCounts[m_storeItemTypeId];
                ++m_tradeOfferedCount;
                m_lastNpcActionTick = now;
                m_tradeSessionTick = now;
                m_storePhase = StorePhase::WaitTradeItem;
                SetState(MiningState::StoreItems, "Offering deposit item to mule");
                return;
            }

            m_storePhase = StorePhase::ConfirmTrade;
            return;
        }

        case StorePhase::WaitTradeItem:
            if (now - m_lastNpcActionTick >= kTradeOfferIntervalMs) {
                m_storeItemId = 0;
                m_storeItemTypeId = 0;
                m_storePhase = StorePhase::OfferTradeItem;
                return;
            }
            if (m_tradeSessionTick != 0 && now - m_tradeSessionTick > kTradeBatchTimeoutMs) {
                BeginWarehouseFallback("Mule trade timed out, using warehouse");
            }
            return;

        case StorePhase::ConfirmTrade:
            if (!HasDepositItems(hero, settings)) {
                CommitPendingTradeTransfers();
                if (m_manualWarehouseMuleRunActive) {
                    ResetTradeSession();
                    if (FindWarehouseDepositItem(settings)) {
                        m_storePhase = StorePhase::MoveToWarehouse;
                        SetState(MiningState::StoreItems, "Checking warehouse for more mule items");
                    } else {
                        ResetManualWarehouseMuleRun();
                        ResetStoreSequence();
                        BeginTravelToMine(travel, settings);
                    }
                } else if (ShouldHoldForReturnItem(hero, settings)) {
                    ResetTradeSession();
                    SetState(MiningState::StoreItems, "Return item in bag, waiting in town");
                } else {
                    ResetStoreSequence();
                    ResetTradeSession();
                    BeginTravelToMine(travel, settings);
                }
                return;
            }

            if (m_tradePartnerId == 0) {
                if (m_manualWarehouseMuleRunActive) {
                    m_storePhase = StorePhase::MoveToMule;
                    return;
                }
                BeginWarehouseFallback("Lost mule, using warehouse");
                return;
            }

            if (now - m_lastNpcActionTick >= kTradeConfirmIntervalMs) {
                hero->AcceptTrade(m_tradePartnerId);
                m_lastNpcActionTick = now;
                SetState(MiningState::StoreItems, "Accepting trade with mule");
            }

            if (m_tradeSessionTick != 0 && now - m_tradeSessionTick > kTradeBatchTimeoutMs) {
                BeginWarehouseFallback("Mule trade timed out, using warehouse");
            }
            return;
    }
}

void MiningPlugin::RenderItemSelector(MiningSettings& settings)
{
    ImGui::InputText("Item Search", m_itemSearch, IM_ARRAYSIZE(m_itemSearch));
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear Search##miningitems"))
        m_itemSearch[0] = '\0';
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear Return Items"))
        settings.returnItemIds.clear();
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear Deposit Items"))
        settings.depositItemIds.clear();
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear Sell Items"))
        settings.sellItemIds.clear();
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear Drop Items"))
        settings.dropItemIds.clear();

    ImGui::Text("Return: %d  Deposit: %d  Sell: %d  Drop: %d",
        (int)settings.returnItemIds.size(),
        (int)settings.depositItemIds.size(),
        (int)settings.sellItemIds.size(),
        (int)settings.dropItemIds.size());

    const std::string searchText = ToLowerCopy(m_itemSearch);
    int shown = 0;
    bool limitedResults = false;
    ImGui::BeginChild("##miningitembrowser", ImVec2(0, 260.0f), ImGuiChildFlags_Borders);
    if (ImGui::BeginTable("##miningitemtable", 6,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
            ImVec2(0, 0))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Item", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Return", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Deposit", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Sell", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Drop", ImGuiTableColumnFlags_WidthFixed, 80.0f);
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
                const bool selected = IsSelectedReturnItem(settings, info->id);
                char buttonId[64];
                snprintf(buttonId, sizeof(buttonId), "%s##miningreturn%u", selected ? "Remove" : "Add", info->id);
                if (ImGui::SmallButton(buttonId)) {
                    if (selected)
                        RemoveItemId(settings.returnItemIds, info->id);
                    else
                        AddItemId(settings.returnItemIds, info->id);
                }
            }

            ImGui::TableNextColumn();
            {
                const bool selected = IsSelectedDepositItem(settings, info->id);
                char buttonId[64];
                snprintf(buttonId, sizeof(buttonId), "%s##miningdeposit%u", selected ? "Remove" : "Add", info->id);
                if (ImGui::SmallButton(buttonId)) {
                    if (selected)
                        RemoveItemId(settings.depositItemIds, info->id);
                    else
                        AddItemId(settings.depositItemIds, info->id);
                }
            }

            ImGui::TableNextColumn();
            {
                const bool selected = IsSelectedSellItem(settings, info->id);
                char buttonId[64];
                snprintf(buttonId, sizeof(buttonId), "%s##miningsell%u", selected ? "Remove" : "Add", info->id);
                if (ImGui::SmallButton(buttonId)) {
                    if (selected)
                        RemoveItemId(settings.sellItemIds, info->id);
                    else
                        AddItemId(settings.sellItemIds, info->id);
                }
            }

            ImGui::TableNextColumn();
            {
                const bool selected = IsSelectedDropItem(settings, info->id);
                char buttonId[64];
                snprintf(buttonId, sizeof(buttonId), "%s##miningdrop%u", selected ? "Remove" : "Add", info->id);
                if (ImGui::SmallButton(buttonId)) {
                    if (selected)
                        RemoveItemId(settings.dropItemIds, info->id);
                    else
                        AddItemId(settings.dropItemIds, info->id);
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

    if (shown == 0)
        ImGui::TextDisabled("No item types matched the current filter.");
    else if (limitedResults)
        ImGui::TextDisabled("Showing the first 250 items. Use search to narrow the list.");

    ImGui::EndChild();
}

void MiningPlugin::Update()
{
    MiningSettings& settings = GetMiningSettings();
    TravelPlugin* travel = PluginManager::Get().GetPlugin<TravelPlugin>();

    CHero* hero = Game::GetHero();
    CGameMap* map = Game::GetMap();
    RefreshRuntimeState(hero, map);

    if (!settings.enabled) {
        if (m_state != MiningState::Idle)
            StopAutomation(true);
        return;
    }

    if (!hero || !map) {
        SetState(MiningState::WaitingForGame, "Waiting for hero and map");
        return;
    }

    SyncInventoryTracker(hero, settings);

    if (!HasValidMineSpot(settings)) {
        SetState(MiningState::Failed, "Configure a mine coordinate first");
        return;
    }

    if (settings.tradeReturnItemsToMule
        && HasDepositItems(hero, settings)
        && !HasValidMuleName(settings)) {
        SetState(MiningState::Failed, "Configure a mule name first");
        return;
    }

    if (HandleDeath(hero, travel, settings))
        return;

    if (m_state == MiningState::TravelToMarket) {
        HandleTravelToMarket(travel, hero, settings);
        return;
    }

    if (m_state == MiningState::StoreItems) {
        HandleStoreState(hero, map, travel, settings);
        return;
    }

    if (m_state == MiningState::TravelToMine || m_state == MiningState::ReturnToMine) {
        HandleTravelToMine(travel, settings);
        return;
    }

    if (ShouldHoldForReturnItem(hero, settings) || ShouldStartTownRunByBagThreshold(hero, settings)) {
        BeginTravelToMarket(travel, hero, settings);
        return;
    }

    if (map->GetId() != settings.mineMapId) {
        BeginTravelToMine(travel, settings);
        return;
    }

    if (travel && travel->IsTraveling()) {
        SetState(MiningState::WaitingForGame, "Waiting for travel plugin");
        return;
    }

    if (TryDropFilteredItem(hero, map, settings))
        return;

    Position mineStandPos = settings.minePos;
    if (!FindMineStandTile(hero, map, settings.minePos, mineStandPos)) {
        ResetMiningSession();
        SetState(MiningState::MoveToSpot, "Waiting for empty tile near mine spot");
        return;
    }

    const int distToMine = CGameMap::TileDist(hero->m_posMap.x, hero->m_posMap.y, mineStandPos.x, mineStandPos.y);
    if (distToMine > 0) {
        ResetMiningSession();
        if (MoveToMineSpot(hero, map, mineStandPos)) {
            SetState(MiningState::MoveToSpot,
                (mineStandPos.x == settings.minePos.x && mineStandPos.y == settings.minePos.y)
                    ? "Walking to mine spot"
                    : "Walking to empty tile near mine spot");
        } else {
            SetState(MiningState::Failed, "Failed to move to mine spot");
        }
        return;
    }

    if (Pathfinder::Get().IsActive() || hero->IsJumping()) {
        ResetMiningSession();
        SetState(MiningState::MoveToSpot, "Moving to mine spot");
        return;
    }

    if (IsMovementCommandStillAdvancing(hero)) {
        ResetMiningSession();
        SetState(MiningState::MoveToSpot, "Settling on mine spot");
        return;
    }

    const DWORD now = GetTickCount();
    if (HasMiningSignal(hero)) {
        m_miningSessionActive = true;
        SetState(MiningState::Mining, "Mining");
        return;
    }

    if (now - m_lastMineTick >= kMineActionIntervalMs) {
        hero->StartMining();
        m_lastMineTick = now;
    }

    m_miningSessionActive = false;
    SetState(MiningState::StartMining, "Starting mining");
}

void MiningPlugin::RenderUI()
{
    MiningSettings& settings = GetMiningSettings();
    CHero* hero = Game::GetHero();
    TravelPlugin* travel = PluginManager::Get().GetPlugin<TravelPlugin>();
    constexpr ImGuiTreeNodeFlags kSectionFlags = ImGuiTreeNodeFlags_DefaultOpen;

    if (ImGui::CollapsingHeader("General", kSectionFlags)) {
        ImGui::Checkbox("Enabled", &settings.enabled);
        ImGui::Checkbox("Auto Revive In Town", &settings.autoReviveInTown);
    }

    if (ImGui::CollapsingHeader("Mine Spot", kSectionFlags)) {
        ImGui::Text("Mine Map: %u", settings.mineMapId);
        ImGui::InputInt2("Mine Position", &settings.minePos.x);
        if (hero && ImGui::Button("Use Hero Position")) {
            settings.mineMapId = m_lastMapId;
            settings.minePos = hero->m_posMap;
        }
        ImGui::SameLine();
        if (ImGui::Button(m_captureMinePos ? "Stop Map Capture" : "Capture From Map"))
            m_captureMinePos = !m_captureMinePos;
        ImGui::TextDisabled("%s", m_captureMinePos ? "Click the map to set the mine coordinate." : "Mine at a single configured coordinate.");
    }

    if (ImGui::CollapsingHeader("Return Travel", kSectionFlags)) {
        ImGui::Checkbox("Deposit In Twin City Warehouse", &settings.useTwinCityWarehouse);
        ImGui::Checkbox("Trade Deposit Items To Mule", &settings.tradeReturnItemsToMule);
        ImGui::Checkbox("Use TwinCityGate To Return", &settings.useTwinCityGate);
        ImGui::Checkbox("Buy TwinCityGates In Twin City", &settings.buyTwinCityGates);
        ImGui::InputInt("TwinCityGate Target Count", &settings.twinCityGateTargetCount);
        if (settings.twinCityGateTargetCount < 1)
            settings.twinCityGateTargetCount = 1;
        ImGui::TextDisabled("Return-to-town order: TwinCityGate, then Travel may use VIP teleport automatically, then normal travel.");
        ImGui::TextDisabled("TwinCityGate restock still happens before leaving town when enabled.");
        ImGui::TextDisabled("TwinCityGate restock sells marked items at the Twin City Pharmacist first and only buys gates when you have at least 200 silver.");
        ImGui::InputText("Mule Name", settings.muleName, IM_ARRAYSIZE(settings.muleName));
        const OBJID stopWarehouseMapId = GetStorageMapId(settings, false);
        const Position stopWarehousePos = GetStorageWarehousePos(settings);
        const char* stopWarehouseTown = GetStorageTownName(settings, false);
        if (ImGui::Button("Stop And Go To Warehouse")) {
            settings.enabled = false;
            StopAutomation(true);

            if (!hero) {
                SetState(MiningState::Idle, "Disabled, waiting for hero");
            } else if (!travel) {
                SetState(MiningState::Idle, "Disabled, travel plugin not available");
            } else {
                travel->StartTravel(stopWarehouseMapId, stopWarehousePos);
                char status[128];
                snprintf(status, sizeof(status), "Disabled, traveling to %s warehouse", stopWarehouseTown);
                SetState(MiningState::Idle, status);
            }
        }
        ImGui::TextDisabled("Stops mining immediately and sends the hero to the selected warehouse.");
        ImGui::TextDisabled("This ignores mule routing and does not keep the mining loop running in the background.");
        if (settings.tradeReturnItemsToMule) {
            ImGui::TextDisabled("Normal deposit runs still search for the exact configured mule name.");
            ImGui::TextDisabled("Normal deposit runs trade deposit items to the mule instead of the warehouse.");
            ImGui::TextDisabled("Trade-to-mule always returns to Market and ignores Twin City warehouse routing.");
        }
        if (hero) {
            ImGui::Text("VIP Active: %s", hero->IsVip() ? "Yes" : "No");
            if (IsVipTeleportOnCooldown()) {
                const DWORD elapsed = GetTickCount() - GetLastVipTeleportTick();
                const DWORD remaining = elapsed < 60000 ? (60000 - elapsed + 999) / 1000 : 0;
                ImGui::Text("VIP Cooldown: %lu s", (unsigned long)remaining);
            } else {
                ImGui::Text("VIP Cooldown: Ready");
            }
            ImGui::Text("TwinCityGate Count: %d / %d", GetTwinCityGateCount(hero), settings.twinCityGateTargetCount);
        }
    }

    if (ImGui::CollapsingHeader("Item Rules", kSectionFlags)) {
        ImGui::InputInt("Drop Threshold", &settings.dropItemThreshold);
        settings.dropItemThreshold = std::clamp(settings.dropItemThreshold, 1, CHero::MAX_BAG_ITEMS);
        ImGui::SliderInt("Town Return Threshold", &settings.townBagThreshold, 0, CHero::MAX_BAG_ITEMS);
        ImGui::TextDisabled("Return items trigger the trip back to town.");
        ImGui::TextDisabled("Town Return Threshold starts the town loop when bag slots reach the threshold and there are return, deposit, or sell items to process.");
        ImGui::TextDisabled("Set Town Return Threshold to 0 to disable it.");
        ImGui::TextDisabled("Deposit items are stored in the warehouse, or traded to the mule when enabled.");
        ImGui::TextDisabled("Sell items are sold before deposit logic when the miner is already processing a town run.");
        ImGui::TextDisabled("If Market travel would need 100 silver you do not have, the miner falls back to the Twin City Shopkeeper first.");
        ImGui::TextDisabled("If you have under 100 silver and nothing to sell, return items are ignored and mining resumes.");
        ImGui::TextDisabled("Items selected as return, deposit, or sell are never dropped by the cleanup filter.");
        RenderItemSelector(settings);
    }

    if (ImGui::CollapsingHeader("Timing", kSectionFlags))
        ImGui::SliderInt("Movement Interval (ms)", &settings.movementIntervalMs, 100, 5000);

    if (ImGui::CollapsingHeader("Session Tracker", kSectionFlags)) {
        if (ImGui::Button("Reset Mining Tracker"))
            ResetItemTracking();
        ImGui::SameLine();
        ImGui::TextDisabled("Tracks newly acquired items and successful warehouse deposits for the current character session.");

        std::vector<uint32_t> trackedTypeIds;
        trackedTypeIds.reserve(m_obtainedItemCounts.size() + m_depositedItemCounts.size());
        for (const auto& [typeId, count] : m_obtainedItemCounts) {
            if (count > 0)
                trackedTypeIds.push_back(typeId);
        }
        for (const auto& [typeId, count] : m_depositedItemCounts) {
            if (count > 0 && std::find(trackedTypeIds.begin(), trackedTypeIds.end(), typeId) == trackedTypeIds.end())
                trackedTypeIds.push_back(typeId);
        }

        std::sort(trackedTypeIds.begin(), trackedTypeIds.end(), [](uint32_t lhs, uint32_t rhs) {
            const ItemTypeInfo* lhsInfo = GetItemTypeInfo(lhs);
            const ItemTypeInfo* rhsInfo = GetItemTypeInfo(rhs);
            const std::string lhsName = lhsInfo ? lhsInfo->name : "";
            const std::string rhsName = rhsInfo ? rhsInfo->name : "";
            if (lhsName != rhsName)
                return lhsName < rhsName;
            return lhs < rhs;
        });

        if (trackedTypeIds.empty()) {
            ImGui::TextDisabled("No tracked item activity yet.");
        } else if (ImGui::BeginTable("##miningtracker", 4,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                ImVec2(0, 180.0f))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Item", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Obtained", ImGuiTableColumnFlags_WidthFixed, 75.0f);
            ImGui::TableSetupColumn("Deposited", ImGuiTableColumnFlags_WidthFixed, 75.0f);
            ImGui::TableSetupColumn("In Bag", ImGuiTableColumnFlags_WidthFixed, 60.0f);
            ImGui::TableHeadersRow();

            for (uint32_t typeId : trackedTypeIds) {
                const ItemTypeInfo* info = GetItemTypeInfo(typeId);
                const auto obtainedIt = m_obtainedItemCounts.find(typeId);
                const auto depositedIt = m_depositedItemCounts.find(typeId);
                const int obtained = obtainedIt != m_obtainedItemCounts.end() ? obtainedIt->second : 0;
                const int deposited = depositedIt != m_depositedItemCounts.end() ? depositedIt->second : 0;
                const int inBag = CountInventoryItemsByType(hero, typeId);

                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%s (%u)", info ? info->name.c_str() : "Unknown", typeId);
                ImGui::TableNextColumn();
                ImGui::Text("%d", obtained);
                ImGui::TableNextColumn();
                ImGui::Text("%d", deposited);
                ImGui::TableNextColumn();
                ImGui::Text("%d", inBag);
            }

            ImGui::EndTable();
        }
    }

    if (ImGui::CollapsingHeader("Runtime", kSectionFlags)) {
        ImGui::Text("State: %s", GetStateName());
        ImGui::Text("Status: %s", m_statusText);
        ImGui::Text("Map: %u", m_lastMapId);
        ImGui::Text("Hero Pos: (%d, %d)", m_lastHeroPos.x, m_lastHeroPos.y);
        ImGui::Text("Bag Items: %d / %d", (int)m_lastBagCount, CHero::MAX_BAG_ITEMS);
        ImGui::Text("Town Return Threshold: %d", settings.townBagThreshold);
        ImGui::Text("Mining Active: %s", hero && HasMiningSignal(hero) ? "Yes" : "No");
        if (hero) {
            int returnItemsInBag = 0;
            int depositItemsInBag = 0;
            int sellItemsInBag = 0;
            int dropItemsInBag = 0;
            for (const auto& itemRef : hero->m_deqItem) {
                if (!itemRef)
                    continue;

                const uint32_t typeId = itemRef->GetTypeID();
                if (IsSelectedReturnItem(settings, typeId))
                    ++returnItemsInBag;
                if (IsSelectedDepositItem(settings, typeId))
                    ++depositItemsInBag;
                if (IsSelectedSellItem(settings, typeId))
                    ++sellItemsInBag;
                if (IsSelectedDropItem(settings, typeId))
                    ++dropItemsInBag;
            }

            const int gateCount = GetTwinCityGateCount(hero);
            const bool gateRestockNeeded =
                settings.useTwinCityGate
                && settings.buyTwinCityGates
                && gateCount < settings.twinCityGateTargetCount;

            ImGui::Text("Silver: %u", hero->GetSilver());
            ImGui::Text("Town Flags In Bag: return=%d deposit=%d sell=%d drop=%d",
                returnItemsInBag, depositItemsInBag, sellItemsInBag, dropItemsInBag);
            ImGui::Text("Mining Enabled: %s", settings.enabled ? "Yes" : "No");
            ImGui::Text("Gate Restock: need=%s affordable=%s gates=%d / %d pending=%s",
                gateRestockNeeded ? "Yes" : "No",
                hero->GetSilver() >= kTwinCityGateSilverCost ? "Yes" : "No",
                gateCount,
                settings.twinCityGateTargetCount,
                m_gateCountBeforeBuy >= 0 ? "Yes" : "No");

            const CCommand& cmd = hero->GetCommand();
            ImGui::Text("Command: type=%d status=%d", cmd.iType, cmd.iStatus);
        }
    }
}

bool MiningPlugin::OnMapClick(const Position& tile)
{
    CGameMap* map = Game::GetMap();
    if (!map)
        return false;

    MiningSettings& settings = GetMiningSettings();
    if (m_captureMinePos) {
        settings.mineMapId = map->GetId();
        settings.minePos = tile;
        m_captureMinePos = false;
        snprintf(m_statusText, sizeof(m_statusText), "Mine spot set to (%d,%d)", tile.x, tile.y);
        return true;
    }

    return false;
}
