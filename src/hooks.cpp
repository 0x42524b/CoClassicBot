#include "hooks.h"
#include "CEntityInfo.h"
#include "config.h"
#include "discord.h"
#include "gfx.h"
#include "game.h"
#include "plugins/plugin_mgr.h"
#include "log.h"
#include <detours.h>
#include <cstring>

// =====================================================================
// RenderEntityVisual hook — thin dispatcher to plugin callbacks
//
// CEntity::RenderVisual
// Called once per visible entity during the visual render phase.
// =====================================================================
static GameCall::CEntity_RenderVisualFn OrigRenderVisual = nullptr;
static GameCall::MsgUpdate_ProcessFn OrigMsgUpdateProcess = nullptr;
static GameCall::TradeWindow_HandleMessageFn OrigTradeWindowHandleMessage = nullptr;
static GameCall::CGameUI_ShowMsgFn OrigShowMsg = nullptr;
static WhisperCallback g_whisperCallback;
static std::vector<LootDropRecord> g_lootDropRecords;
static bool g_hasIncomingTradeRequest = false;
static DWORD g_incomingTradeTick = 0;
static OBJID g_incomingTradeRequesterId = 0;
static char g_incomingTradeRequesterName[32] = "";
static uint64_t g_lastTradeRequestRawState = 0;

namespace {
constexpr int kTradeWindowMessage = 0x464;
constexpr int64_t kTradeEventClose = 0x3ED;
constexpr int64_t kTradeEventHide = 0x3EE;
constexpr int64_t kTradeEventDismiss = 0x3EF;
constexpr int64_t kTradeEventIncomingRequest = 0x3F6;
constexpr uint32_t kMsgUpdateModeMoney = 5;

#pragma pack(push, 1)
struct MsgUpdateEntry
{
    uint8_t _pad00[0x10];
    uint32_t playerId;
    uint32_t mode;
    uint32_t value;
};

struct MsgUpdateEntryList
{
    uint8_t _pad00[0x18];
    int32_t count;
    uint8_t _pad1C[0x4];
    uint8_t* items;
};
#pragma pack(pop)

const MsgUpdateEntry* FindMoneyUpdate(void* msgUpdate, OBJID heroId)
{
    if (!msgUpdate || heroId == 0)
        return nullptr;

    auto* entryList = *reinterpret_cast<MsgUpdateEntryList**>(
        reinterpret_cast<uint8_t*>(msgUpdate) + 0x20);
    if (!entryList || !entryList->items || entryList->count <= 0 || entryList->count > 256)
        return nullptr;

    auto** entries = reinterpret_cast<MsgUpdateEntry**>(entryList->items + 0x8);
    for (int32_t i = 0; i < entryList->count; ++i) {
        const MsgUpdateEntry* entry = entries[i];
        if (!entry)
            continue;
        if (entry->playerId != heroId)
            continue;
        if (entry->mode != kMsgUpdateModeMoney)
            continue;
        return entry;
    }

    return nullptr;
}
}

static void HkRenderEntityVisual(void* entityPtr)
{
    CRole* entity = (CRole*)entityPtr;

    if (!PluginManager::Get().PreRenderEntity(entity))
        return;

    OrigRenderVisual(entityPtr);

    PluginManager::Get().PostRenderEntity(entity);
}

static uint64_t HkTradeWindowHandleMessage(int64_t* param1, int param2, int64_t param3)
{
    if (param2 == kTradeWindowMessage) {
        if (CEntityInfo* entityInfo = Game::GetEntityInfo())
            g_lastTradeRequestRawState = entityInfo->m_nTradeRequestState;

        if (param3 == kTradeEventIncomingRequest) {
            if (CEntityInfo* entityInfo = Game::GetEntityInfo()) {
                g_hasIncomingTradeRequest = true;
                g_incomingTradeTick = GetTickCount();
                g_incomingTradeRequesterId = entityInfo->GetTradeRequesterId();
                strncpy_s(g_incomingTradeRequesterName,
                    entityInfo->GetTradeRequesterName(),
                    _TRUNCATE);
                spdlog::info("[hooks] Incoming trade request from {} (ID={})",
                    g_incomingTradeRequesterName, g_incomingTradeRequesterId);
            }
        } else if (param3 == kTradeEventClose
                || param3 == kTradeEventHide
                || param3 == kTradeEventDismiss) {
            spdlog::debug("[hooks] Trade event close/hide/dismiss (param3=0x{:X})", param3);
            ConsumeIncomingTradeRequest();
        }
    }

    return OrigTradeWindowHandleMessage(param1, param2, param3);
}

static uint8_t HkMsgUpdateProcess(void* msgUpdate)
{
    CHero* heroBefore = Game::GetHero();
    const OBJID heroIdBefore = heroBefore ? heroBefore->GetID() : 0;
    const MsgUpdateEntry* moneyUpdate = FindMoneyUpdate(msgUpdate, heroIdBefore);
    const uint64_t silverBefore = heroBefore ? heroBefore->GetSilverRuntimeValue() : 0;

    const uint8_t result = OrigMsgUpdateProcess(msgUpdate);

    CHero* heroAfter = Game::GetHero();
    if (moneyUpdate && heroAfter && heroAfter->GetID() == heroIdBefore) {
        heroAfter->SetTrustedSilver(moneyUpdate->value);
        return result;
    }

    if (heroBefore && heroAfter
        && heroAfter->GetID() == heroIdBefore
        && heroAfter->GetSilverRuntimeValue() != silverBefore) {
        heroAfter->RefreshSilverCache(true);
    }

    return result;
}

static uint64_t HkShowMsg(
    void* gameUI,
    const std::string* sender,
    const std::string* receiver,
    const std::string* suffix,
    const std::string* message,
    uint32_t color,
    uint16_t style,
    uint16_t channel,
    uint32_t timestamp,
    uint32_t extra)
{
    constexpr uint16_t kChannelWhisper = 0x7D1;
    constexpr uint16_t kChannelSystemLoot = 0x7E1;

    // Log all chat messages for channel discovery
    if (message && !message->empty()) {
        spdlog::info("[chat] ch=0x{:X} style={} color=0x{:08X} sender='{}' msg='{}'",
            channel, style, color,
            sender ? *sender : "",
            *message);
    }

    // Parse system loot messages: "A <item>(+<plus>) has dropped at <x>,<y>"
    if (channel == kChannelSystemLoot && message && !message->empty()) {
        // Find "has dropped at " then parse x,y
        const char* kDropMarker = "has dropped at ";
        auto dropPos = message->find(kDropMarker);
        if (dropPos != std::string::npos) {
            const char* coordStr = message->c_str() + dropPos + strlen(kDropMarker);
            int x = 0, y = 0;
            if (sscanf(coordStr, "%d,%d", &x, &y) == 2
                || sscanf(coordStr, "%d, %d", &x, &y) == 2) {
                g_lootDropRecords.push_back({ Position{x, y}, GetTickCount() });
                spdlog::info("[loot-drop] Recorded our drop at ({},{}) msg='{}'", x, y, *message);
                if (GetMiscSettings().lootDropNotifyEnabled) {
                    CHero* notifHero = Game::GetHero();
                    const char* heroName = notifHero ? notifHero->GetName() : "Unknown";
                    std::string notif = std::string("[") + heroName + "] " + *message;
                    SendDiscordNotification(notif, false);
                }
            }
        }
    }

    if (channel == kChannelWhisper && sender && message && g_whisperCallback) {
        // Only notify for whispers from other players (not from ourselves)
        CHero* hero = Game::GetHero();
        if (!hero || *sender != hero->GetName()) {
            g_whisperCallback(*sender, *message);
        }
    }

    return OrigShowMsg(gameUI, sender, receiver, suffix, message,
                       color, style, channel, timestamp, extra);
}

void SetWhisperCallback(WhisperCallback cb)
{
    g_whisperCallback = std::move(cb);
}

const std::vector<LootDropRecord>& GetLootDropRecords()
{
    return g_lootDropRecords;
}

void PruneLootDropRecords(DWORD maxAgeMs)
{
    const DWORD now = GetTickCount();
    std::erase_if(g_lootDropRecords, [now, maxAgeMs](const LootDropRecord& r) {
        return (now - r.tick) > maxAgeMs;
    });
}

// =====================================================================
// Init / Cleanup
// =====================================================================
void InitHooks()
{
    Gfx::Init(Game::Base());

    OrigRenderVisual = GameCall::CEntity_RenderVisual();
    uintptr_t addr = reinterpret_cast<uintptr_t>(OrigRenderVisual);
    OrigMsgUpdateProcess = GameCall::MsgUpdate_Process();
    uintptr_t msgUpdateAddr = reinterpret_cast<uintptr_t>(OrigMsgUpdateProcess);
    OrigTradeWindowHandleMessage = GameCall::TradeWindow_HandleMessage();
    uintptr_t tradeAddr = reinterpret_cast<uintptr_t>(OrigTradeWindowHandleMessage);
    OrigShowMsg = GameCall::CGameUI_ShowMsg();
    uintptr_t showMsgAddr = reinterpret_cast<uintptr_t>(OrigShowMsg);

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)OrigRenderVisual, HkRenderEntityVisual);
    DetourAttach(&(PVOID&)OrigMsgUpdateProcess, HkMsgUpdateProcess);
    DetourAttach(&(PVOID&)OrigTradeWindowHandleMessage, HkTradeWindowHandleMessage);
    DetourAttach(&(PVOID&)OrigShowMsg, HkShowMsg);
    LONG err = DetourTransactionCommit();

    spdlog::info("[hooks] RenderEntityVisual @ 0x{:X}: {}", addr, err == NO_ERROR ? "OK" : "FAILED");
    spdlog::info("[hooks] MsgUpdate::Process @ 0x{:X}: {}", msgUpdateAddr, err == NO_ERROR ? "OK" : "FAILED");
    spdlog::info("[hooks] TradeWindow_HandleMessage @ 0x{:X}: {}", tradeAddr, err == NO_ERROR ? "OK" : "FAILED");
    spdlog::info("[hooks] CGameUI::ShowMsg @ 0x{:X}: {}", showMsgAddr, err == NO_ERROR ? "OK" : "FAILED");
}

void CleanupHooks()
{
    if (!OrigRenderVisual && !OrigMsgUpdateProcess && !OrigTradeWindowHandleMessage && !OrigShowMsg) return;

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    if (OrigRenderVisual)
        DetourDetach(&(PVOID&)OrigRenderVisual, HkRenderEntityVisual);
    if (OrigMsgUpdateProcess)
        DetourDetach(&(PVOID&)OrigMsgUpdateProcess, HkMsgUpdateProcess);
    if (OrigTradeWindowHandleMessage)
        DetourDetach(&(PVOID&)OrigTradeWindowHandleMessage, HkTradeWindowHandleMessage);
    if (OrigShowMsg)
        DetourDetach(&(PVOID&)OrigShowMsg, HkShowMsg);
    DetourTransactionCommit();

    OrigRenderVisual = nullptr;
    OrigMsgUpdateProcess = nullptr;
    OrigTradeWindowHandleMessage = nullptr;
    OrigShowMsg = nullptr;
    spdlog::info("[hooks] Hooks removed");
}

bool HasIncomingTradeRequest(DWORD maxAgeMs)
{
    return g_hasIncomingTradeRequest
        && g_incomingTradeTick != 0
        && GetTickCount() - g_incomingTradeTick < maxAgeMs;
}

OBJID GetIncomingTradeRequesterId()
{
    return g_incomingTradeRequesterId;
}

const char* GetIncomingTradeRequesterName()
{
    return g_incomingTradeRequesterName;
}

uint64_t GetLastTradeRequestRawState()
{
    return g_lastTradeRequestRawState;
}

void ConsumeIncomingTradeRequest()
{
    g_hasIncomingTradeRequest = false;
    g_incomingTradeTick = 0;
    g_incomingTradeRequesterId = 0;
    g_incomingTradeRequesterName[0] = '\0';
}
