#pragma once
#include "base.h"
#include <array>
#include <vector>
#include <cstring>

// =====================================================================
// PacketEntry — single captured packet
// =====================================================================
struct PacketEntry
{
    DWORD    tick     = 0;        // GetTickCount() at capture time
    uint16_t msgType  = 0;        // packet type from header bytes [2..3]
    uint16_t msgSize  = 0;        // packet size from header bytes [0..1]
    uint16_t rawSize  = 0;        // actual buffer size passed to SendMsg
    std::vector<uint8_t> data;    // full raw packet data
};

// =====================================================================
// PacketLog — ring buffer of recent packets
// =====================================================================
class PacketLog
{
public:
    static constexpr size_t CAPACITY = 200;

    void Push(const PacketEntry& entry) {
        m_buffer[m_head] = entry;
        m_head = (m_head + 1) % CAPACITY;
        if (m_count < CAPACITY) m_count++;
    }

    void Clear() {
        m_count = 0;
        m_head  = 0;
    }

    size_t Count() const { return m_count; }

    // Index 0 = newest, Count()-1 = oldest
    const PacketEntry& Get(size_t i) const {
        size_t idx = (m_head + CAPACITY - 1 - i) % CAPACITY;
        return m_buffer[idx];
    }

    bool enabled = true;

private:
    std::array<PacketEntry, CAPACITY> m_buffer{};
    size_t m_head  = 0;
    size_t m_count = 0;
};

PacketLog& GetPacketLog();
DWORD GetLastVipTeleportTick();
bool IsVipTeleportOnCooldown(DWORD cooldownMs = 60000);

void InitPacketHook();
void CleanupPacketHook();

// Send a raw packet through the game's CNetClient::SendMsg
// Returns false if the client pointer hasn't been captured yet.
bool SendPacket(const uint8_t* data, size_t size);

// Build and send a raw MsgAction jump packet with speed hack applied.
// Updates local position via ApplyLocalJumpPrediction.
bool SendJumpPacket(OBJID heroId, int destX, int destY, int facing);
