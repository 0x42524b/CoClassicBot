#pragma once
#include "base.h"
#include "CHero.h"
#include <cstdint>
#include <vector>

class CItem;
struct CMapItem;
struct ItemTypeInfo;

// ── Item-ID list helpers ──────────────────────────────────────────────
bool ContainsItemId(const std::vector<uint32_t>& ids, uint32_t typeId);
void AddItemId(std::vector<uint32_t>& ids, uint32_t typeId);
void RemoveItemId(std::vector<uint32_t>& ids, uint32_t typeId);

// ── Inventory search ──────────────────────────────────────────────────
CItem* FindInventoryItemById(const CHero* hero, OBJID id);
CItem* FindInventoryItemByType(const CHero* hero, OBJID typeId);
int    CountInventoryItemsByType(const CHero* hero, OBJID typeId);

// ── Item classification ───────────────────────────────────────────────
int  GetDurabilityPercent(const CItem& item);
bool IsConsumablePotionType(const ItemTypeInfo& info, bool manaPotion);
bool IsEquipmentQualitySort(int itemSort);
bool IsGemTypeId(uint32_t typeId);

// ── Inventory predicate helper (template — must be defined in header) ──
// Requires CHero to be a complete type at instantiation; include CHero.h before use.
template <typename Predicate>
bool InventoryHasMatchingItem(const CHero* hero, Predicate&& predicate)
{
    if (!hero)
        return false;

    for (const auto& itemRef : hero->m_deqItem) {
        if (itemRef && predicate(*itemRef))
            return true;
    }
    return false;
}
