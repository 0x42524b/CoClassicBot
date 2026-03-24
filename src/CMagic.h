#pragma once
#include "base.h"
#include <string>
#include <vector>

// =====================================================================
// CMagic — a learned skill on the hero
//
// The hero stores skills in a std::vector<shared_ptr<CMagic>> at +0x1918.
// Each CMagic object contains a control-block header followed by an
// embedded MagicTypeInfo struct (populated from magictype.json by the
// game at load time).
//
// Layout verified via Ghidra decompilation of:
//   - FUN_140199eb0 (magictype.json loader)
//   - FUN_1401a70b0 (AddMagic — hero skill initializer)
//   - CHero__Init (0x140177710)
//
// Offsets are relative to the CMagic object base (the shared_ptr target).
// =====================================================================
#pragma pack(push, 1)
class CMagic
{
public:
    virtual ~CMagic() = default;    // +0x00  vtable (8 bytes)
    int32_t     m_bEnable;          // +0x08  (BOOL — is the skill usable)
    uint32_t    m_dwExp;            // +0x0C  (current skill experience)

    // ── Embedded MagicTypeInfo (starts at +0x10) ──
    uint32_t    m_idMagicType;      // +0x10  (skill type ID, e.g. 1000 = Thunder)
    uint32_t    m_dwActionSort;     // +0x14
    std::string m_strName;          // +0x18  (skill name, 32 bytes on MSVC x64)
    // +0x38..0x47: Crime, Ground, Multi, Target
    BYTE        _pad38[0x48 - 0x38];
    uint32_t    m_dwLevel;          // +0x48
    uint32_t    m_dwMpCost;         // +0x4C
    uint32_t    m_dwPower;          // +0x50
    uint32_t    m_dwIntoneDuration; // +0x54
    uint32_t    m_dwHitPoint;       // +0x58
    uint32_t    m_dwDuration;       // +0x5C
    uint32_t    m_dwRange;          // +0x60
    uint32_t    m_dwDistance;       // +0x64
    BYTE        _pad68[0x70 - 0x68]; // Status, ProfessionalRequired
    uint32_t    m_dwExpRequired;    // +0x70
    BYTE        _pad74[0x78 - 0x74]; // MonsterLevelRequired
    uint32_t    m_dwXp;             // +0x78  (0=regular, 1=XP skill)
    BYTE        _pad7C[0x98 - 0x7C]; // WeaponSubType, Active/Auto/Floor/Learn/Drop
    uint32_t    m_dwUsePP;          // +0x98  (stamina cost)

    // ── Accessors ──
    const char* GetName()        const { return m_strName.c_str(); }
    uint32_t    GetMagicType()   const { return m_idMagicType; }
    uint32_t    GetLevel()       const { return m_dwLevel; }
    uint32_t    GetMpCost()      const { return m_dwMpCost; }
    uint32_t    GetPower()       const { return m_dwPower; }
    uint32_t    GetExp()         const { return m_dwExp; }
    uint32_t    GetExpRequired() const { return m_dwExpRequired; }
    uint32_t    GetDuration()    const { return m_dwDuration; }
    uint32_t    GetRange()       const { return m_dwRange; }
    uint32_t    GetDistance()    const { return m_dwDistance; }
    uint32_t    GetStaminaCost() const { return m_dwUsePP; }
    bool        IsXpSkill()      const { return m_dwXp == 1; }
    bool        IsEnabled()      const { return m_bEnable != 0; }
};
#pragma pack(pop)

static_assert(offsetof(CMagic, m_bEnable)      == 0x08, "CMagic::m_bEnable");
static_assert(offsetof(CMagic, m_dwExp)         == 0x0C, "CMagic::m_dwExp");
static_assert(offsetof(CMagic, m_idMagicType)   == 0x10, "CMagic::m_idMagicType");
static_assert(offsetof(CMagic, m_strName)        == 0x18, "CMagic::m_strName");
static_assert(offsetof(CMagic, m_dwLevel)        == 0x48, "CMagic::m_dwLevel");
static_assert(offsetof(CMagic, m_dwMpCost)       == 0x4C, "CMagic::m_dwMpCost");
static_assert(offsetof(CMagic, m_dwPower)        == 0x50, "CMagic::m_dwPower");
static_assert(offsetof(CMagic, m_dwExpRequired)  == 0x70, "CMagic::m_dwExpRequired");
static_assert(offsetof(CMagic, m_dwXp)            == 0x78, "CMagic::m_dwXp");
static_assert(offsetof(CMagic, m_dwUsePP)        == 0x98, "CMagic::m_dwUsePP");

using PMagic = Ref<CMagic>;
