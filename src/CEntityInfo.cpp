#include "CEntityInfo.h"
#include "game.h"

namespace {
struct WarehouseItemSlot
{
    CItem* pItem = nullptr;
    void* pSharedControl = nullptr;
};
}

CEntityInfo* CEntityInfo::GetInstance()
{
    return Game::GetEntityInfo();
}

bool CEntityInfo::HasPendingTradeRequest() const
{
    return m_nTradeRequestState == TRADE_REQUEST_PENDING
        && m_idTradeRequester != 0
        && !m_szTradeRequesterName.empty();
}

OBJID CEntityInfo::GetTradeRequesterId() const
{
    return m_idTradeRequester;
}

const char* CEntityInfo::GetTradeRequesterName() const
{
    return m_szTradeRequesterName.c_str();
}

std::vector<CItem*> CEntityInfo::GetWarehouseItems() const
{
    std::vector<CItem*> items;
    if (!m_pWarehouseRing || m_qwWarehouseRingCount == 0 || m_qwWarehouseRingMask == 0)
        return items;

    auto** ring = reinterpret_cast<WarehouseItemSlot**>(m_pWarehouseRing);
    const uint64_t end = m_qwWarehouseRingStart + m_qwWarehouseRingCount;
    items.reserve((size_t)m_qwWarehouseRingCount);
    for (uint64_t index = m_qwWarehouseRingStart; index != end; ++index) {
        WarehouseItemSlot* slot = ring[(m_qwWarehouseRingMask - 1) & index];
        if (slot && slot->pItem)
            items.push_back(slot->pItem);
    }

    return items;
}

CItem* CEntityInfo::FindWarehouseItemById(OBJID idItem) const
{
    if (idItem == 0)
        return nullptr;

    for (CItem* item : GetWarehouseItems()) {
        if (item && item->GetID() == idItem)
            return item;
    }

    return nullptr;
}
