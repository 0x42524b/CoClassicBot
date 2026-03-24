#include "CHero.h"
#include "CGameMap.h"
#include "config.h"
#include "game.h"
#include "gateway.h"
#include "packets.h"
#include "plugins/plugin_mgr.h"
#include "plugins/travel_plugin.h"
#include "log.h"
#include <cstring>

namespace {
constexpr uint16_t kMsgActionPacketType = 0x03F2;
constexpr uint16_t kMsgMapItemPacketType = 0x044D;
constexpr uint32_t kMsgActionModeRevive = 25;
constexpr uint32_t kMsgMapItemModePickup = 3;
constexpr uint16_t kMsgVipPacketType = 0x1B5C;
constexpr uint16_t kMsgTradePacketType = 0x0420;
constexpr uint32_t kMsgVipModeTeleport = 1;
constexpr uint32_t kVipDestinationTwinCity = 1;
constexpr uint32_t kVipDestinationPhoenixCastle = 2;
constexpr uint32_t kVipDestinationApeMountain = 3;
constexpr uint32_t kVipDestinationDesertCity = 4;
constexpr uint32_t kVipDestinationBirdIsland = 5;
constexpr uint32_t kTradeModeStart = 1;
constexpr uint32_t kTradeModeCancel = 2;
constexpr uint32_t kTradeModeAddItem = 6;
constexpr uint32_t kTradeModeAccept = 10;
constexpr uint64_t kSilverTimestampWindowMs = 5000;
constexpr uint32_t kMaxInitialUntrustedSilver = 100000000;
constexpr uint32_t kMaxUntrustedSilverDelta = 5000000;

using GetTimestampFn = long long*(*)(long long*);

struct SilverCacheState {
    OBJID heroId = 0;
    uint32_t value = 0;
    bool hasValue = false;
    bool isTrusted = false;
};

SilverCacheState g_silverCache;

void ResetSilverCacheForHero(OBJID heroId)
{
    if (g_silverCache.heroId == heroId)
        return;

    g_silverCache = {};
    g_silverCache.heroId = heroId;
}

void StoreSilverCache(OBJID heroId, uint32_t value, bool trusted)
{
    ResetSilverCacheForHero(heroId);
    if (g_silverCache.isTrusted && !trusted)
        return;

    g_silverCache.value = value;
    g_silverCache.hasValue = true;
    g_silverCache.isTrusted = trusted;
}

bool IsPlausibleUntrustedSilverValue(uint32_t candidate)
{
    if (!g_silverCache.hasValue)
        return candidate <= kMaxInitialUntrustedSilver;

    const uint32_t current = g_silverCache.value;
    const uint32_t delta = (candidate > current) ? (candidate - current) : (current - candidate);
    return delta <= kMaxUntrustedSilverDelta;
}

uint64_t GetGameTimestampMs()
{
    static auto fn = Game::Resolve<GetTimestampFn>(0x0C6A80);
    if (!fn)
        return 0;

    long long timestampNs = 0;
    fn(&timestampNs);
    return static_cast<uint64_t>(timestampNs / 1000000);
}

bool LooksLikeMovementTimestamp(uint64_t value, uint64_t nowMs)
{
    return nowMs != 0 && value <= nowMs && nowMs - value <= kSilverTimestampWindowMs;
}

uint32_t GetVipDestinationForMap(OBJID mapId)
{
    switch (mapId) {
        case MAP_TWIN_CITY:      return kVipDestinationTwinCity;
        case MAP_PHOENIX_CASTLE: return kVipDestinationPhoenixCastle;
        case MAP_APE_MOUNTAIN:   return kVipDestinationApeMountain;
        case MAP_DESERT_CITY:    return kVipDestinationDesertCity;
        case MAP_BIRD_ISLAND:    return kVipDestinationBirdIsland;
        default:                 return 0;
    }
}
}

const char* GetEquipSlotName(int slot)
{
    switch (slot) {
        case EquipSlot::HEAD:     return "Head";
        case EquipSlot::NECKLACE: return "Necklace";
        case EquipSlot::ARMOR:    return "Armor";
        case EquipSlot::RWEAPON:  return "R.Hand";
        case EquipSlot::LWEAPON:  return "L.Hand";
        case EquipSlot::RING:     return "Ring";
        case EquipSlot::GARMENT:  return "Garment";
        case EquipSlot::BOOTS:    return "Boots";
        default:                  return "Unknown";
    }
}

CHero* CHero::GetSingletonPtr()
{
    auto* mgr = Game::GetRoleMgr();
    if (!mgr || !mgr->m_pHero) return nullptr;
    return mgr->m_pHero;
}

static int WriteVarint(uint8_t* buf, uint32_t value);

void CHero::Jump(int nX, int nY)
{
    const TravelSettings& ts = GetTravelSettings();

    // Check if travel wants packet jump
    const bool travelActive = [&]() {
        if (auto* tp = PluginManager::Get().GetPlugin<TravelPlugin>())
            return tp->IsTraveling();
        return false;
    }();
    const bool wantPacketJump = travelActive && ts.usePacketJump;

    if (!wantPacketJump) {
        GameCall::CHero_Jump()(this, nX, nY);
        return;
    }

    // Suppress packet jump when other players are nearby — fall back to
    // the native jump so the game plays a normal animation.
    if (AreOtherPlayersNearby(GetID())) {
        GameCall::CHero_Jump()(this, nX, nY);
    } else {
        // SetCommand tells the game a jump is happening so it doesn't
        // reset m_posMap on the next frame. ApplyLocalJumpPrediction
        // (inside SendJumpPacket) then overwrites the command status
        // to ACCOMPLISH and snaps the position instantly.
        // Pass the command as already accomplished so the game registers
        // the jump (preventing m_posMap resets) without starting its
        // animation engine which would fight with prediction.
        CCommand cmd = {};
        cmd.iType = _COMMAND_JUMP;
        cmd.iStatus = _CMDSTATUS_ACCOMPLISH;
        cmd.posTarget = Position(nX, nY);

        const uint64_t nowMs = GetGameTimestampMs();
        cmd.dwTimestamp = static_cast<DWORD>(nowMs);
        cmd.bDiagnal = m_posMap.DistanceTo(cmd.posTarget) > 18.0f ? TRUE : FALSE;

        SetCommand(&cmd);
        m_qwRuntimeA30 = nowMs > 5000 ? (nowMs - 5000) : 0;

        SendJumpPacket(GetID(), nX, nY, 0);
    }
}

void CHero::Walk(int nX, int nY)
{
    GameCall::CHero_Walk()(this, nX, nY);
}

void CHero::Attack(OBJID idTarget)
{
    CRoleMgr* mgr = Game::GetRoleMgr();
    if (!mgr)
        return;

    for (const auto& roleRef : mgr->m_deqRole) {
        if (roleRef && roleRef->GetID() == idTarget) {
            AttackTarget(idTarget, roleRef->m_posMap);
            return;
        }
    }
}

void CHero::AttackTarget(OBJID idTarget, const Position& posTarget)
{
    uint8_t buf[48] = {};
    int off = 4;

    buf[off++] = 0x08;
    off += WriteVarint(buf + off, GetID());

    buf[off++] = 0x10;
    off += WriteVarint(buf + off, idTarget);

    buf[off++] = 0x18;
    off += WriteVarint(buf + off, (uint32_t)posTarget.x);

    buf[off++] = 0x20;
    off += WriteVarint(buf + off, (uint32_t)posTarget.y);

    buf[off++] = 0x30;
    off += WriteVarint(buf + off, 2); // physical attack interaction

    *(uint16_t*)buf = (uint16_t)off;
    *(uint16_t*)(buf + 2) = 0x03FE;
    SendPacket(buf, off);
}

void CHero::ShootTarget(OBJID idTarget)
{
    // Get equipped weapon (right hand) ID for the shoot packet
    CItem* weapon = GetEquip(EquipSlot::RWEAPON);
    if (!weapon)
        return;

    uint8_t buf[48] = {};
    int off = 4;

    buf[off++] = 0x08;
    off += WriteVarint(buf + off, GetID());

    buf[off++] = 0x10;
    off += WriteVarint(buf + off, idTarget);

    buf[off++] = 0x28; // field 5: equipped weapon item ID
    off += WriteVarint(buf + off, weapon->GetID());

    buf[off++] = 0x30;
    off += WriteVarint(buf + off, 25); // archer/shoot interaction

    *(uint16_t*)buf = (uint16_t)off;
    *(uint16_t*)(buf + 2) = 0x03FE;
    SendPacket(buf, off);
}

void CHero::PickupItem(OBJID idItem, const Position& pos)
{
    if (CGameMap* map = Game::GetMap()) {
        for (const auto& itemRef : map->m_vecItems) {
            if (!itemRef || itemRef->m_id != idItem)
                continue;
            if (itemRef->m_pos.x != pos.x || itemRef->m_pos.y != pos.y)
                continue;
            PickupItem(*itemRef);
            return;
        }
    }

    CCommand cmd = {};
    cmd.iType = _COMMAND_PICKUP;
    cmd.iStatus = _CMDSTATUS_BEGIN;
    cmd.idTarget = idItem;
    cmd.posTarget = pos;
    SetCommand(&cmd);
}

static int WriteVarint(uint8_t* buf, uint32_t value)
{
    int n = 0;
    while (value > 0x7F) {
        buf[n++] = (uint8_t)(value & 0x7F) | 0x80;
        value >>= 7;
    }
    buf[n++] = (uint8_t)value;
    return n;
}

static int WriteFixed32(uint8_t* buf, uint32_t value)
{
    memcpy(buf, &value, sizeof(value));
    return sizeof(value);
}

static bool SendPickupItemPacket(const CMapItem& item)
{
    uint8_t buf[48] = {};
    int off = 4;

    buf[off++] = 0x0D;
    off += WriteFixed32(buf + off, item.m_id);

    buf[off++] = 0x10;
    off += WriteVarint(buf + off, item.m_idType);

    buf[off++] = 0x18;
    off += WriteVarint(buf + off, static_cast<uint32_t>(item.m_pos.x));

    buf[off++] = 0x20;
    off += WriteVarint(buf + off, static_cast<uint32_t>(item.m_pos.y));

    buf[off++] = 0x28;
    off += WriteVarint(buf + off, static_cast<uint32_t>(item.GetPlus()));

    buf[off++] = 0x30;
    off += WriteVarint(buf + off, kMsgMapItemModePickup);

    *(uint16_t*)buf = (uint16_t)off;
    *(uint16_t*)(buf + 2) = kMsgMapItemPacketType;
    return SendPacket(buf, off);
}

void CHero::PickupItem(const CMapItem& item)
{
    if (!SendPickupItemPacket(item)) {
        CCommand cmd = {};
        cmd.iType = _COMMAND_PICKUP;
        cmd.iStatus = _CMDSTATUS_BEGIN;
        cmd.idTarget = item.m_id;
        cmd.posTarget = item.m_pos;
        SetCommand(&cmd);
    }
}

static bool SendMsgItem(OBJID idItem, uint32_t action, int slot = 0)
{
    uint8_t buf[32] = {};
    int off = 4;

    buf[off++] = 0x08;
    off += WriteVarint(buf + off, idItem);

    if (slot > 0) {
        buf[off++] = 0x10;
        off += WriteVarint(buf + off, (uint32_t)slot);
    }

    buf[off++] = 0x28;
    off += WriteVarint(buf + off, action);

    *(uint16_t*)buf = (uint16_t)off;
    *(uint16_t*)(buf + 2) = 0x03F1;
    return SendPacket(buf, off);
}

static bool SendWarehousePacket(OBJID idNpc, uint32_t action, uint32_t packageType, OBJID idItem = 0)
{
    uint8_t buf[32] = {};
    int off = 4;

    buf[off++] = 0x08;
    off += WriteVarint(buf + off, idNpc);

    buf[off++] = 0x10;
    off += WriteVarint(buf + off, action);

    buf[off++] = 0x18;
    off += WriteVarint(buf + off, packageType);

    if (idItem != 0) {
        buf[off++] = 0x20;
        off += WriteVarint(buf + off, idItem);
    }

    *(uint16_t*)buf = (uint16_t)off;
    *(uint16_t*)(buf + 2) = 0x044E;
    return SendPacket(buf, off);
}

static bool SendSimplePacket(uint16_t type, uint8_t fieldTag, uint32_t value)
{
    uint8_t buf[16] = {};
    int off = 4;

    buf[off++] = fieldTag;
    off += WriteVarint(buf + off, value);

    *(uint16_t*)buf = (uint16_t)off;
    *(uint16_t*)(buf + 2) = type;
    return SendPacket(buf, off);
}

static bool SendBuyItemPacket(OBJID idNpc, uint32_t typeId)
{
    uint8_t buf[32] = {};
    int off = 4;

    buf[off++] = 0x08;
    off += WriteVarint(buf + off, idNpc);

    buf[off++] = 0x10;
    off += WriteVarint(buf + off, typeId);

    buf[off++] = 0x28;
    off += WriteVarint(buf + off, 1);

    *(uint16_t*)buf = (uint16_t)off;
    *(uint16_t*)(buf + 2) = 0x03F1;
    return SendPacket(buf, off);
}

static bool SendSellItemPacket(OBJID idNpc, OBJID idItem)
{
    uint8_t buf[32] = {};
    int off = 4;

    buf[off++] = 0x08;
    off += WriteVarint(buf + off, idNpc);

    buf[off++] = 0x10;
    off += WriteVarint(buf + off, idItem);

    buf[off++] = 0x28;
    off += WriteVarint(buf + off, 2);

    *(uint16_t*)buf = (uint16_t)off;
    *(uint16_t*)(buf + 2) = 0x03F1;
    return SendPacket(buf, off);
}

static bool SendTradePacket(uint32_t mode, uint32_t value)
{
    uint8_t buf[32] = {};
    int off = 4;

    buf[off++] = 0x08;
    off += WriteVarint(buf + off, mode);

    buf[off++] = 0x10;
    off += WriteVarint(buf + off, value);

    *(uint16_t*)buf = (uint16_t)off;
    *(uint16_t*)(buf + 2) = kMsgTradePacketType;
    return SendPacket(buf, off);
}

static bool SendDropItemPacket(OBJID idItem, const Position& pos)
{
    uint8_t buf[32] = {};
    int off = 4;

    buf[off++] = 0x08;
    off += WriteVarint(buf + off, idItem);

    buf[off++] = 0x18;
    off += WriteVarint(buf + off, (uint32_t)pos.x);

    buf[off++] = 0x20;
    off += WriteVarint(buf + off, (uint32_t)pos.y);

    buf[off++] = 0x28;
    off += WriteVarint(buf + off, 3);

    *(uint16_t*)buf = (uint16_t)off;
    *(uint16_t*)(buf + 2) = 0x03F1;
    return SendPacket(buf, off);
}

void CHero::MagicAttack(OBJID idMagic, OBJID idTarget, const Position& posTarget)
{
    uint8_t buf[48] = {};
    int off = 4;

    buf[off++] = 0x08;
    off += WriteVarint(buf + off, GetID());

    buf[off++] = 0x10;
    off += WriteVarint(buf + off, idTarget);

    buf[off++] = 0x18;
    off += WriteVarint(buf + off, (uint32_t)posTarget.x);

    buf[off++] = 0x20;
    off += WriteVarint(buf + off, (uint32_t)posTarget.y);

    buf[off++] = 0x30;
    off += WriteVarint(buf + off, 21); // magic attack interaction

    buf[off++] = 0x40;
    off += WriteVarint(buf + off, idMagic);

    *(uint16_t*)buf = (uint16_t)off;
    *(uint16_t*)(buf + 2) = 0x03FE;
    SendPacket(buf, off);
}

void CHero::MagicAttack(OBJID idMagic, const Position& posTarget)
{
    MagicAttack(idMagic, 0, posTarget);
}

void CHero::StartMining()
{
    GameCall::CHero_StartMining()(this);
}

void CHero::UseItem(OBJID idItem)
{
    SendMsgItem(idItem, 4);
}

void CHero::DropItem(OBJID idItem, const Position& pos)
{
    SendDropItemPacket(idItem, pos);
}

void CHero::EquipItem(OBJID idItem, int slot)
{
    SendMsgItem(idItem, 4, slot + 1);
}

void CHero::UnequipItem(OBJID idItem, int slot)
{
    SendMsgItem(idItem, 6, slot + 1);
}

void CHero::RepairItem(OBJID idItem)
{
    SendMsgItem(idItem, 14);
}

void CHero::OpenWarehouse(OBJID idNpc)
{
    ActivateNpc(idNpc);
    SendMsgItem(idNpc, 9);
    SendWarehousePacket(idNpc, 1, 10);
}

void CHero::DepositWarehouseItem(OBJID idNpc, OBJID idItem)
{
    SendWarehousePacket(idNpc, 2, 10, idItem);
}

void CHero::DepositWarehouseSilver(OBJID idNpc, uint32_t amount)
{
    if (amount == 0) return;

    // Packet type 0x03F1: field1=npcId, field2=amount, field5=10 (deposit silver)
    {
        uint8_t buf[32] = {};
        int off = 4;
        buf[off++] = 0x08;
        off += WriteVarint(buf + off, idNpc);
        buf[off++] = 0x10;
        off += WriteVarint(buf + off, amount);
        buf[off++] = 0x28;
        off += WriteVarint(buf + off, 10);
        *(uint16_t*)buf = (uint16_t)off;
        *(uint16_t*)(buf + 2) = 0x03F1;
        SendPacket(buf, off);
    }

    // Confirm packet: field1=npcId, field5=9
    {
        uint8_t buf[32] = {};
        int off = 4;
        buf[off++] = 0x08;
        off += WriteVarint(buf + off, idNpc);
        buf[off++] = 0x28;
        off += WriteVarint(buf + off, 9);
        *(uint16_t*)buf = (uint16_t)off;
        *(uint16_t*)(buf + 2) = 0x03F1;
        SendPacket(buf, off);
    }

    spdlog::info("[npc] DepositWarehouseSilver npc={} amount={}", idNpc, amount);
}

void CHero::WithdrawWarehouseItem(OBJID idNpc, OBJID idItem)
{
    SendWarehousePacket(idNpc, 3, 10, idItem);
}

void CHero::OpenTreasureBank(OBJID idNpc)
{
    ActivateNpc(idNpc);
}

void CHero::DepositTreasureBankMeteors(OBJID idNpc)
{
    ActivateNpc(idNpc);
    AnswerNpcEx(0, 101);
}

void CHero::DepositTreasureBankDragonBalls(OBJID idNpc)
{
    ActivateNpc(idNpc);
    AnswerNpcEx(1, 101);
}

void CHero::OpenComposeBank(OBJID idNpc)
{
    ActivateNpc(idNpc);
}

void CHero::DepositComposeBankAll()
{
    SendSimplePacket(0x1B67, 0x08, 2);
}

void CHero::CancelFly()
{
    constexpr uint32_t kMsgActionModeCancelFly = 53;
    uint8_t buf[32] = {};
    int off = 4;

    buf[off++] = 0x08;
    off += WriteVarint(buf + off, kMsgActionModeCancelFly);

    buf[off++] = 0x10;
    off += WriteVarint(buf + off, GetID());

    buf[off++] = 0x18;
    off += WriteVarint(buf + off, GetTickCount());

    *(uint16_t*)buf = (uint16_t)off;
    *(uint16_t*)(buf + 2) = kMsgActionPacketType;
    SendPacket(buf, off);
}

void CHero::Sit()
{
    constexpr uint32_t kMsgActionModeSit = 3;
    uint8_t buf[32] = {};
    int off = 4;

    buf[off++] = 0x08;
    off += WriteVarint(buf + off, kMsgActionModeSit);

    buf[off++] = 0x10;
    off += WriteVarint(buf + off, GetID());

    buf[off++] = 0x18;
    off += WriteVarint(buf + off, GetTickCount());

    buf[off++] = 0x20;
    off += WriteVarint(buf + off, static_cast<uint32_t>(m_posMap.x));

    buf[off++] = 0x28;
    off += WriteVarint(buf + off, static_cast<uint32_t>(m_posMap.y));

    buf[off++] = 0x40;
    off += WriteVarint(buf + off, 250);

    *(uint16_t*)buf = static_cast<uint16_t>(off);
    *(uint16_t*)(buf + 2) = kMsgActionPacketType;
    SendPacket(buf, off);
}

void CHero::ReviveInTown()
{
    uint8_t buf[32] = {};
    int off = 4;

    buf[off++] = 0x08;
    off += WriteVarint(buf + off, kMsgActionModeRevive);

    buf[off++] = 0x10;
    off += WriteVarint(buf + off, GetID());

    buf[off++] = 0x18;
    off += WriteVarint(buf + off, GetTickCount());

    *(uint16_t*)buf = (uint16_t)off;
    *(uint16_t*)(buf + 2) = kMsgActionPacketType;
    SendPacket(buf, off);
}

bool CHero::VipTeleport(OBJID mapId)
{
    const uint32_t destination = GetVipDestinationForMap(mapId);
    if (destination == 0)
        return false;

    uint8_t buf[16] = {};
    int off = 4;

    buf[off++] = 0x08;
    off += WriteVarint(buf + off, kMsgVipModeTeleport);

    buf[off++] = 0x30;
    off += WriteVarint(buf + off, destination);

    *(uint16_t*)buf = (uint16_t)off;
    *(uint16_t*)(buf + 2) = kMsgVipPacketType;
    SendPacket(buf, off);
    return true;
}

void CHero::VipTeleportTwinCity()
{
    VipTeleport(MAP_TWIN_CITY);
}

void CHero::BuyItem(OBJID idNpc, uint32_t typeId)
{
    SendBuyItemPacket(idNpc, typeId);
}

void CHero::SellItem(OBJID idNpc, OBJID idItem)
{
    SendSellItemPacket(idNpc, idItem);
}

void CHero::StartTrade(OBJID idPlayer)
{
    SendTradePacket(kTradeModeStart, idPlayer);
}

void CHero::OfferTradeItem(OBJID idItem)
{
    SendTradePacket(kTradeModeAddItem, idItem);
}

void CHero::AcceptTrade(OBJID idPlayer)
{
    SendTradePacket(kTradeModeAccept, idPlayer);
}

void CHero::CancelTrade(OBJID idPlayer)
{
    SendTradePacket(kTradeModeCancel, idPlayer);
}

// =====================================================================
// NPC interaction — packet-based
// for our binary, so we send raw protobuf packets via SendMsg instead)
//
// Activate NPC packet (type 0x07EF):
//   protobuf field 1 (varint) = NPC entity ID
//   protobuf field 5 (varint) = 1
//
// Answer NPC packet (type 0x07F0):
//   protobuf field 5 (varint) = dialog option index
//   protobuf field 6 (varint) = NPC task/dialog ID
// =====================================================================
void CHero::ActivateNpc(OBJID idNpc)
{
    // [u16 size][u16 type=0x07EF][field1=idNpc][field5=1]
    uint8_t buf[32];
    int off = 4; // skip header, fill size+type later

    // field 1 (tag=0x08), varint = NPC entity ID
    buf[off++] = 0x08;
    off += WriteVarint(buf + off, idNpc);

    // field 5 (tag=0x28), varint = 1
    buf[off++] = 0x28;
    buf[off++] = 0x01;

    // Fill header
    *(uint16_t*)buf = (uint16_t)off;       // size
    *(uint16_t*)(buf + 2) = 0x07EF;        // type

    spdlog::debug("[npc] ActivateNpc entity={}, pkt size={}", idNpc, off);
    SendPacket(buf, off);
}

void CHero::AnswerNpc(int answer)
{
    AnswerNpcEx(answer, 101); // default task ID for Conductress
}

void CHero::AnswerNpcEx(int answer, int taskId)
{
    // [u16 size][u16 type=0x07F0][field5=answer][field6=taskId]
    // Proto3: field5=0 is omitted (default value not serialized)
    uint8_t buf[32];
    int off = 4;

    // field 5 (tag=0x28), varint = answer option (skip if 0)
    if (answer != 0) {
        buf[off++] = 0x28;
        off += WriteVarint(buf + off, (uint32_t)answer);
    }

    // field 6 (tag=0x30), varint = task/dialog ID
    buf[off++] = 0x30;
    off += WriteVarint(buf + off, (uint32_t)taskId);

    // Fill header
    *(uint16_t*)buf = (uint16_t)off;
    *(uint16_t*)(buf + 2) = 0x07F0;

    spdlog::debug("[npc] AnswerNpc option={} taskId={}, pkt size={}", answer, taskId, off);
    SendPacket(buf, off);
}

bool CHero::IsNpcActive() const
{
    return m_bNpcActive != FALSE;
}

OBJID CHero::GetActiveNpc() const
{
    return m_idActiveNpc;
}

int CHero::GetCurrentHp() const
{
    return m_pStatTable ? m_pStatTable->GetValue(1) : 0;
}

int CHero::GetMaxHp() const
{
    if (m_nMaxHp > 0)
        return m_nMaxHp;

    return GameCall::CHero_GetMaxHp()(this);
}

int CHero::GetCurrentMana() const
{
    return GameCall::CHero_GetCurrentMana()(this);
}

int CHero::GetMaxMana() const
{
    if (m_bMaxManaValid)
        return m_nMaxMana;

    return GameCall::CHero_GetMaxMana()(this);
}

void CHero::RefreshSilverCache(bool trusted) const
{
    const OBJID heroId = GetID();
    ResetSilverCacheForHero(heroId);

    const uint64_t rawValue = m_qwRuntimeA30;
    if (rawValue > UINT32_MAX)
        return;

    if (!trusted) {
        const uint64_t nowMs = GetGameTimestampMs();
        if (LooksLikeMovementTimestamp(rawValue, nowMs))
            return;

        const uint32_t candidate = static_cast<uint32_t>(rawValue);
        if (!IsPlausibleUntrustedSilverValue(candidate))
            return;
    }

    StoreSilverCache(heroId, static_cast<uint32_t>(rawValue), trusted);
}

void CHero::SetTrustedSilver(uint32_t value) const
{
    StoreSilverCache(GetID(), value, true);
}

bool CHero::HasTrustedSilverCache() const
{
    ResetSilverCacheForHero(GetID());
    return g_silverCache.hasValue && g_silverCache.isTrusted;
}

uint32_t CHero::GetSilver() const
{
    RefreshSilverCache(false);
    ResetSilverCacheForHero(GetID());
    if (g_silverCache.hasValue)
        return g_silverCache.value;

    const uint64_t rawValue = m_qwRuntimeA30;
    const uint64_t nowMs = GetGameTimestampMs();
    if (rawValue > UINT32_MAX || LooksLikeMovementTimestamp(rawValue, nowMs))
        return 0;

    return static_cast<uint32_t>(rawValue);
}

CMagic* CHero::FindMagicByName(const char* name) const
{
    if (!name || !name[0])
        return nullptr;

    for (const auto& magicRef : m_vecMagic) {
        if (!magicRef)
            continue;
        if (_stricmp(magicRef->GetName(), name) == 0)
            return magicRef.get();
    }

    return nullptr;
}

CMagic* CHero::FindMagicById(OBJID idMagic) const
{
    for (const auto& magicRef : m_vecMagic) {
        if (magicRef && magicRef->GetMagicType() == idMagic)
            return magicRef.get();
    }
    return nullptr;
}

bool CHero::IsVip() const
{
    return m_bVip != FALSE;
}
