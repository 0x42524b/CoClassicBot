#pragma once
#include "CMapObj.h"

// =====================================================================
// User status flags
// =====================================================================
const __int64 USERSTATUS_NORMAL       = 0ll;
const __int64 USERSTATUS_CRIME        = 1ll << 0;
const __int64 USERSTATUS_POISON       = 1ll << 1;
const __int64 USERSTATUS_INVISIBLE    = 1ll << 2;
const __int64 USERSTATUS_DIE          = 1ll << 3;
const __int64 USERSTATUS_XPFULL       = 1ll << 4;
const __int64 USERSTATUS_DEAD         = 1ll << 5;
const __int64 USERSTATUS_TEAMLEADER   = 1ll << 6;
const __int64 USERSTATUS_ATKSPEED     = 1ll << 7;
const __int64 USERSTATUS_SHIELD       = 1ll << 8;
const __int64 USERSTATUS_ATKPOWER     = 1ll << 9;
const __int64 USERSTATUS_GHOST        = 1ll << 10;
const __int64 USERSTATUS_DISAPPEARING = 1ll << 11;
const __int64 USERSTATUS_MAGICDEF     = 1ll << 12;
const __int64 USERSTATUS_BOWDEF       = 1ll << 13;
const __int64 USERSTATUS_RED          = 1ll << 14;
const __int64 USERSTATUS_BLACK        = 1ll << 15;
const __int64 USERSTATUS_ATKRANGE     = 1ll << 16;
const __int64 USERSTATUS_REFLECT      = 1ll << 17;
const __int64 USERSTATUS_SUPERMAN     = 1ll << 18;
const __int64 USERSTATUS_BODYSHIELD   = 1ll << 19;
const __int64 USERSTATUS_MAGICDMG     = 1ll << 20;
const __int64 USERSTATUS_ATKSPEEDEX   = 1ll << 21;
const __int64 USERSTATUS_INVISIBILTY  = 1ll << 22;
const __int64 USERSTATUS_CYCLONE      = 1ll << 23;
const __int64 USERSTATUS_SYNCRIME     = 1ll << 24;
const __int64 USERSTATUS_REFLECTMAGIC = 1ll << 25;
const __int64 USERSTATUS_DODGE        = 1ll << 26;
const __int64 USERSTATUS_FLY          = 1ll << 27;
const __int64 USERSTATUS_CHARGEUP     = 1ll << 28;
const __int64 USERSTATUS_FROZEN       = 1ll << 29;
const __int64 USERSTATUS_PRAY         = 1ll << 30;
const __int64 USERSTATUS_FOLLOWPRAY   = 1ll << 31;
const __int64 USERSTATUS_CURSE        = 1ll << 32;
const __int64 USERSTATUS_BLESS        = 1ll << 33;

// =====================================================================
// Command types
// =====================================================================
const int _COMMAND_NULL      = 0;
const int _COMMAND_PICKUP    = 1;
const int _COMMAND_STOP      = 1;
const int _COMMAND_STANDBY   = 2;
const int _COMMAND_WALK      = 3;
const int _COMMAND_RUN       = 4;
const int _COMMAND_EMOTION   = 6;
const int _COMMAND_ACTION    = 7;
const int _COMMAND_FOLLOW    = 9;
const int _COMMAND_SHITHAPPEN = 10;
const int _COMMAND_DIE       = 11;
const int _COMMAND_SPATTACK  = 12;
const int _COMMAND_FAINT     = 13;
const int _COMMAND_WOUND     = 15;
const int _COMMAND_JUMP      = 16;
const int _COMMAND_DEFEND    = 16;
const int _COMMAND_WALKFORWARD = 17;
const int _COMMAND_RUNFORWARD  = 18;
const int _COMMAND_ATTACK    = 20;
const int _COMMAND_SHOOT     = 21;
const int _COMMAND_RUSHATK   = 23;
const int _COMMAND_INTONE    = 23;
const int _COMMAND_LOCKATK   = 24;
const int _COMMAND_MINE      = 25;
const int _COMMAND_DASH      = 25;

// =====================================================================
// Command status
// =====================================================================
const int _CMDSTATUS_BEGIN      = 0;
const int _CMDSTATUS_DEPART     = 1;
const int _CMDSTATUS_PROGRESS   = 2;
const int _CMDSTATUS_CONTINUE   = 3;
const int _CMDSTATUS_WAITING    = 4;
const int _CMDSTATUS_ACCOMPLISH = 6;

// =====================================================================
// CCommand — action command
// =====================================================================
class CCommand {
public:
    CCommand() {}
    int iType;
    int iStatus;

    union {
        char szCmd[256];

        struct {
            OBJID  idTarget;
            Position posTarget;
            int    nDir;
            int    nArg;
            DWORD  dwData;
            int    nUnknown;
            BOOL   bData;
            int    nData;
            BOOL   bHeroSendMsg;
            int    nFrameStart;
            int    nFrameEnd;
            DWORD  dwTimestamp;
            int    nFrameInterval;
            BOOL   bDiagnal;
            BOOL   bAddUp;
            DWORD  dwIndex;
        };
    };
};

// =====================================================================
// CRole — base entity class
//
// Hierarchy: CMapObj -> CRole -> CHero
//
// All offsets verified via Ghidra + Cheat Engine on the 64-bit client.
// =====================================================================
#pragma pack(push, 1)
class CRole : public CMapObj
{
public:
    virtual ~CRole() {}

private:
    BYTE _pad20[0x10];            // +0x20

public:
    __int64 m_nStatusFlag;         // +0x30

private:
    BYTE _pad38[0x30];            // +0x38  (deque<PAniEffect> + unknowns)

public:
    // ── CRoleInfo fields ──
    OBJID m_id;                    // +0x68  entity ID
private:
    BYTE _pad6C[0x28];            // +0x6C
public:
    char m_szName[16];             // +0x94  null-terminated ASCII
private:
    BYTE _padA4[0x34];            // +0xA4
public:
    Position m_posMap;               // +0xD8  map cell position
    Position m_posWorld;             // +0xE0  world position
    Position m_posScr;               // +0xE8  screen position

private:
    BYTE _padF0[0x188 - 0xF0];     // +0xF0
public:
    CCommand m_cmdAction;           // +0x188  current action command
private:
    BYTE _pad290[0x3D0 - 0x290];    // +0x290
public:
    int   m_nMaxHp;                // +0x3D0 cached max HP
private:
    BYTE _pad3D4[0x6E0 - 0x3D4];  // +0x3D4
public:
    int   m_nStamina;              // +0x6E0 current stamina (PP)
    int   m_nMaxStamina;           // +0x6E4 max stamina
private:
    BYTE _pad6E8[0x714 - 0x6E8];  // +0x6E8
public:
    OBJID m_idSyndicate;           // +0x714  syndicate/guild ID (0 = none)
    int   m_nSyndicateRank;        // +0x718  syndicate rank (100=Leader, 90=Deputy, 50=Member)

    // ── Helpers ──
    OBJID GetID() const { return m_id; }
    const char* GetName() const { return m_szName; }
    BOOL TestState(__int64 nState) const { return (m_nStatusFlag & nState) != 0; }

    BOOL IsDead() const { return (m_nStatusFlag & USERSTATUS_DEAD) != 0; }
    BOOL IsPlayer() const { return m_id >= 1000000; }
    BOOL IsMonster() const { return m_id >= 400000 && m_id < 500000 && !IsGuard() && !IsPatrol(); }
    BOOL HasXpSkillReady() const { return TestState(USERSTATUS_XPFULL); }
    BOOL IsCycloneActive() const { return TestState(USERSTATUS_CYCLONE); }
    BOOL IsFlyActive() const { return TestState(USERSTATUS_FLY); }
    BOOL IsStigmaActive() const { return TestState(USERSTATUS_ATKPOWER); }
    BOOL IsSupermanActive() const { return TestState(USERSTATUS_SUPERMAN); }
    BOOL IsMagicShieldActive() const { return TestState(USERSTATUS_SHIELD); }
    int GetStamina() const { return m_nStamina; }
    int GetMaxStamina() const { return m_nMaxStamina; }
    BOOL IsMining() const { return m_cmdAction.iType == _COMMAND_MINE; }
    BOOL IsJumping() const {
        return m_cmdAction.iType == _COMMAND_JUMP
            && (m_cmdAction.posTarget.x != m_posMap.x || m_cmdAction.posTarget.y != m_posMap.y);
    }
    BOOL IsGuard() const {
        return m_id >= 400000 && m_id < 500000
            && strstr(m_szName, "Guard") == m_szName;
    }
    BOOL IsPatrol() const {
        return m_id >= 400000 && m_id < 500000
            && strstr(m_szName, "Patrol") == m_szName;
    }
    BOOL HasSyndicate() const { return m_idSyndicate != 0; }

    void GetPos(Position& pos) const { pos = m_posMap; }
    const CCommand& GetCommand() const { return m_cmdAction; }

    void SetCommand(CCommand* cmd);
};
#pragma pack(pop)

static_assert(offsetof(CRole, m_nStatusFlag) == 0x30, "CRole::m_nStatusFlag");
static_assert(offsetof(CRole, m_id)          == 0x68, "CRole::m_id");
static_assert(offsetof(CRole, m_szName)      == 0x94, "CRole::m_szName");
static_assert(offsetof(CRole, m_posMap)      == 0xD8, "CRole::m_posMap");
static_assert(offsetof(CRole, m_posWorld)    == 0xE0, "CRole::m_posWorld");
static_assert(offsetof(CRole, m_posScr)      == 0xE8, "CRole::m_posScr");
static_assert(offsetof(CRole, m_cmdAction)   == 0x188, "CRole::m_cmdAction");
static_assert(offsetof(CRole, m_nMaxHp)      == 0x3D0, "CRole::m_nMaxHp");
static_assert(offsetof(CRole, m_nStamina)     == 0x6E0, "CRole::m_nStamina");
static_assert(offsetof(CRole, m_nMaxStamina)  == 0x6E4, "CRole::m_nMaxStamina");
static_assert(offsetof(CRole, m_idSyndicate)  == 0x714, "CRole::m_idSyndicate");
static_assert(offsetof(CRole, m_nSyndicateRank) == 0x718, "CRole::m_nSyndicateRank");

using PRole = Ref<CRole>;

class CHero; // forward declaration

// =====================================================================
// CRoleMgr — holds hero reference + entity deque
//
// Located at base + 0x4DF588.
// Game uses same MSVC 2022 17.14 as us, so std::deque ABI matches.
// =====================================================================
#pragma pack(push, 1)
class CRoleMgr
{
public:
    CHero*                m_pHero;       // 0x00
private:
    BYTE                  _pad08[0x68];  // 0x08
public:
    std::deque<PRole>     m_deqRole;     // 0x70
};
#pragma pack(pop)

static_assert(offsetof(CRoleMgr, m_pHero)   == 0x00, "CRoleMgr::m_pHero");
static_assert(offsetof(CRoleMgr, m_deqRole) == 0x70, "CRoleMgr::m_deqRole");
