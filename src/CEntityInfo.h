#pragma once
#include "base.h"
#include "CItem.h"
#include <string>
#include <vector>

// =====================================================================
// CEntityInfo - UI/entity singleton with incoming trade request state
//
// Offsets verified live with Cheat Engine while an incoming trade request
// from player 1022560 ("Mizu") was active.
// =====================================================================
#pragma pack(push, 1)
class CEntityInfo
{
private:
    BYTE _pad00[0xF80];

public:
    uint64_t m_nTradeRequestState;      // +0xF80 observed as 7 while request popup is active

private:
    BYTE _padF88[0xF90 - 0xF88];

public:
    OBJID m_idTradeRequester;           // +0xF90 requester UID

private:
    BYTE _padF94[0xF98 - 0xF94];

public:
    std::string m_szTradeRequesterName; // +0xF98 requester name

public:
    void* m_pWarehouseRing;            // +0xFB8 warehouse item pointer ring buffer
    uint64_t m_qwWarehouseRingMask;    // +0xFC0 ring capacity mask
    uint64_t m_qwWarehouseRingStart;   // +0xFC8 logical start index
    uint64_t m_qwWarehouseRingCount;   // +0xFD0 item count

private:
    BYTE _padFD8[0xFE0 - 0xFD8];

public:
    int m_nWarehouseCapacity;          // +0xFE0 visible warehouse slot count

    static constexpr uint64_t TRADE_REQUEST_PENDING = 7;

    static CEntityInfo* GetInstance();

    bool HasPendingTradeRequest() const;
    OBJID GetTradeRequesterId() const;
    const char* GetTradeRequesterName() const;
    std::vector<CItem*> GetWarehouseItems() const;
    CItem* FindWarehouseItemById(OBJID idItem) const;
};
#pragma pack(pop)

static_assert(offsetof(CEntityInfo, m_nTradeRequestState) == 0xF80, "CEntityInfo::m_nTradeRequestState");
static_assert(offsetof(CEntityInfo, m_idTradeRequester) == 0xF90, "CEntityInfo::m_idTradeRequester");
static_assert(offsetof(CEntityInfo, m_szTradeRequesterName) == 0xF98, "CEntityInfo::m_szTradeRequesterName");
static_assert(offsetof(CEntityInfo, m_pWarehouseRing) == 0xFB8, "CEntityInfo::m_pWarehouseRing");
static_assert(offsetof(CEntityInfo, m_qwWarehouseRingMask) == 0xFC0, "CEntityInfo::m_qwWarehouseRingMask");
static_assert(offsetof(CEntityInfo, m_qwWarehouseRingStart) == 0xFC8, "CEntityInfo::m_qwWarehouseRingStart");
static_assert(offsetof(CEntityInfo, m_qwWarehouseRingCount) == 0xFD0, "CEntityInfo::m_qwWarehouseRingCount");
static_assert(offsetof(CEntityInfo, m_nWarehouseCapacity) == 0xFE0, "CEntityInfo::m_nWarehouseCapacity");
