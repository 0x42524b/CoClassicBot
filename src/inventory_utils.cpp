#include "inventory_utils.h"
#include "CHero.h"
#include "CItem.h"
#include "CGameMap.h"
#include "itemtype.h"
#include "hunt_settings.h"
#include <algorithm>

// ── Item-ID list helpers ──────────────────────────────────────────────

bool ContainsItemId(const std::vector<uint32_t>& ids, uint32_t typeId)
{
    return std::find(ids.begin(), ids.end(), typeId) != ids.end();
}

void AddItemId(std::vector<uint32_t>& ids, uint32_t typeId)
{
    if (!ContainsItemId(ids, typeId))
        ids.push_back(typeId);
}

void RemoveItemId(std::vector<uint32_t>& ids, uint32_t typeId)
{
    ids.erase(std::remove(ids.begin(), ids.end(), typeId), ids.end());
}

// ── Inventory search ──────────────────────────────────────────────────

CItem* FindInventoryItemById(const CHero* hero, OBJID itemId)
{
    if (!hero || itemId == 0)
        return nullptr;

    for (const auto& itemRef : hero->m_deqItem) {
        if (itemRef && itemRef->GetID() == itemId)
            return itemRef.get();
    }
    return nullptr;
}

CItem* FindInventoryItemByType(const CHero* hero, OBJID typeId)
{
    if (!hero || typeId == 0)
        return nullptr;

    for (const auto& itemRef : hero->m_deqItem) {
        if (itemRef && itemRef->GetTypeID() == typeId)
            return itemRef.get();
    }
    return nullptr;
}

int CountInventoryItemsByType(const CHero* hero, OBJID typeId)
{
    if (!hero || typeId == 0)
        return 0;

    int count = 0;
    for (const auto& itemRef : hero->m_deqItem) {
        if (itemRef && itemRef->GetTypeID() == typeId)
            ++count;
    }
    return count;
}

// ── Item classification ───────────────────────────────────────────────

int GetDurabilityPercent(const CItem& item)
{
    if (item.GetMaxDurabilityRaw() <= 0)
        return 100;
    return (item.GetDurabilityRaw() * 100) / item.GetMaxDurabilityRaw();
}

bool IsConsumablePotionType(const ItemTypeInfo& info, bool manaPotion)
{
    if (info.id < 1000000 || info.id >= 1100000)
        return false;
    if (info.amount != 1 || info.amountLimit != 1)
        return false;
    return manaPotion ? info.mana > 0 : info.life > 0;
}

bool IsSelectedLootQuality(const AutoHuntSettings& settings, int quality)
{
    switch (quality) {
        case ItemQuality::REFINED: return settings.lootRefined;
        case ItemQuality::UNIQUE:  return settings.lootUnique;
        case ItemQuality::ELITE:   return settings.lootElite;
        case ItemQuality::SUPER:   return settings.lootSuper;
        default:                   return false;
    }
}

bool IsEquipmentQualitySort(int itemSort)
{
    switch (itemSort) {
        case ItemSort::HELMET:
        case ItemSort::NECKLACE:
        case ItemSort::ARMOR:
        case ItemSort::WEAPON_SINGLE_HAND:
        case ItemSort::WEAPON_DOUBLE_HAND:
        case ItemSort::SHIELD:
        case ItemSort::RING:
        case ItemSort::SHOES:
        case ItemSort::SPRITE:
        case ItemSort::KATANA:
            return true;
        default:
            return false;
    }
}

bool IsGemTypeId(uint32_t typeId)
{
    if (GetItemSort(typeId) != ItemSort::OTHER)
        return false;

    // Gem family item ids follow 7000X1-7000X3.
    return typeId >= 700001 && typeId < 701000 && (typeId % 10) >= 1 && (typeId % 10) <= 3;
}

bool MatchesSelectedLootQuality(const AutoHuntSettings& settings, const CMapItem& item)
{
    const uint32_t typeId = item.m_idType;
    const int suffix = typeId % 10;
    const int itemSort = GetItemSort(typeId);
    if (IsEquipmentQualitySort(itemSort))
        return IsSelectedLootQuality(settings, suffix);

    if (IsGemTypeId(typeId)) {
        switch (suffix) {
            case 2: return settings.lootRefined;
            case 3: return settings.lootSuper;
            default: return false;
        }
    }

    return false;
}
