#pragma once
#include "CHero.h"
#include "CGameMap.h"
#include "log.h"
#include <string>

class CStatTable;
class CEntitySet;
class CEntityInfo;

// =====================================================================
// Offsets — all RVAs relative to ImConquer.exe base (0x140000000)
// Verified in Ghidra on the 64-bit Scylla dump.
// =====================================================================
namespace Offsets {
    constexpr uintptr_t ROLE_MGR  = 0x4DF588;
    constexpr uintptr_t ENTITY_INFO = 0x4DF590;
    constexpr uintptr_t ENTITY_SET = 0x4DF5F0;
    constexpr uintptr_t GAME_MAP  = 0x4E02E0;
}

namespace GameRva {
    constexpr uintptr_t CENTITY_RENDER_VISUAL = 0x1AFD20;
    constexpr uintptr_t CNETCLIENT_GET_INSTANCE = 0x0B9490;
    constexpr uintptr_t CNETCLIENT_SEND_MSG   = 0x18EEA0;
    constexpr uintptr_t CHERO_GET_MAX_HP       = 0x179980;
    constexpr uintptr_t CHERO_GET_MAX_MANA     = 0x179B90;
    constexpr uintptr_t CSTATTABLE_GET_VALUE   = 0x1F1490;
    constexpr uintptr_t CHERO_GET_CURRENT_MANA = 0x1A58F0;
    constexpr uintptr_t CHERO_FATAL_STRIKE     = 0x2297B0;
    constexpr uintptr_t CHERO_WALK             = 0x229DF0;
    constexpr uintptr_t CHERO_JUMP             = 0x22A0B0;
    constexpr uintptr_t CHERO_START_MINING     = 0x18BB10;
    constexpr uintptr_t MSGUPDATE_PROCESS      = 0x1E0870;
    constexpr uintptr_t TRADEWINDOW_HANDLE_MESSAGE = 0x10B1F0;
    constexpr uintptr_t CGAMEUI_SHOW_MSG           = 0x191960;
}

namespace GameVtableIndex {
    constexpr size_t CRole_SetCommand = 59;
}

// =====================================================================
// Game — static accessor for game data
// =====================================================================
class Game
{
public:
    static void Init() {
        g_qwModuleBase = (ULONG64)GetModuleHandleA(nullptr);
        spdlog::info("[game] Base: 0x{:X}", (uintptr_t)g_qwModuleBase);
    }

    static uintptr_t Base() { return (uintptr_t)g_qwModuleBase; }

    template <typename T>
    static T Resolve(uintptr_t rva) {
        return reinterpret_cast<T>(Base() + rva);
    }

    static CRoleMgr* GetRoleMgr() {
        if (!g_qwModuleBase) return nullptr;
        return Resolve<CRoleMgr*>(Offsets::ROLE_MGR);
    }

    static CGameMap* GetMap() {
        if (!g_qwModuleBase) return nullptr;
        return Resolve<CGameMap*>(Offsets::GAME_MAP);
    }

    static CEntitySet* GetEntitySet() {
        if (!g_qwModuleBase) return nullptr;
        return Resolve<CEntitySet*>(Offsets::ENTITY_SET);
    }

    static CEntityInfo* GetEntityInfo() {
        if (!g_qwModuleBase) return nullptr;
        return *Resolve<CEntityInfo**>(Offsets::ENTITY_INFO);
    }

    static CHero* GetHero() {
        return CHero::GetSingletonPtr();
    }
};

namespace GameCall {
    using CEntity_RenderVisualFn = void(*)(void*);
    using CNetClient_GetInstanceFn = void*(*)();
    using CNetClient_SendMsgFn = uint32_t(*)(int64_t, const uint8_t*, int64_t);
    using CRole_SetCommandFn = void(*)(CRole*, CCommand*);
    using CStatTable_GetValueFn = int(*)(const CStatTable*, int);
    using CHero_GetCurrentManaFn = int(*)(const CHero*);
    using CHero_GetMaxHpFn = int(*)(const CHero*);
    using CHero_GetMaxManaFn = int(*)(const CHero*);
    using CHero_FatalStrikeFn = void(*)(CHero*, OBJID);
    using CHero_WalkFn = void(*)(CHero*, int, int);
    using CHero_JumpFn = void(*)(CHero*, int, int);
    using CHero_StartMiningFn = void(*)(CHero*);
    using MsgUpdate_ProcessFn = uint8_t(*)(void*);
    using TradeWindow_HandleMessageFn = uint64_t(*)(int64_t*, int, int64_t);
    // CGameUI::ShowMsg — adds a chat message to the display.
    // sender/receiver/suffix/message are std::string* (MSVC ABI).
    using CGameUI_ShowMsgFn = uint64_t(*)(
        void* gameUI,             // CGameUI* this
        const std::string* sender,
        const std::string* receiver,
        const std::string* suffix,
        const std::string* message,
        uint32_t color,           // ARGB
        uint16_t style,
        uint16_t channel,         // 0x7D1 = whisper
        uint32_t timestamp,
        uint32_t extra
    );

    inline CEntity_RenderVisualFn CEntity_RenderVisual() {
        static auto fn = Game::Resolve<CEntity_RenderVisualFn>(GameRva::CENTITY_RENDER_VISUAL);
        return fn;
    }

    inline CNetClient_GetInstanceFn CNetClient_GetInstance() {
        static auto fn = Game::Resolve<CNetClient_GetInstanceFn>(GameRva::CNETCLIENT_GET_INSTANCE);
        return fn;
    }

    inline CNetClient_SendMsgFn CNetClient_SendMsg() {
        static auto fn = Game::Resolve<CNetClient_SendMsgFn>(GameRva::CNETCLIENT_SEND_MSG);
        return fn;
    }

    inline CStatTable_GetValueFn CStatTable_GetValue() {
        static auto fn = Game::Resolve<CStatTable_GetValueFn>(GameRva::CSTATTABLE_GET_VALUE);
        return fn;
    }

    inline CHero_GetMaxHpFn CHero_GetMaxHp() {
        static auto fn = Game::Resolve<CHero_GetMaxHpFn>(GameRva::CHERO_GET_MAX_HP);
        return fn;
    }

    inline CHero_GetCurrentManaFn CHero_GetCurrentMana() {
        static auto fn = Game::Resolve<CHero_GetCurrentManaFn>(GameRva::CHERO_GET_CURRENT_MANA);
        return fn;
    }

    inline CHero_GetMaxManaFn CHero_GetMaxMana() {
        static auto fn = Game::Resolve<CHero_GetMaxManaFn>(GameRva::CHERO_GET_MAX_MANA);
        return fn;
    }

    inline CHero_FatalStrikeFn CHero_FatalStrike() {
        static auto fn = Game::Resolve<CHero_FatalStrikeFn>(GameRva::CHERO_FATAL_STRIKE);
        return fn;
    }

    inline CHero_WalkFn CHero_Walk() {
        static auto fn = Game::Resolve<CHero_WalkFn>(GameRva::CHERO_WALK);
        return fn;
    }

    inline CHero_JumpFn CHero_Jump() {
        static auto fn = Game::Resolve<CHero_JumpFn>(GameRva::CHERO_JUMP);
        return fn;
    }

    inline CHero_StartMiningFn CHero_StartMining() {
        static auto fn = Game::Resolve<CHero_StartMiningFn>(GameRva::CHERO_START_MINING);
        return fn;
    }

    inline MsgUpdate_ProcessFn MsgUpdate_Process() {
        static auto fn = Game::Resolve<MsgUpdate_ProcessFn>(GameRva::MSGUPDATE_PROCESS);
        return fn;
    }

    inline TradeWindow_HandleMessageFn TradeWindow_HandleMessage() {
        static auto fn = Game::Resolve<TradeWindow_HandleMessageFn>(GameRva::TRADEWINDOW_HANDLE_MESSAGE);
        return fn;
    }

    inline CGameUI_ShowMsgFn CGameUI_ShowMsg() {
        static auto fn = Game::Resolve<CGameUI_ShowMsgFn>(GameRva::CGAMEUI_SHOW_MSG);
        return fn;
    }
}

// Returns true if a name appears in a comma/semicolon-separated whitelist (case-insensitive).
inline bool IsNameWhitelisted(const char* name, const char* whitelist)
{
    if (!name || !name[0] || !whitelist || !whitelist[0])
        return false;
    const char* p = whitelist;
    while (*p) {
        // skip delimiters/whitespace
        while (*p == ',' || *p == ';' || *p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
            ++p;
        if (!*p) break;
        const char* start = p;
        while (*p && *p != ',' && *p != ';' && *p != '\n' && *p != '\r' && *p != '\t')
            ++p;
        // trim trailing spaces
        const char* end = p;
        while (end > start && *(end - 1) == ' ')
            --end;
        size_t len = end - start;
        if (len > 0 && _strnicmp(start, name, len) == 0 && name[len] == '\0')
            return true;
    }
    return false;
}

// Returns true if any non-whitelisted player (other than heroId) is on the entity list.
inline bool AreOtherPlayersNearby(OBJID heroId, const char* whitelist = nullptr)
{
    CRoleMgr* mgr = Game::GetRoleMgr();
    if (!mgr || mgr->m_deqRole.empty() || mgr->m_deqRole.size() >= 10000)
        return false;

    for (size_t i = 0; i < mgr->m_deqRole.size() && i < 500; ++i) {
        const auto& roleRef = mgr->m_deqRole[i];
        if (!roleRef)
            continue;
        const CRole* role = roleRef.get();
        if (!role->IsPlayer() || role->GetID() == heroId)
            continue;
        if (whitelist && IsNameWhitelisted(role->GetName(), whitelist))
            continue;
        return true;
    }
    return false;
}

// Convenience wrapper — resolves hero ID automatically.
inline bool ArePlayersNearby(const char* whitelist = nullptr)
{
    const CHero* hero = Game::GetHero();
    return AreOtherPlayersNearby(hero ? hero->GetID() : 0, whitelist);
}
