#pragma once
#include "base.h"
#include "itemtype.h"

// =====================================================================
// CItem — inventory item
//
// Offsets verified via Cheat Engine on the 64-bit client.
// =====================================================================
// =====================================================================
// Item quality — derived from typeId % 10
// =====================================================================
namespace ItemQuality {
    constexpr int NONE    = 0;
    constexpr int NORMAL  = 3;
    constexpr int REFINED = 6;
    constexpr int UNIQUE  = 7;
    constexpr int ELITE   = 8;
    constexpr int SUPER   = 9;
}

inline const char* GetQualityName(int q)
{
    switch (q) {
        case ItemQuality::NORMAL:  return "Normal";
        case ItemQuality::REFINED: return "Refined";
        case ItemQuality::UNIQUE:  return "Unique";
        case ItemQuality::ELITE:   return "Elite";
        case ItemQuality::SUPER:   return "Super";
        default:                   return "None";
    }
}

namespace ItemSort {
    constexpr int INVALID = -1;
    constexpr int EXPEND = 0;
    constexpr int HELMET = 1;
    constexpr int NECKLACE = 2;
    constexpr int ARMOR = 3;
    constexpr int WEAPON_SINGLE_HAND = 4;
    constexpr int WEAPON_DOUBLE_HAND = 5;
    constexpr int SHIELD = 6;
    constexpr int RING = 7;
    constexpr int SHOES = 8;
    constexpr int OTHER = 9;
    constexpr int MOUNT = 10;
    constexpr int SPRITE = 11;
    constexpr int TREASURE = 12;
    constexpr int KATANA = 13;
}

namespace ItemTypeId {
    constexpr OBJID TWIN_CITY_GATE = 1060020;
    constexpr OBJID DESERT_CITY_GATE = 1060021;
    constexpr OBJID APE_CITY_GATE = 1060022;
    constexpr OBJID CASTLE_GATE = 1060023;
    constexpr OBJID BIRD_ISLAND_GATE = 1060024;
    constexpr OBJID STONE_CITY_GATE = 1060102;
    constexpr OBJID DRAGONBALL = 1088000;
    constexpr OBJID METEOR = 1088001;
    constexpr OBJID METEOR_TEAR = 1088002;
    constexpr OBJID METEOR_SCROLL = 720027;
    constexpr OBJID DB_SCROLL = 720028;
    constexpr OBJID MEGA_METEOR_SCROLL = 720029;
}

inline int GetItemSort(OBJID idType)
{
    int type = ItemSort::INVALID;
    switch ((idType % 10000000) / 100000) {
        case 10:
            type = ItemSort::EXPEND;
            break;

        case 1:
            switch ((idType % 1000000) / 10000) {
                case 11:
                case 14:
                case 17:
                    type = ItemSort::HELMET;
                    break;
                case 12:
                    type = ItemSort::NECKLACE;
                    break;
                case 13:
                    type = ItemSort::ARMOR;
                    break;
                case 15:
                    type = ItemSort::RING;
                    break;
                case 16:
                    type = ItemSort::SHOES;
                    break;
                case 18:
                case 19:
                    type = ItemSort::SPRITE;
                    break;
            }
            break;

        case 4:
            type = ItemSort::WEAPON_SINGLE_HAND;
            break;

        case 5:
            type = ItemSort::WEAPON_DOUBLE_HAND;
            break;

        case 6:
            type = ItemSort::KATANA;
            break;

        case 7:
            type = ItemSort::OTHER;
            break;

        case 9:
            type = ItemSort::SHIELD;
            break;
    }

    const int sortGroup = (idType % 10000000) / 100000;
    if (sortGroup > 19 && sortGroup <= 29)
        type = ItemSort::MOUNT;
    return type;
}

// =====================================================================
// Gem/socket types — stored as byte encoding: tens = gem class, units = level
// 0 = no socket, 255 = open empty socket
// Gem class IDs match item type encoding: 7000X1-7000X3 where X = class
// =====================================================================
inline const char* GetGemClassName(uint8_t gem)
{
    if (gem == 0) return "-";
    if (gem == 255) return "Empty";
    switch (gem / 10) {
        case 0: return "Phoenix";    // 700001-03
        case 1: return "Dragon";     // 700011-13
        case 2: return "Fury";       // 700021-23
        case 3: return "Rainbow";    // 700031-33
        case 4: return "Kylin";      // 700041-43
        case 5: return "Violet";     // 700051-53
        case 6: return "Moon";       // 700061-63
        case 7: return "Tortoise";   // 700071-73
        case 10: return "Thunder";   // 700101-03
        case 12: return "Glory";     // 700121-23
        default: return "Gem";
    }
}

inline const char* GetGemLevelName(uint8_t gem)
{
    switch (gem % 10) {
        case 1: return "Nml";
        case 2: return "Ref";
        case 3: return "Sup";
        default: return "";
    }
}

#pragma pack(push, 1)
class CItem
{
public:
    virtual ~CItem() {}

    OBJID m_id;                    // +0x08  item instance UID
private:
    BYTE _pad0C[0x04];            // +0x0C
public:
    OBJID m_idType;                // +0x10  item type ID
private:
    BYTE _pad14[0x04];            // +0x14
public:
    char m_szName[16];             // +0x18  null-terminated ASCII name

private:
    BYTE _pad28[0x62 - 0x28];     // +0x28  gap to attribute fields

public:
    uint16_t m_nAmount;            // +0x62  current durability raw, or current arrow count
    uint16_t m_nAmountLimit;       // +0x64  max durability raw, or max arrow count
    uint8_t  m_nStatus;            // +0x66  item status flags
    uint8_t  m_nGem1;              // +0x67  socket 1 gem type (0=none, 255=empty)
    uint8_t  m_nGem2;              // +0x68  socket 2 gem type (0=none, 255=empty)
    uint8_t  m_nMagic1;            // +0x69  magic attribute 1
    uint8_t  m_nBless;             // +0x6A  lucky/bless (0-7)
    uint8_t  m_nAddition;          // +0x6B  plus level (+0 to +12)

    // ── Helpers ──
    OBJID GetID() const { return m_id; }
    OBJID GetTypeID() const { return m_idType; }
    const char* GetName() const { return m_szName; }

    int  GetQuality() const { return m_idType % 10; }
    int  GetSort() const { return GetItemSort(m_idType); }
    const char* GetQualityName() const { return ::GetQualityName(GetQuality()); }
    int  GetPlus() const { return m_nAddition; }
    int  GetBless() const { return m_nBless; }
    int  GetDurability() const { return IsArrow() ? m_nAmount : m_nAmount / 100; }
    int  GetMaxDurability() const { return IsArrow() ? m_nAmountLimit : m_nAmountLimit / 100; }
    int  GetDurabilityRaw() const { return m_nAmount; }
    int  GetMaxDurabilityRaw() const { return m_nAmountLimit; }
    uint8_t GetGem1() const { return m_nGem1; }
    uint8_t GetGem2() const { return m_nGem2; }
    bool HasSocket1() const { return m_nGem1 != 0; }
    bool HasSocket2() const { return m_nGem2 != 0; }
    bool IsWearableEquipment() const {
        const int sort = GetSort();
        return sort >= ItemSort::HELMET && sort <= ItemSort::SHOES;
    }
    bool IsEquipment() const {
        const int sort = GetSort();
        return (sort >= ItemSort::HELMET && sort <= ItemSort::SHOES) || sort == ItemSort::TREASURE;
    }
    bool IsDragonBall() const { return m_idType == ItemTypeId::DRAGONBALL; }
    bool IsMeteor() const { return m_idType == ItemTypeId::METEOR; }
    bool IsMeteorTear() const { return m_idType == ItemTypeId::METEOR_TEAR; }
    bool IsArrow() const {
        if (const ItemTypeInfo* info = GetItemTypeInfo(m_idType)) {
            if (info->name.find("Arrow") != std::string::npos)
                return true;
        }
        return m_idType >= 1050000 && m_idType < 1060000;
    }
    bool IsTreasureItem() const {
        return m_idType >= ItemTypeId::DRAGONBALL && m_idType <= ItemTypeId::METEOR_TEAR;
    }
    bool IsTwinCityGate() const { return m_idType == ItemTypeId::TWIN_CITY_GATE; }
    bool IsMeteorScroll() const { return m_idType == ItemTypeId::METEOR_SCROLL; }
    bool IsDBScroll() const { return m_idType == ItemTypeId::DB_SCROLL; }
    bool IsMegaMeteorScroll() const { return m_idType == ItemTypeId::MEGA_METEOR_SCROLL; }
};
#pragma pack(pop)

static_assert(offsetof(CItem, m_id)           == 0x08, "CItem::m_id");
static_assert(offsetof(CItem, m_idType)       == 0x10, "CItem::m_idType");
static_assert(offsetof(CItem, m_szName)       == 0x18, "CItem::m_szName");
static_assert(offsetof(CItem, m_nAmount)      == 0x62, "CItem::m_nAmount");
static_assert(offsetof(CItem, m_nAmountLimit) == 0x64, "CItem::m_nAmountLimit");
static_assert(offsetof(CItem, m_nStatus)      == 0x66, "CItem::m_nStatus");
static_assert(offsetof(CItem, m_nGem1)        == 0x67, "CItem::m_nGem1");
static_assert(offsetof(CItem, m_nGem2)        == 0x68, "CItem::m_nGem2");
static_assert(offsetof(CItem, m_nMagic1)      == 0x69, "CItem::m_nMagic1");
static_assert(offsetof(CItem, m_nBless)       == 0x6A, "CItem::m_nBless");
static_assert(offsetof(CItem, m_nAddition)    == 0x6B, "CItem::m_nAddition");

using PItem = Ref<CItem>;
