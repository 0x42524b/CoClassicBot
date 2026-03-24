#pragma once
#include "CRole.h"
#include "CItem.h"
#include "CMagic.h"
#include "CStatTable.h"

struct CMapItem;

// =====================================================================
// Equipment slot indices (0-based, 8 slots total)
// From FUN_140189950 — equipment update handler
// =====================================================================
namespace EquipSlot {
    constexpr int HEAD     = 0;
    constexpr int NECKLACE = 1;
    constexpr int ARMOR    = 2;
    constexpr int RWEAPON  = 3;  // right hand (main weapon)
    constexpr int LWEAPON  = 4;  // left hand (shield / dual wield)
    constexpr int RING     = 5;
    constexpr int GARMENT  = 6;
    constexpr int BOOTS    = 7;
    constexpr int COUNT    = 8;
}

const char* GetEquipSlotName(int slot);

// =====================================================================
// CHero — the player's controllable character
//
// Hierarchy: CMapObj -> CRole -> CHero
//
// The weak_ptr at CRole+0x08 (from enable_shared_from_this) was
// previously mistaken for a separate shared_ptr<CRole>; it actually
// points back to the object itself.
//
// Offsets verified via Ghidra + Cheat Engine.
// =====================================================================
#pragma pack(push, 1)
class CHero : public CRole
{
public:
    static CHero* GetSingletonPtr();

    static constexpr int MAX_BAG_ITEMS = 40;

private:
    BYTE _pad71C[0x968 - 0x71C]; // +0x71C  hero runtime fields

public:
    CStatTable* m_pStatTable;      // +0x968 current HP/stat table

private:
    BYTE _pad970[0xA30 - 0x970];   // +0x970 gap to runtime A30 slot

public:
    uint64_t m_qwRuntimeA30;       // +0xA30 movement timestamp / UI runtime value

private:
    BYTE _padA38[0xB20 - 0xA38];   // +0xA38 gap to inventory deque

public:
    std::deque<PItem> m_deqItem;   // +0xB20  inventory items

private:
    BYTE _padDeq[0xB88 - 0xB20 - sizeof(std::deque<PItem>)]; // gap between deque end and equipment

public:
    PItem m_equipment[EquipSlot::COUNT]; // +0xB88  equipped items (8 shared_ptr<CItem>)

private:
    BYTE _padC08[0xCA8 - 0xC08];   // +0xC08 gap to max mana cache

public:
    int32_t m_nMaxMana;            // +0xCA8 cached max MP
    uint8_t m_bMaxManaValid;       // +0xCAC max MP cache-valid byte

private:
    BYTE _padCAD[0x1000 - 0xCAD];  // +0xCAD

public:
    BOOL m_bNpcActive;             // +0x1000 active NPC dialog flag

private:
    BYTE _pad1004[0x1918 - 0x1004]; // +0x1004 gap to magic vector

public:
    std::vector<PMagic> m_vecMagic; // +0x1918  learned skills

private:
    BYTE _pad1930[0x3724 - 0x1930]; // +0x1930 gap to active NPC UID

public:
    OBJID m_idActiveNpc;            // +0x3724 NPC entity currently in dialog

private:
    BYTE _pad3728[0x3740 - 0x3728]; // +0x3728 gap to VIP flag

public:
    BOOL m_bVip;                    // +0x3740 VIP status flag

    // ── Helpers ──
    bool IsBagFull() const { return m_deqItem.size() >= MAX_BAG_ITEMS; }
    CItem* GetEquip(int slot) const {
        if (slot < 0 || slot >= EquipSlot::COUNT) return nullptr;
        return m_equipment[slot].get();
    }

    void Jump(int nX, int nY);
    void JumpPacket(int nX, int nY);
    void Walk(int nX, int nY);
    void Attack(OBJID idTarget);
    void AttackTarget(OBJID idTarget, const Position& posTarget);
    void ShootTarget(OBJID idTarget);
    void MagicAttack(OBJID idMagic, const Position& posTarget);
    void PickupItem(OBJID idItem, const Position& pos);
    void PickupItem(const CMapItem& item);
    void MagicAttack(OBJID idMagic, OBJID idTarget, const Position& posTarget);
    void StartMining();
    void UseItem(OBJID idItem);
    void DropItem(OBJID idItem, const Position& pos);
    void EquipItem(OBJID idItem, int slot);
    void UnequipItem(OBJID idItem, int slot);
    void RepairItem(OBJID idItem);
    void OpenWarehouse(OBJID idNpc);
    void DepositWarehouseItem(OBJID idNpc, OBJID idItem);
    void DepositWarehouseSilver(OBJID idNpc, uint32_t amount);
    void WithdrawWarehouseItem(OBJID idNpc, OBJID idItem);
    void OpenTreasureBank(OBJID idNpc);
    void DepositTreasureBankMeteors(OBJID idNpc);
    void DepositTreasureBankDragonBalls(OBJID idNpc);
    void OpenComposeBank(OBJID idNpc);
    void DepositComposeBankAll();
    void CancelFly();
    void Sit();
    void ReviveInTown();
    bool VipTeleport(OBJID mapId);
    void VipTeleportTwinCity();
    void BuyItem(OBJID idNpc, uint32_t typeId);
    void SellItem(OBJID idNpc, OBJID idItem);
    void StartTrade(OBJID idPlayer);
    void OfferTradeItem(OBJID idItem);
    void AcceptTrade(OBJID idPlayer);
    void CancelTrade(OBJID idPlayer);

    // ── NPC interaction (packet-based) ──
    void ActivateNpc(OBJID idNpc);
    void AnswerNpc(int answer);                  // uses default taskId=101
    void AnswerNpcEx(int answer, int taskId);    // explicit task ID
    bool IsNpcActive() const;
    OBJID GetActiveNpc() const;

    // ── State queries ──
    int GetCurrentHp() const;
    int GetMaxHp() const;
    int GetCurrentMana() const;
    int GetMaxMana() const;
    void RefreshSilverCache(bool trusted = false) const;
    void SetTrustedSilver(uint32_t value) const;
    bool HasTrustedSilverCache() const;
    uint64_t GetSilverRuntimeValue() const { return m_qwRuntimeA30; }
    uint32_t GetSilver() const;
    CMagic* FindMagicByName(const char* name) const;
    CMagic* FindMagicById(OBJID idMagic) const;
    bool IsVip() const;
};
#pragma pack(pop)

static_assert(offsetof(CHero, m_pStatTable) == 0x968, "CHero::m_pStatTable");
static_assert(offsetof(CHero, m_qwRuntimeA30) == 0xA30, "CHero::m_qwRuntimeA30");
static_assert(offsetof(CHero, m_nMaxMana) == 0xCA8, "CHero::m_nMaxMana");
static_assert(offsetof(CHero, m_bMaxManaValid) == 0xCAC, "CHero::m_bMaxManaValid");
static_assert(offsetof(CHero, m_bNpcActive) == 0x1000, "CHero::m_bNpcActive");
static_assert(offsetof(CHero, m_deqItem) == 0xB20, "CHero::m_deqItem");
static_assert(offsetof(CHero, m_equipment) == 0xB88, "CHero::m_equipment");
static_assert(offsetof(CHero, m_vecMagic) == 0x1918, "CHero::m_vecMagic");
static_assert(offsetof(CHero, m_idActiveNpc) == 0x3724, "CHero::m_idActiveNpc");
static_assert(offsetof(CHero, m_bVip) == 0x3740, "CHero::m_bVip");

#define g_objHero (*CHero::GetSingletonPtr())
