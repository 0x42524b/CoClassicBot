#include "hunt_town.h"
#include "inventory_utils.h"
#include "npc_utils.h"
#include "CHero.h"
#include "CGameMap.h"
#include "CItem.h"
#include "itemtype.h"
#include "gateway.h"
#include "log.h"
#include <algorithm>

// ── File-local constants and helpers ─────────────────────────────────────────
namespace {

const Position kTreasureBankPos = {180, 183};
const Position kComposeBankPos  = {179, 187};
const Position kWarehousePos    = {182, 180};
const Position kPharmacistPos   = {198, 181};

struct BlacksmithEntry {
    OBJID       mapId;
    const char* npcName;
    Position    pos;
};

const BlacksmithEntry kBlacksmiths[] = {
    { MAP_TWIN_CITY,      "Blacksmith", {452, 330} },
    { MAP_DESERT_CITY,    "Blacksmith", {486, 621} },
    { MAP_APE_MOUNTAIN,   "Blacksmith", {560, 508} },
    { MAP_BIRD_ISLAND,    "Blacksmith", {751, 544} },
    { MAP_PHOENIX_CASTLE, "Blacksmith", {197, 226} },
};

constexpr uint32_t kArrowCost = 34000;

constexpr int kMinNpcActionIntervalMs = 100;
constexpr int kMaxNpcActionIntervalMs = 2000;

const BlacksmithEntry* FindBlacksmithForMap(OBJID mapId)
{
    for (const auto& entry : kBlacksmiths) {
        if (entry.mapId == mapId && entry.pos.x != 0 && entry.pos.y != 0)
            return &entry;
    }
    return nullptr;
}

bool IsArcherModeEnabled(const AutoHuntSettings& settings)
{
    return settings.archerMode || settings.combatMode == AutoHuntCombatMode::Archer;
}

DWORD GetNpcActionIntervalMs(const AutoHuntSettings& settings)
{
    return static_cast<DWORD>(std::clamp(settings.npcActionIntervalMs,
        kMinNpcActionIntervalMs, kMaxNpcActionIntervalMs));
}

} // namespace

// ── HuntTownService — Map query helpers ──────────────────────────────────────

bool HuntTownService::HasBlacksmithOnMap(OBJID mapId)
{
    return FindBlacksmithForMap(mapId) != nullptr;
}

// ── HuntTownService — Reset helpers ──────────────────────────────────────────

void HuntTownService::ResetRepairSequence()
{
    m_repairPhase  = RepairPhase::MoveToNpc;
    m_repairNpcId  = 0;
    m_repairItemId = 0;
    m_repairSlot   = 0;
}

void HuntTownService::ResetBuyArrowsSequence()
{
    m_buyArrowsPhase   = BuyArrowsPhase::MoveToBlacksmith;
    m_blacksmithNpcId  = 0;
    m_arrowsBoughtCount = 0;
}

void HuntTownService::ResetStoreSequence()
{
    m_storePhase           = StorePhase::PackMeteors;
    m_treasureBankNpcId    = 0;
    m_composeBankNpcId     = 0;
    m_warehouseNpcId       = 0;
    m_storeItemId          = 0;
    m_storeMeteorCountBefore = 0;
}

// ── HuntTownService — Arrow helpers ──────────────────────────────────────────

int HuntTownService::CountUsableArrowPacks(const CHero* hero) const
{
    if (!hero) return 0;
    int count = 0;
    CItem* equipped = hero->GetEquip(EquipSlot::LWEAPON);
    if (equipped && equipped->IsArrow() && equipped->GetDurability() > 3)
        ++count;
    for (const auto& itemRef : hero->m_deqItem) {
        if (itemRef && itemRef->IsArrow() && itemRef->GetDurability() > 3)
            ++count;
    }
    return count;
}

bool HuntTownService::NeedsArrows(const CHero* hero, const AutoHuntSettings& settings) const
{
    if (!settings.buyArrows || !IsArcherModeEnabled(settings) || settings.arrowTypeId == 0)
        return false;
    return CountUsableArrowPacks(hero) < settings.arrowBuyCount;
}

// ── HuntTownService — Decision helpers ───────────────────────────────────────

bool HuntTownService::NeedsRepair(const CHero* hero, const AutoHuntSettings& settings) const
{
    if (!settings.autoRepair || !hero)
        return false;

    for (int slot = 0; slot < EquipSlot::COUNT; ++slot) {
        const CItem* item = hero->GetEquip(slot);
        if (!item || item->GetMaxDurabilityRaw() <= 0)
            continue;
        if (item->IsArrow())
            continue;

        if (item->GetDurabilityRaw() < item->GetMaxDurabilityRaw()
            && GetDurabilityPercent(*item) <= settings.repairPercent) {
            return true;
        }
    }

    return false;
}

bool HuntTownService::NeedsStorage(const CHero* hero, const AutoHuntSettings& settings) const
{
    if (!settings.autoStore || !hero)
        return false;

    if (settings.storeTreasureBank && HasTreasureBankItems(hero))
        return true;
    if (settings.storeComposeBank && HasComposeBankItems(hero))
        return true;
    if (HasWarehouseItems(hero, settings))
        return true;
    if (settings.autoDepositSilver) {
        const uint32_t keepAmount = settings.silverKeepAmount > 0 ? (uint32_t)settings.silverKeepAmount : 0;
        if (hero->GetSilver() > keepAmount)
            return true;
    }
    return false;
}

bool HuntTownService::NeedTownRun(CHero* hero, const AutoHuntSettings& settings, bool needsArrows) const
{
    if (!hero)
        return false;

    if (NeedsRepair(hero, settings))
        return true;

    if (needsArrows)
        return true;

    if (settings.autoStore && settings.immediateReturnOnPriorityItems
        && HasPriorityReturnItems(hero, settings)) {
        return true;
    }

    if (NeedsStorage(hero, settings)) {
        const int bagThreshold = std::clamp(settings.bagStoreThreshold, 1, CHero::MAX_BAG_ITEMS);
        if ((int)hero->m_deqItem.size() >= bagThreshold || hero->IsBagFull())
            return true;
    }

    return false;
}

// ── HuntTownService — Item predicates ────────────────────────────────────────

bool HuntTownService::IsSelectedLootItem(const AutoHuntSettings& settings, uint32_t typeId)
{
    return std::find(settings.lootItemIds.begin(), settings.lootItemIds.end(), typeId)
        != settings.lootItemIds.end();
}

bool HuntTownService::IsSelectedWarehouseItem(const AutoHuntSettings& settings, uint32_t typeId)
{
    return std::find(settings.warehouseItemIds.begin(), settings.warehouseItemIds.end(), typeId)
        != settings.warehouseItemIds.end();
}

bool HuntTownService::IsSelectedPriorityReturnItem(const AutoHuntSettings& settings, uint32_t typeId)
{
    return std::find(settings.priorityReturnItemIds.begin(), settings.priorityReturnItemIds.end(), typeId)
        != settings.priorityReturnItemIds.end();
}

bool HuntTownService::ShouldLootMapItem(const AutoHuntSettings& settings, const CMapItem& item)
{
    if (IsSelectedLootItem(settings, item.m_idType))
        return true;
    if (MatchesSelectedLootQuality(settings, item))
        return true;
    return settings.minimumLootPlus > 0 && item.GetPlus() >= settings.minimumLootPlus;
}

bool HuntTownService::IsTreasureBankDragonBallFamily(const CItem& item) const
{
    return item.IsDragonBall() || item.IsDBScroll();
}

bool HuntTownService::IsTreasureBankMeteorFamily(const CItem& item) const
{
    return item.IsMeteor() || item.IsMeteorScroll();
}

bool HuntTownService::IsTreasureBankItem(const CItem& item) const
{
    return IsTreasureBankDragonBallFamily(item) || IsTreasureBankMeteorFamily(item);
}

bool HuntTownService::IsComposeBankItem(const CItem& item) const
{
    return item.IsWearableEquipment() && (item.GetPlus() == 1 || item.GetPlus() == 2);
}

bool HuntTownService::HasTreasureBankItems(const CHero* hero) const
{
    return InventoryHasMatchingItem(hero, [this](const CItem& item) {
        return IsTreasureBankItem(item);
    });
}

bool HuntTownService::HasTreasureBankDragonBallItems(const CHero* hero) const
{
    return InventoryHasMatchingItem(hero, [this](const CItem& item) {
        return IsTreasureBankDragonBallFamily(item);
    });
}

bool HuntTownService::HasTreasureBankMeteorItems(const CHero* hero) const
{
    return InventoryHasMatchingItem(hero, [this](const CItem& item) {
        return IsTreasureBankMeteorFamily(item);
    });
}

bool HuntTownService::HasComposeBankItems(const CHero* hero) const
{
    return InventoryHasMatchingItem(hero, [this](const CItem& item) {
        return IsComposeBankItem(item);
    });
}

bool HuntTownService::HasPriorityReturnItems(const CHero* hero, const AutoHuntSettings& settings) const
{
    return InventoryHasMatchingItem(hero, [&settings, this](const CItem& item) {
        return IsSelectedPriorityReturnItem(settings, item.GetTypeID());
    });
}

bool HuntTownService::HasWarehouseItems(const CHero* hero, const AutoHuntSettings& settings) const
{
    return InventoryHasMatchingItem(hero, [this, &settings](const CItem& item) {
        return ShouldStoreWarehouseItem(settings, item);
    });
}

bool HuntTownService::ShouldStoreWarehouseItem(const AutoHuntSettings& settings, const CItem& item) const
{
    if (const ItemTypeInfo* info = GetItemTypeInfo(item.GetTypeID())) {
        if (IsConsumablePotionType(*info, false) || IsConsumablePotionType(*info, true))
            return false;
    }

    if (settings.storeTreasureBank && IsTreasureBankItem(item))
        return false;

    if (settings.storeComposeBank && IsComposeBankItem(item))
        return false;

    if (IsSelectedPriorityReturnItem(settings, item.GetTypeID()))
        return true;

    if (IsSelectedWarehouseItem(settings, item.GetTypeID()))
        return true;

    if (IsEquipmentQualitySort(item.GetSort())) {
        const int quality = item.GetQuality();
        if ((quality == ItemQuality::REFINED && settings.storeRefined)
            || (quality == ItemQuality::UNIQUE && settings.storeUnique)
            || (quality == ItemQuality::ELITE && settings.storeElite)
            || (quality == ItemQuality::SUPER && settings.storeSuper))
            return true;
    }

    return settings.minimumStorePlus > 0 && item.GetPlus() >= settings.minimumStorePlus;
}

// ── HuntTownService — HandleRepairState ──────────────────────────────────────

void HuntTownService::HandleRepairState(CHero* hero, CGameMap* map,
    const AutoHuntSettings& settings, const HuntTownCallbacks& cb)
{
    if (!hero || !map) {
        cb.setStateFn(AutoHuntState::WaitingForGame, "Waiting for hero and map");
        return;
    }

    if (map->GetId() != MAP_MARKET) {
        cb.beginTravelToMarketFn();
        return;
    }

    CRole* pharmacist = FindNpcByName("Pharmacist", kPharmacistPos, 16);
    const Position pharmacistPos = pharmacist ? pharmacist->m_posMap : kPharmacistPos;
    const int npcDist = CGameMap::TileDist(hero->m_posMap.x, hero->m_posMap.y, pharmacistPos.x, pharmacistPos.y);

    if (npcDist > 5) {
        cb.startPathNearTargetFn(hero, map, pharmacistPos, 4);
        cb.setStateFn(AutoHuntState::Repair, "Moving to Pharmacist");
        return;
    }

    if (pharmacist)
        m_repairNpcId = pharmacist->GetID();

    const DWORD now = GetTickCount();
    const DWORD npcActionInterval = GetNpcActionIntervalMs(settings);

    switch (m_repairPhase) {
        case RepairPhase::MoveToNpc:
            m_repairPhase = RepairPhase::Unequip;
            return;

        case RepairPhase::Unequip: {
            for (int slot = 0; slot < EquipSlot::COUNT; ++slot) {
                CItem* item = hero->GetEquip(slot);
                if (!item || item->GetMaxDurabilityRaw() <= 0)
                    continue;
                if (item->IsArrow())
                    continue;
                if (item->GetDurabilityRaw() >= item->GetMaxDurabilityRaw())
                    continue;

                if (now - m_lastNpcActionTick < npcActionInterval)
                    return;

                m_repairSlot = slot;
                m_repairItemId = item->GetID();
                hero->UnequipItem(m_repairItemId, m_repairSlot);
                m_repairPhase = RepairPhase::WaitUnequip;
                m_lastNpcActionTick = now;
                cb.setStateFn(AutoHuntState::Repair, "Unequipping item for repair");
                return;
            }

            ResetRepairSequence();
            if (settings.autoStore && NeedsStorage(hero, settings)) {
                ResetStoreSequence();
                cb.setStateFn(AutoHuntState::StoreItems, "Processing storage rules");
            } else {
                cb.beginTravelToZoneFn();
            }
            return;
        }

        case RepairPhase::WaitUnequip:
            if (!hero->GetEquip(m_repairSlot) && FindInventoryItemById(hero, m_repairItemId)) {
                m_repairPhase = RepairPhase::Repair;
                return;
            }
            if (now - m_lastNpcActionTick > 2500)
                m_repairPhase = RepairPhase::Unequip;
            return;

        case RepairPhase::Repair:
            if (now - m_lastNpcActionTick < npcActionInterval)
                return;
            hero->RepairItem(m_repairItemId);
            m_repairPhase = RepairPhase::WaitRepair;
            m_lastNpcActionTick = now;
            cb.setStateFn(AutoHuntState::Repair, "Repairing item");
            return;

        case RepairPhase::WaitRepair: {
            CItem* bagItem = FindInventoryItemById(hero, m_repairItemId);
            if (bagItem && bagItem->GetDurabilityRaw() >= bagItem->GetMaxDurabilityRaw()) {
                m_repairPhase = RepairPhase::Reequip;
                return;
            }
            if (now - m_lastNpcActionTick > 2500)
                m_repairPhase = RepairPhase::Repair;
            return;
        }

        case RepairPhase::Reequip:
            if (now - m_lastNpcActionTick < npcActionInterval)
                return;
            hero->EquipItem(m_repairItemId, m_repairSlot);
            m_repairPhase = RepairPhase::WaitReequip;
            m_lastNpcActionTick = now;
            cb.setStateFn(AutoHuntState::Repair, "Re-equipping repaired item");
            return;

        case RepairPhase::WaitReequip: {
            CItem* equipped = hero->GetEquip(m_repairSlot);
            if (equipped && equipped->GetID() == m_repairItemId) {
                m_repairItemId = 0;
                m_repairPhase = RepairPhase::Unequip;
                return;
            }
            if (now - m_lastNpcActionTick > 2500)
                m_repairPhase = RepairPhase::Reequip;
            return;
        }
    }
}

// ── HuntTownService — HandleBuyArrowsState ───────────────────────────────────

void HuntTownService::HandleBuyArrowsState(CHero* hero, CGameMap* map,
    const AutoHuntSettings& settings, const HuntTownCallbacks& cb)
{
    if (!hero || !map) {
        cb.setStateFn(AutoHuntState::WaitingForGame, "Waiting for hero and map");
        return;
    }

    // Helper lambda: finish BuyArrows — go straight to zone.
    auto finishBuyArrows = [&]() {
        ResetBuyArrowsSequence();
        cb.beginTravelToZoneFn();
    };

    // Wait for any in-progress travel before acting on the blacksmith.
    // (Caller ensures travel->IsTraveling() is checked at the top of Update,
    //  but this guard handles the race between state dispatch and travel start.)
    // NOTE: We do NOT have travel here — the caller already gates on travel.IsTraveling()
    // before dispatching to us, so we just check the map.

    const BlacksmithEntry* bs = FindBlacksmithForMap(map->GetId());
    if (!bs) {
        // No blacksmith on this map — go to the zone city (which has one).
        // HandleTravelToZone will re-enter BuyArrows on arrival.
        spdlog::info("[hunt] No blacksmith on current map, traveling to zone city for arrows");
        finishBuyArrows();
        return;
    }

    CRole* blacksmith = FindNpcByName(bs->npcName, bs->pos, 16);
    const Position bsPos = blacksmith ? blacksmith->m_posMap : bs->pos;
    const int npcDist = CGameMap::TileDist(hero->m_posMap.x, hero->m_posMap.y, bsPos.x, bsPos.y);

    if (npcDist > 5) {
        cb.startPathNearTargetFn(hero, map, bsPos, 4);
        cb.setStateFn(AutoHuntState::BuyArrows, "Moving to Blacksmith");
        return;
    }

    if (blacksmith)
        m_blacksmithNpcId = blacksmith->GetID();

    const DWORD now = GetTickCount();
    const DWORD npcActionInterval = GetNpcActionIntervalMs(settings);

    switch (m_buyArrowsPhase) {
        case BuyArrowsPhase::MoveToBlacksmith:
            m_arrowsBoughtCount = 0;
            m_buyArrowsPhase = BuyArrowsPhase::BuyArrow;
            return;

        case BuyArrowsPhase::BuyArrow: {
            if (m_blacksmithNpcId == 0) {
                spdlog::warn("[hunt] Blacksmith NPC not found near ({},{})", bsPos.x, bsPos.y);
                m_buyArrowsPhase = BuyArrowsPhase::EquipArrows;
                return;
            }
            const int currentPacks = CountUsableArrowPacks(hero);
            if (currentPacks >= settings.arrowBuyCount || hero->IsBagFull()) {
                spdlog::info("[hunt] Arrow purchase complete: have {} packs (target {}), bought {}",
                    currentPacks, settings.arrowBuyCount, m_arrowsBoughtCount);
                m_buyArrowsPhase = BuyArrowsPhase::EquipArrows;
                return;
            }
            if (hero->GetSilver() < kArrowCost) {
                spdlog::info("[hunt] Not enough silver for arrows (have {}, need {}), bought {} so far",
                    hero->GetSilver(), kArrowCost, m_arrowsBoughtCount);
                m_buyArrowsPhase = BuyArrowsPhase::EquipArrows;
                return;
            }
            if (now - m_lastNpcActionTick < npcActionInterval)
                return;

            hero->BuyItem(m_blacksmithNpcId, settings.arrowTypeId);
            m_lastNpcActionTick = now;
            ++m_arrowsBoughtCount;
            m_buyArrowsPhase = BuyArrowsPhase::WaitBuy;
            spdlog::debug("[hunt] Buying arrow type {} from NPC {} (purchase #{}, silver={})",
                settings.arrowTypeId, m_blacksmithNpcId, m_arrowsBoughtCount, hero->GetSilver());
            cb.setStateFn(AutoHuntState::BuyArrows, "Buying arrows");
            return;
        }

        case BuyArrowsPhase::WaitBuy: {
            const int currentPacks = CountUsableArrowPacks(hero);
            if (currentPacks > m_arrowsBoughtCount - 1) {
                m_buyArrowsPhase = BuyArrowsPhase::BuyArrow;
                return;
            }
            if (now - m_lastNpcActionTick > 2500) {
                spdlog::warn("[hunt] Arrow buy timeout, retrying");
                m_buyArrowsPhase = BuyArrowsPhase::BuyArrow;
            }
            return;
        }

        case BuyArrowsPhase::EquipArrows: {
            CItem* equipped = hero->GetEquip(EquipSlot::LWEAPON);
            if (equipped && equipped->IsArrow() && equipped->GetDurability() > 3) {
                m_buyArrowsPhase = BuyArrowsPhase::WaitEquip;
                return;
            }
            for (const auto& itemRef : hero->m_deqItem) {
                if (!itemRef || !itemRef->IsArrow() || itemRef->GetDurability() <= 3)
                    continue;
                if (now - m_lastNpcActionTick < npcActionInterval)
                    return;
                hero->EquipItem(itemRef->GetID(), EquipSlot::LWEAPON);
                m_lastNpcActionTick = now;
                cb.setStateFn(AutoHuntState::BuyArrows, "Equipping arrows");
                m_buyArrowsPhase = BuyArrowsPhase::WaitEquip;
                return;
            }
            m_buyArrowsPhase = BuyArrowsPhase::WaitEquip;
            return;
        }

        case BuyArrowsPhase::WaitEquip: {
            CItem* equipped = hero->GetEquip(EquipSlot::LWEAPON);
            if ((equipped && equipped->IsArrow() && equipped->GetDurability() > 3)
                || now - m_lastNpcActionTick > 2500) {
                finishBuyArrows();
            }
            return;
        }
    }
}

// ── HuntTownService — HandleStoreState ───────────────────────────────────────

void HuntTownService::HandleStoreState(CHero* hero, CGameMap* map,
    const AutoHuntSettings& settings, const HuntTownCallbacks& cb)
{
    if (!hero || !map) {
        cb.setStateFn(AutoHuntState::WaitingForGame, "Waiting for hero and map");
        return;
    }

    if (map->GetId() != MAP_MARKET) {
        cb.beginTravelToMarketFn();
        return;
    }

    const DWORD now = GetTickCount();
    const DWORD npcActionInterval = GetNpcActionIntervalMs(settings);

    switch (m_storePhase) {
        case StorePhase::PackMeteors: {
            if (!settings.packMeteorsIntoScrolls) {
                m_storePhase = StorePhase::MoveToWarehouse;
                return;
            }

            const int meteorCount = CountInventoryItemsByType(hero, ItemTypeId::METEOR);
            if (meteorCount < 10) {
                m_storePhase = StorePhase::MoveToWarehouse;
                return;
            }

            CItem* meteor = FindInventoryItemByType(hero, ItemTypeId::METEOR);
            if (!meteor) {
                m_storePhase = StorePhase::MoveToWarehouse;
                return;
            }

            if (now - m_lastNpcActionTick < npcActionInterval)
                return;

            m_storeMeteorCountBefore = meteorCount;
            m_storeItemId = meteor->GetID();
            hero->UseItem(m_storeItemId);
            m_storePhase = StorePhase::WaitPackMeteors;
            m_lastNpcActionTick = now;
            cb.setStateFn(AutoHuntState::StoreItems, "Packing Meteors into MeteorScrolls");
            return;
        }

        case StorePhase::WaitPackMeteors: {
            const int meteorCount = CountInventoryItemsByType(hero, ItemTypeId::METEOR);
            if (meteorCount < m_storeMeteorCountBefore || meteorCount < 10) {
                m_storeItemId = 0;
                m_storeMeteorCountBefore = 0;
                m_storePhase = StorePhase::PackMeteors;
                return;
            }
            if (now - m_lastNpcActionTick > 2500) {
                m_storeItemId = 0;
                m_storeMeteorCountBefore = 0;
                m_storePhase = StorePhase::PackMeteors;
            }
            return;
        }

        case StorePhase::MoveToWarehouse: {
            const bool needsWarehouseItems = HasWarehouseItems(hero, settings);
            const bool needsSilverDeposit = settings.autoDepositSilver
                && hero->GetSilver() > (settings.silverKeepAmount > 0 ? (uint32_t)settings.silverKeepAmount : 0u);
            if (!needsWarehouseItems && !needsSilverDeposit) {
                m_storePhase = StorePhase::MoveToTreasureBank;
                return;
            }

            CRole* warehouseman = FindNpcByName("Warehouseman", kWarehousePos, 16);
            if (warehouseman)
                m_warehouseNpcId = warehouseman->GetID();

            const Position warehousePos = warehouseman ? warehouseman->m_posMap : kWarehousePos;
            const int npcDist = CGameMap::TileDist(hero->m_posMap.x, hero->m_posMap.y, warehousePos.x, warehousePos.y);
            if (npcDist > 5) {
                cb.startPathNearTargetFn(hero, map, warehousePos, 4);
                cb.setStateFn(AutoHuntState::StoreItems, "Moving to Warehouseman");
                return;
            }

            if (m_warehouseNpcId == 0) {
                cb.setStateFn(AutoHuntState::StoreItems, "Waiting for Warehouseman");
                return;
            }

            m_storePhase = StorePhase::OpenWarehouse;
            return;
        }

        case StorePhase::OpenWarehouse:
            if (now - m_lastNpcActionTick < npcActionInterval)
                return;
            hero->OpenWarehouse(m_warehouseNpcId);
            m_storePhase = StorePhase::WaitWarehouseOpen;
            m_lastNpcActionTick = now;
            cb.setStateFn(AutoHuntState::StoreItems, "Opening warehouse");
            return;

        case StorePhase::WaitWarehouseOpen:
            if ((hero->IsNpcActive() && hero->GetActiveNpc() == m_warehouseNpcId)
                || now - m_lastNpcActionTick > 1200) {
                m_storePhase = StorePhase::DepositWarehouse;
            }
            return;

        case StorePhase::DepositWarehouse: {
            for (const auto& itemRef : hero->m_deqItem) {
                if (!itemRef || !ShouldStoreWarehouseItem(settings, *itemRef))
                    continue;
                if (m_warehouseNpcId == 0)
                    return;
                if (now - m_lastNpcActionTick < npcActionInterval)
                    return;

                m_storeItemId = itemRef->GetID();
                hero->DepositWarehouseItem(m_warehouseNpcId, m_storeItemId);
                m_storePhase = StorePhase::WaitWarehouseDeposit;
                m_lastNpcActionTick = now;
                cb.setStateFn(AutoHuntState::StoreItems, "Depositing warehouse item");
                return;
            }

            m_storePhase = StorePhase::DepositSilver;
            return;
        }

        case StorePhase::WaitWarehouseDeposit:
            if (!FindInventoryItemById(hero, m_storeItemId)) {
                m_storeItemId = 0;
                m_storePhase = StorePhase::DepositWarehouse;
                return;
            }
            if (now - m_lastNpcActionTick > 2500)
                m_storePhase = StorePhase::DepositWarehouse;
            return;

        case StorePhase::DepositSilver: {
            if (!settings.autoDepositSilver || m_warehouseNpcId == 0) {
                m_storePhase = StorePhase::MoveToTreasureBank;
                return;
            }
            const uint32_t currentSilver = hero->GetSilver();
            const uint32_t keepAmount = settings.silverKeepAmount > 0 ? (uint32_t)settings.silverKeepAmount : 0;
            if (currentSilver <= keepAmount) {
                m_storePhase = StorePhase::MoveToTreasureBank;
                return;
            }
            if (now - m_lastNpcActionTick < npcActionInterval)
                return;
            const uint32_t depositAmount = currentSilver - keepAmount;
            hero->DepositWarehouseSilver(m_warehouseNpcId, depositAmount);
            m_lastNpcActionTick = now;
            m_storePhase = StorePhase::WaitSilverDeposit;
            cb.setStateFn(AutoHuntState::StoreItems, "Depositing silver");
            return;
        }

        case StorePhase::WaitSilverDeposit:
            if (now - m_lastNpcActionTick > 2500
                || hero->GetSilver() <= (settings.silverKeepAmount > 0 ? (uint32_t)settings.silverKeepAmount : 0u)) {
                m_storePhase = StorePhase::MoveToTreasureBank;
            }
            return;

        case StorePhase::MoveToTreasureBank: {
            if (!settings.storeTreasureBank || !HasTreasureBankItems(hero)) {
                m_storePhase = StorePhase::MoveToComposeBank;
                return;
            }

            CRole* treasureBank = FindNpcByName("TreasureBank", kTreasureBankPos, 16);
            if (treasureBank)
                m_treasureBankNpcId = treasureBank->GetID();

            const Position treasurePos = treasureBank ? treasureBank->m_posMap : kTreasureBankPos;
            const int npcDist = CGameMap::TileDist(hero->m_posMap.x, hero->m_posMap.y, treasurePos.x, treasurePos.y);
            if (npcDist > 5) {
                cb.startPathNearTargetFn(hero, map, treasurePos, 4);
                cb.setStateFn(AutoHuntState::StoreItems, "Moving to TreasureBank");
                return;
            }

            if (m_treasureBankNpcId == 0) {
                cb.setStateFn(AutoHuntState::StoreItems, "Waiting for TreasureBank");
                return;
            }

            m_storePhase = StorePhase::OpenTreasureBank;
            return;
        }

        case StorePhase::OpenTreasureBank:
            if (now - m_lastNpcActionTick < npcActionInterval)
                return;
            hero->OpenTreasureBank(m_treasureBankNpcId);
            m_storePhase = StorePhase::WaitTreasureBank;
            m_lastNpcActionTick = now;
            cb.setStateFn(AutoHuntState::StoreItems, "Opening TreasureBank");
            return;

        case StorePhase::WaitTreasureBank:
            if ((hero->IsNpcActive() && hero->GetActiveNpc() == m_treasureBankNpcId)
                || now - m_lastNpcActionTick > 1200) {
                m_storePhase = HasTreasureBankMeteorItems(hero)
                    ? StorePhase::DepositTreasureMeteors
                    : StorePhase::DepositTreasureDragonBalls;
            }
            return;

        case StorePhase::DepositTreasureMeteors:
            if (!HasTreasureBankMeteorItems(hero)) {
                m_storePhase = StorePhase::DepositTreasureDragonBalls;
                return;
            }
            if (now - m_lastNpcActionTick < npcActionInterval)
                return;
            hero->DepositTreasureBankMeteors(m_treasureBankNpcId);
            m_storePhase = StorePhase::WaitTreasureMeteors;
            m_lastNpcActionTick = now;
            cb.setStateFn(AutoHuntState::StoreItems, "Depositing Meteors");
            return;

        case StorePhase::WaitTreasureMeteors:
            if (!HasTreasureBankMeteorItems(hero)) {
                m_storePhase = StorePhase::DepositTreasureDragonBalls;
                return;
            }
            if (now - m_lastNpcActionTick > 2500)
                m_storePhase = StorePhase::DepositTreasureDragonBalls;
            return;

        case StorePhase::DepositTreasureDragonBalls:
            if (!HasTreasureBankDragonBallItems(hero)) {
                m_storePhase = StorePhase::MoveToComposeBank;
                return;
            }
            if (now - m_lastNpcActionTick < npcActionInterval)
                return;
            hero->DepositTreasureBankDragonBalls(m_treasureBankNpcId);
            m_storePhase = StorePhase::WaitTreasureDragonBalls;
            m_lastNpcActionTick = now;
            cb.setStateFn(AutoHuntState::StoreItems, "Depositing DragonBalls");
            return;

        case StorePhase::WaitTreasureDragonBalls:
            if (!HasTreasureBankDragonBallItems(hero)) {
                m_storePhase = StorePhase::MoveToComposeBank;
                return;
            }
            if (now - m_lastNpcActionTick > 2500)
                m_storePhase = StorePhase::MoveToComposeBank;
            return;

        case StorePhase::MoveToComposeBank: {
            if (!settings.storeComposeBank || !HasComposeBankItems(hero)) {
                ResetStoreSequence();
                cb.beginTravelToZoneFn();
                return;
            }

            CRole* composeBank = FindNpcByName("ComposeBank", kComposeBankPos, 16);
            if (composeBank)
                m_composeBankNpcId = composeBank->GetID();

            const Position composePos = composeBank ? composeBank->m_posMap : kComposeBankPos;
            const int npcDist = CGameMap::TileDist(hero->m_posMap.x, hero->m_posMap.y, composePos.x, composePos.y);
            if (npcDist > 5) {
                cb.startPathNearTargetFn(hero, map, composePos, 4);
                cb.setStateFn(AutoHuntState::StoreItems, "Moving to ComposeBank");
                return;
            }

            if (m_composeBankNpcId == 0) {
                cb.setStateFn(AutoHuntState::StoreItems, "Waiting for ComposeBank");
                return;
            }

            m_storePhase = StorePhase::OpenComposeBank;
            return;
        }

        case StorePhase::OpenComposeBank:
            if (now - m_lastNpcActionTick < npcActionInterval)
                return;
            hero->OpenComposeBank(m_composeBankNpcId);
            m_storePhase = StorePhase::WaitComposeBank;
            m_lastNpcActionTick = now;
            cb.setStateFn(AutoHuntState::StoreItems, "Opening ComposeBank");
            return;

        case StorePhase::WaitComposeBank:
            if ((hero->IsNpcActive() && hero->GetActiveNpc() == m_composeBankNpcId)
                || now - m_lastNpcActionTick > 1200) {
                m_storePhase = StorePhase::DepositComposeBank;
            }
            return;

        case StorePhase::DepositComposeBank:
            if (!HasComposeBankItems(hero)) {
                ResetStoreSequence();
                cb.beginTravelToZoneFn();
                return;
            }
            if (now - m_lastNpcActionTick < npcActionInterval)
                return;
            hero->DepositComposeBankAll();
            m_storePhase = StorePhase::WaitComposeBankDeposit;
            m_lastNpcActionTick = now;
            cb.setStateFn(AutoHuntState::StoreItems, "Depositing +1/+2 gear");
            return;

        case StorePhase::WaitComposeBankDeposit:
            if (!HasComposeBankItems(hero)) {
                ResetStoreSequence();
                cb.beginTravelToZoneFn();
                return;
            }
            if (now - m_lastNpcActionTick > 2500) {
                ResetStoreSequence();
                cb.beginTravelToZoneFn();
            }
            return;
    }
}
