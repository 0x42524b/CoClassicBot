#include "packets.h"
#include "config.h"
#include "game.h"
#include "log.h"
#include <detours.h>

static PacketLog g_packetLog;
PacketLog& GetPacketLog() { return g_packetLog; }
static DWORD g_lastVipTeleportTick = 0;
DWORD GetLastVipTeleportTick() { return g_lastVipTeleportTick; }
bool IsVipTeleportOnCooldown(DWORD cooldownMs)
{
    return g_lastVipTeleportTick != 0 && GetTickCount() - g_lastVipTeleportTick < cooldownMs;
}

// Captured CNetClient pointer from first SendMsg call
static int64_t g_netClient = 0;

// =====================================================================
// CNetClient::SendMsg hook
//
// Signature: uint32_t SendMsg(int64_t client, const uint8_t* data, int64_t size)
// Packet format: [u16 size][u16 type][payload...]
// =====================================================================
static GameCall::CNetClient_SendMsgFn OrigSendMsg = nullptr;

namespace {
constexpr uint16_t kMsgActionPacketType = 0x03F2;
constexpr uint32_t kMsgActionModeJump = 19;
using GetTimestampFn = long long*(*)(long long*);

struct MsgActionPacket
{
    uint32_t mode = 0;
    uint32_t id = 0;
    uint32_t timestamp = 0;
    uint32_t data1 = 0;
    uint32_t data2 = 0;
    uint32_t facing = 0;
    uint32_t tag7 = 0;
    uint32_t tag8 = 0;
    uint32_t data3 = 0;
    uint32_t data4 = 0;
};

uint32_t g_jumpSpeedTimer = 0;
static DWORD g_lastSpeedHackJumpTick = 0;
constexpr DWORD kSpeedTimerResetGapMs = 1000;

uint64_t GetGameTimestampMs()
{
    static auto fn = Game::Resolve<GetTimestampFn>(0x0C6A80);
    if (!fn)
        return 0;

    long long timestampNs = 0;
    fn(&timestampNs);
    return static_cast<uint64_t>(timestampNs / 1000000);
}

static int WriteVarint(uint8_t* buf, uint32_t value)
{
    int n = 0;
    while (value > 0x7F) {
        buf[n++] = static_cast<uint8_t>(value & 0x7F) | 0x80;
        value >>= 7;
    }
    buf[n++] = static_cast<uint8_t>(value);
    return n;
}

static bool ReadVarint(const uint8_t* data, size_t size, size_t& off, uint32_t& value)
{
    value = 0;
    int shift = 0;
    while (off < size && shift <= 28) {
        const uint8_t byte = data[off++];
        value |= static_cast<uint32_t>(byte & 0x7F) << shift;
        if ((byte & 0x80) == 0)
            return true;
        shift += 7;
    }
    return false;
}

} // end anonymous namespace

static void ApplyLocalJumpPrediction(const MsgActionPacket& packet)
{
    CHero* hero = Game::GetHero();
    if (!hero) {
        spdlog::warn("[prediction] No hero");
        return;
    }
    if (hero->GetID() != packet.id) {
        spdlog::warn("[prediction] ID mismatch: hero={} packet={}", hero->GetID(), packet.id);
        return;
    }

    const Position origin = hero->m_posMap;
    const Position destination(static_cast<int>(packet.data3), static_cast<int>(packet.data4));

    spdlog::debug("[prediction] ({},{}) -> ({},{}) ts={} timer={}",
        origin.x, origin.y, destination.x, destination.y,
        packet.timestamp, g_jumpSpeedTimer);

    hero->m_cmdAction.iType = _COMMAND_JUMP;
    hero->m_cmdAction.iStatus = _CMDSTATUS_ACCOMPLISH;
    hero->m_cmdAction.posTarget = destination;
    hero->m_cmdAction.nDir = static_cast<int>(packet.facing);
    hero->m_cmdAction.dwTimestamp = packet.timestamp;
    hero->m_cmdAction.bDiagnal = origin.DistanceTo(destination) > 18.0f ? TRUE : FALSE;

    hero->m_posMap = destination;

    if (CGameMap* map = Game::GetMap()) {
        const int worldX = (destination.x - destination.y) * 32 + map->m_posCameraPos.x;
        const int worldY = (destination.x + destination.y) * 16 + map->m_posCameraPos.y;
        hero->m_posWorld = Position(worldX, worldY);
        hero->m_posScr = Position(worldX - map->m_posViewport.x, worldY - map->m_posViewport.y);
    } else {
        spdlog::warn("[prediction] No map for world coord update");
    }

    const uint64_t nowMs = GetGameTimestampMs();
    hero->m_qwRuntimeA30 = nowMs > 5000 ? (nowMs - 5000) : 0;
    
}

static size_t BuildMsgActionPacket(const MsgActionPacket& packet, uint8_t* buf, size_t capacity)
{
    if (!buf || capacity < 32)
        return 0;

    int off = 4;
    buf[off++] = 0x08;
    off += WriteVarint(buf + off, packet.mode);
    buf[off++] = 0x10;
    off += WriteVarint(buf + off, packet.id);
    buf[off++] = 0x18;
    off += WriteVarint(buf + off, packet.timestamp);
    buf[off++] = 0x20;
    off += WriteVarint(buf + off, packet.data1);
    buf[off++] = 0x28;
    off += WriteVarint(buf + off, packet.data2);
    buf[off++] = 0x30;
    off += WriteVarint(buf + off, packet.facing);
    if (packet.tag7 != 0) {
        buf[off++] = 0x38;
        off += WriteVarint(buf + off, packet.tag7);
    }
    if (packet.tag8 != 0) {
        buf[off++] = 0x40;
        off += WriteVarint(buf + off, packet.tag8);
    }
    buf[off++] = 0x50;
    off += WriteVarint(buf + off, packet.data3);
    buf[off++] = 0x58;
    off += WriteVarint(buf + off, packet.data4);

    *(uint16_t*)buf = static_cast<uint16_t>(off);
    *(uint16_t*)(buf + 2) = kMsgActionPacketType;
    return static_cast<size_t>(off);
}

static void TrackOutgoingPacket(const uint8_t* data, size_t size)
{
    if (!data || size < 4)
        return;

    const uint16_t msgType = *(const uint16_t*)(data + 2);
    if (msgType == 0x1B5C) {
        g_lastVipTeleportTick = GetTickCount();
        spdlog::debug("[packets] VIP teleport packet sent");
    }

    if (g_packetLog.enabled) {
        PacketEntry entry;
        entry.tick    = GetTickCount();
        entry.msgSize = *(const uint16_t*)(data);
        entry.msgType = msgType;
        entry.rawSize = (uint16_t)size;
        entry.data.assign(data, data + size);
        g_packetLog.Push(entry);
    }
}

static uint32_t HkSendMsg(int64_t client, const uint8_t* data, int64_t size)
{
    // Capture the CNetClient pointer on first call
    if (!g_netClient && client)
        g_netClient = client;
    TrackOutgoingPacket(data, (size_t)size);

    return OrigSendMsg(client, data, size);
}

// =====================================================================
// Init / Cleanup
// =====================================================================
void InitPacketHook()
{
    OrigSendMsg = GameCall::CNetClient_SendMsg();
    uintptr_t addr = reinterpret_cast<uintptr_t>(OrigSendMsg);

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)OrigSendMsg, HkSendMsg);
    LONG err = DetourTransactionCommit();

    spdlog::info("[packets] SendMsg @ 0x{:X}: {}", addr, err == NO_ERROR ? "OK" : "FAILED");
}

void CleanupPacketHook()
{
    if (!OrigSendMsg) return;

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourDetach(&(PVOID&)OrigSendMsg, HkSendMsg);
    DetourTransactionCommit();

    OrigSendMsg = nullptr;
    spdlog::info("[packets] Hook removed");
}

bool SendPacket(const uint8_t* data, size_t size)
{
    if (!OrigSendMsg)
        return false;

    if (!g_netClient) {
        if (auto getNetClient = GameCall::CNetClient_GetInstance())
            g_netClient = reinterpret_cast<int64_t>(getNetClient());
    }

    if (!g_netClient)
        return false;

    TrackOutgoingPacket(data, size);
    OrigSendMsg(g_netClient, data, (int64_t)size);
    return true;
}

bool SendJumpPacket(OBJID heroId, int destX, int destY, int facing)
{
    const uint32_t now = GetTickCount();
    if (now - g_lastSpeedHackJumpTick >= kSpeedTimerResetGapMs)
        g_jumpSpeedTimer = 0;

    MsgActionPacket packet = {};
    packet.mode = kMsgActionModeJump;
    packet.id = heroId;
    packet.timestamp = now + g_jumpSpeedTimer;
    packet.facing = static_cast<uint32_t>(facing);
    packet.data3 = static_cast<uint32_t>(destX);
    packet.data4 = static_cast<uint32_t>(destY);

    g_jumpSpeedTimer += 5000;
    g_lastSpeedHackJumpTick = now;

    uint8_t buf[64] = {};
    const size_t sz = BuildMsgActionPacket(packet, buf, sizeof(buf));
    if (sz == 0)
    {
        spdlog::error("Failed to build packet");
        return false;
    }
    
    if (!SendPacket(buf, sz))
    {
        spdlog::error("Failed to send packet");
        return false;
    }
    
    ApplyLocalJumpPrediction(packet);
    return true;
}
